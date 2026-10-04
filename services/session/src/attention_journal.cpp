#include "attention_journal.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace lapis::session {
namespace {
constexpr std::string_view magic{"LAPISATT"};
constexpr std::uint8_t format_version = 1;
constexpr std::size_t header_bytes = magic.size() + 2; // magic, version, newline
constexpr std::size_t max_string_bytes = std::size_t{64} * 1024U;
constexpr std::size_t max_choices = 64;
constexpr int lease_attempts = 3;

// std::strerror is not thread safe; the error_code machinery is the
// project's errno-to-message convention.
std::string errno_message(int value = errno) {
    return std::error_code(value, std::generic_category()).message();
}

std::runtime_error corrupt(const char* why) {
    return std::runtime_error(std::string("Corrupt attention journal: ") + why);
}

// Reflected CRC-32 (IEEE). Records are written and verified with this routine,
// so any accepted record round-trips byte-identically.
std::uint32_t checksum(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}
void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}
void put_bytes(std::vector<std::uint8_t>& out, std::string_view value) {
    if (value.size() > max_string_bytes)
        throw std::invalid_argument("Attention journal field too large");
    put_u32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
void put_id(std::vector<std::uint8_t>& out, const attention::RequestId& id) {
    if (const auto* number = std::get_if<std::int64_t>(&id)) {
        put_u8(out, 0);
        put_u64(out, static_cast<std::uint64_t>(*number));
        return;
    }
    put_u8(out, 1);
    put_bytes(out, std::get<std::string>(id));
}
void put_request(std::vector<std::uint8_t>& out, const attention::Request& request) {
    put_bytes(out, request.thread_id);
    put_bytes(out, request.turn_id);
    put_bytes(out, request.item_id);
    put_bytes(out, request.reason);
    put_bytes(out, request.summary);
    if (request.choices.size() > max_choices)
        throw std::invalid_argument("Attention journal request has too many choices");
    put_u32(out, static_cast<std::uint32_t>(request.choices.size()));
    for (const auto& choice : request.choices)
        put_bytes(out, choice);
    put_u8(out, request.priority);
}

// Reads little-endian fields. A shortfall against the remaining file bytes
// marks the record torn (a crash mid-append); only values that fit but are
// invalid are corruption.
class Reader {
  public:
    Reader(const std::uint8_t* begin, const std::uint8_t* end) : at_(begin), end_(end) {}
    [[nodiscard]] bool starved() const { return starved_; }
    bool u8(std::uint8_t& value) {
        if (end_ - at_ < 1)
            return starved_ = true, false;
        value = *at_++;
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (end_ - at_ < 4)
            return starved_ = true, false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(*at_++) << shift;
        return true;
    }
    bool u64(std::uint64_t& value) {
        if (end_ - at_ < 8)
            return starved_ = true, false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(*at_++) << shift;
        return true;
    }
    bool bytes(std::string& value) {
        std::uint32_t size = 0;
        if (!u32(size))
            return false;
        if (static_cast<std::size_t>(end_ - at_) < size)
            return starved_ = true, false;
        if (size > max_string_bytes)
            throw corrupt("field length out of range");
        value.assign(reinterpret_cast<const char*>(at_), size);
        at_ += size;
        return true;
    }
    [[nodiscard]] const std::uint8_t* at() const { return at_; }

  private:
    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool starved_{};
};

bool read_request(Reader& reader, attention::Request& request) {
    if (!reader.bytes(request.thread_id) || !reader.bytes(request.turn_id) ||
        !reader.bytes(request.item_id) || !reader.bytes(request.reason) ||
        !reader.bytes(request.summary))
        return false;
    std::uint32_t count = 0;
    if (!reader.u32(count))
        return false;
    if (count > max_choices)
        throw corrupt("too many choices");
    request.choices.resize(count);
    for (auto& choice : request.choices)
        if (!reader.bytes(choice))
            return false;
    std::uint8_t priority = 0;
    if (!reader.u8(priority))
        return false;
    request.priority = priority;
    return true;
}

AttentionJournal::Entry decode(Reader& reader) {
    AttentionJournal::Entry entry;
    std::uint8_t kind = 0, origin = 0, id_tag = 0, has_request = 0;
    if (!reader.u8(kind) || !reader.u8(origin) || !reader.u64(entry.seq) ||
        !reader.u64(entry.epoch) || !reader.u64(entry.revision) || !reader.u8(id_tag))
        return entry;
    if (kind > static_cast<std::uint8_t>(AttentionJournal::Kind::resolved) ||
        origin > static_cast<std::uint8_t>(AttentionJournal::Origin::outcome_unknown) || id_tag > 1)
        throw corrupt("invalid record field");
    entry.kind = static_cast<AttentionJournal::Kind>(kind);
    entry.origin = static_cast<AttentionJournal::Origin>(origin);
    if (id_tag == 0) {
        std::uint64_t number = 0;
        if (!reader.u64(number))
            return entry;
        entry.id = static_cast<std::int64_t>(number);
    } else {
        std::string text;
        if (!reader.bytes(text))
            return entry;
        entry.id = std::move(text);
    }
    if (!reader.bytes(entry.choice) || !reader.u8(has_request))
        return entry;
    if (has_request > 1)
        throw corrupt("invalid request marker");
    if (has_request == 1) {
        attention::Request request;
        if (!read_request(reader, request))
            return entry;
        entry.request = std::move(request);
    }
    return entry;
}

std::filesystem::path rotated_path(const std::filesystem::path& path) {
    std::filesystem::path sibling = path;
    sibling += ".1";
    return sibling;
}

void sync_directory(const std::filesystem::path& file) {
    const int directory = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        static_cast<void>(::fsync(directory));
        static_cast<void>(::close(directory));
    }
}

// Full write loop at an explicit offset; a short write is failure, so
// append() either made the whole record durable or leaves no complete-looking
// record behind. The file position is meaningless across opens, so nothing
// relies on it.
void write_all_at(int descriptor, const std::uint8_t* data, std::size_t size,
                  std::uint64_t offset) {
    while (size > 0) {
        const auto written = ::pwrite(descriptor, data, size, static_cast<off_t>(offset));
        if (written < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string("Attention journal write failed: ") +
                                     errno_message());
        }
        data += static_cast<std::size_t>(written);
        size -= static_cast<std::size_t>(written);
        offset += static_cast<std::uint64_t>(written);
    }
}
} // namespace

AttentionJournal::AttentionJournal(std::filesystem::path file, std::uint64_t rotate_bytes)
    : path_(std::move(file)),
      rotate_bytes_(rotate_bytes == 0 ? default_rotate_bytes : rotate_bytes) {
    // Lease with inode re-verification: flock alone can be held against a file
    // that was unlinked and replaced under us, and a wedged writer's lock is
    // never stolen by age (ported from dsh's persistence lease).
    for (int attempt = 0; attempt < lease_attempts; ++attempt) {
        descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor_ < 0)
            throw std::runtime_error(std::string("Cannot open attention journal: ") +
                                     errno_message());
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            if (error == EWOULDBLOCK)
                throw std::runtime_error("Attention journal is already owned");
            throw std::runtime_error(std::string("Cannot lock attention journal: ") +
                                     errno_message(error));
        }
        struct stat locked{};
        struct stat current{};
        if (::fstat(descriptor_, &locked) == 0 && ::stat(path_.c_str(), &current) == 0 &&
            locked.st_dev == current.st_dev && locked.st_ino == current.st_ino)
            break;
        ::close(descriptor_);
        descriptor_ = -1;
    }
    if (descriptor_ < 0)
        throw std::runtime_error("Attention journal kept changing under the lease");
    struct stat leased{};
    if (::fstat(descriptor_, &leased) != 0)
        throw std::runtime_error("Cannot size attention journal");
    size_ = static_cast<std::uint64_t>(leased.st_size);
    replay();
    if (size_ > rotate_bytes_ && open_questions(entries_).empty()) {
        // Rotate with the old lock still held and create the fresh file before
        // releasing it, so the unowned-name window is as small as the rename
        // itself. A second service is already excluded by the endpoint lock.
        const auto sibling = rotated_path(path_);
        if (::rename(path_.c_str(), sibling.c_str()) != 0)
            throw std::runtime_error(std::string("Cannot rotate attention journal: ") +
                                     errno_message());
        sync_directory(path_);
        const int previous = descriptor_;
        descriptor_ = -1;
        open_new();
        ensure_header();
        entries_.clear();
        next_seq_ = 1;
        ::close(previous);
    }
}

AttentionJournal::~AttentionJournal() {
    if (descriptor_ >= 0)
        ::close(descriptor_);
}

void AttentionJournal::open_new() {
    // Rotation path only: the previous lease is still held while the fresh
    // file is created, so a would-be second writer never finds it unlocked.
    descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor_ < 0)
        throw std::runtime_error(std::string("Cannot create attention journal: ") +
                                 errno_message());
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error(std::string("Cannot lock fresh attention journal: ") +
                                 errno_message(error));
    }
    struct stat info{};
    if (::fstat(descriptor_, &info) != 0)
        throw std::runtime_error("Cannot size attention journal");
    size_ = static_cast<std::uint64_t>(info.st_size);
}

void AttentionJournal::ensure_header() {
    if (size_ > 0 && size_ < header_bytes) {
        // A header torn by a crash during creation is rewritten, not fatal.
        truncate_to(0);
    }
    if (size_ == 0) {
        std::vector<std::uint8_t> header;
        header.insert(header.end(), magic.begin(), magic.end());
        header.push_back(format_version);
        header.push_back('\n');
        write_all_at(descriptor_, header.data(), header.size(), 0);
        if (::fsync(descriptor_) != 0)
            throw std::runtime_error("Cannot initialize attention journal");
        size_ = header.size();
        sync_directory(path_);
        return;
    }
    std::array<std::uint8_t, header_bytes> stored{};
    if (::pread(descriptor_, stored.data(), stored.size(), 0) != static_cast<ssize_t>(header_bytes))
        throw corrupt("unreadable header");
    if (std::string_view(reinterpret_cast<const char*>(stored.data()), magic.size()) != magic)
        throw corrupt("unrecognized header");
    if (stored[magic.size()] > format_version)
        throw std::runtime_error(
            "Attention journal uses a newer format; upgrade lapis before resuming");
    if (stored[header_bytes - 1] != '\n')
        throw corrupt("damaged header");
}

void AttentionJournal::replay() {
    ensure_header();
    if (size_ == header_bytes)
        return;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size_));
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const auto read_bytes = ::pread(descriptor_, bytes.data() + filled, bytes.size() - filled,
                                        static_cast<off_t>(filled));
        if (read_bytes < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string("Cannot read attention journal: ") +
                                     errno_message());
        }
        if (read_bytes == 0)
            throw corrupt("file shrank while leased");
        filled += static_cast<std::size_t>(read_bytes);
    }
    Reader reader(bytes.data() + header_bytes, bytes.data() + bytes.size());
    std::uint64_t expected_seq = 1;
    for (;;) {
        const auto* record_begin = reader.at();
        if (record_begin == bytes.data() + bytes.size())
            break;
        Entry entry = decode(reader);
        if (reader.starved()) {
            // The append was interrupted before the record was complete, so
            // nothing from here on can be valid.
            truncate_to(static_cast<std::uint64_t>(record_begin - bytes.data()));
            break;
        }
        std::uint32_t stored_checksum = 0;
        if (!reader.u32(stored_checksum)) {
            truncate_to(static_cast<std::uint64_t>(record_begin - bytes.data()));
            break;
        }
        const std::size_t total = static_cast<std::size_t>(reader.at() - record_begin);
        if (checksum(record_begin, total - 4) != stored_checksum) {
            if (reader.at() == bytes.data() + bytes.size()) {
                // A final record failing its checksum is an interrupted append
                // that nonetheless spanned the whole tail; drop it. The same
                // failure before later records is damage no tear explains.
                truncate_to(static_cast<std::uint64_t>(record_begin - bytes.data()));
                break;
            }
            throw corrupt("checksum mismatch");
        }
        if (entry.seq != expected_seq)
            throw corrupt("sequence gap");
        ++expected_seq;
        entries_.push_back(std::move(entry));
    }
    next_seq_ = expected_seq;
}

void AttentionJournal::truncate_to(std::uint64_t bytes) {
    if (::ftruncate(descriptor_, static_cast<off_t>(bytes)) != 0 || ::fsync(descriptor_) != 0)
        throw std::runtime_error("Cannot trim torn attention journal tail");
    size_ = bytes;
}

void AttentionJournal::append(Entry entry) {
    entry.seq = next_seq_;
    std::vector<std::uint8_t> record;
    record.reserve(256);
    put_u8(record, static_cast<std::uint8_t>(entry.kind));
    put_u8(record, static_cast<std::uint8_t>(entry.origin));
    put_u64(record, entry.seq);
    put_u64(record, entry.epoch);
    put_u64(record, entry.revision);
    put_id(record, entry.id);
    put_bytes(record, entry.choice);
    if (entry.request) {
        put_u8(record, 1);
        put_request(record, *entry.request);
    } else {
        put_u8(record, 0);
    }
    put_u32(record, checksum(record.data(), record.size()));
    write_all_at(descriptor_, record.data(), record.size(), size_);
    if (::fsync(descriptor_) != 0) {
        // The bytes may or may not reach the disk later; do not claim them.
        static_cast<void>(::ftruncate(descriptor_, static_cast<off_t>(size_)));
        throw std::runtime_error("Attention journal record is not durable");
    }
    size_ += record.size();
    entries_.push_back(std::move(entry));
    ++next_seq_;
}

std::vector<AttentionJournal::Entry>
open_questions(const std::vector<AttentionJournal::Entry>& entries) {
    std::vector<AttentionJournal::Entry> open;
    for (const auto& entry : entries) {
        if (entry.kind == AttentionJournal::Kind::asked) {
            // A newer source derivation with the same identity supersedes the
            // older ask; a stale decision must not close the current question.
            for (std::size_t index = 0; index < open.size(); ++index) {
                if (open[index].id == entry.id) {
                    open.erase(open.begin() + static_cast<std::ptrdiff_t>(index));
                    break;
                }
            }
            open.push_back(entry);
            continue;
        }
        for (std::size_t index = 0; index < open.size(); ++index) {
            if (open[index].id == entry.id) {
                open.erase(open.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
        }
    }
    return open;
}

AttentionJournal::Entry outcome_unknown_for(const AttentionJournal::Entry& asked) {
    return {.kind = AttentionJournal::Kind::decided,
            .seq = 0,
            .epoch = asked.epoch,
            .id = asked.id,
            .revision = asked.revision,
            .choice = {},
            .origin = AttentionJournal::Origin::outcome_unknown,
            .request = std::nullopt};
}
} // namespace lapis::session
