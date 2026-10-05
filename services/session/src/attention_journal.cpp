#include "attention_journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
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
// One valid record cannot exceed these maximum-size fields. Replay reads at
// most one record of this size at a time instead of loading a whole journal.
constexpr std::size_t max_record_bytes = (max_choices + 6U) * (max_string_bytes + 4U) + 64U;
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
    void invalidate() { invalid_ = true; }
    [[nodiscard]] bool invalid() const { return invalid_; }
    bool bytes(std::string& value) {
        std::uint32_t size = 0;
        if (!u32(size))
            return false;
        if (static_cast<std::size_t>(end_ - at_) < size)
            return starved_ = true, false;
        if (size > max_string_bytes)
            invalidate();
        value.assign(reinterpret_cast<const char*>(at_), size);
        at_ += size;
        return true;
    }
    bool skip_bytes(std::uint32_t size) {
        if (static_cast<std::size_t>(end_ - at_) < size)
            return starved_ = true, false;
        if (size > max_string_bytes)
            invalidate();
        at_ += size;
        return true;
    }
    [[nodiscard]] const std::uint8_t* at() const { return at_; }

  private:
    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool starved_{};
    bool invalid_{};
};

bool read_request(Reader& reader, const attention::RequestId& id, attention::Request& request) {
    if (!reader.bytes(request.thread_id) || !reader.bytes(request.turn_id) ||
        !reader.bytes(request.item_id) || !reader.bytes(request.reason) ||
        !reader.bytes(request.summary))
        return false;
    std::uint32_t count = 0;
    if (!reader.u32(count))
        return false;
    if (count > max_choices) {
        reader.invalidate();
        request.choices.resize(max_choices);
    } else
        request.choices.resize(count);
    for (auto& choice : request.choices)
        if (!reader.bytes(choice))
            return false;
    std::uint32_t length = 0;
    for (std::uint32_t extra = max_choices; reader.invalid() && extra < count; ++extra)
        if (!reader.u32(length) || !reader.skip_bytes(length))
            return false;
    std::uint8_t priority = 0;
    if (!reader.u8(priority))
        return false;
    request.priority = priority;
    request.id = id;
    return true;
}

struct Decoded {
    AttentionJournal::Entry entry;
    bool valid{};
};

Decoded decode(Reader& reader) {
    Decoded result{.entry = {}, .valid = true};
    auto& entry = result.entry;
    std::uint8_t kind = 0, origin = 0, id_tag = 0, has_request = 0;
    if (!reader.u8(kind) || !reader.u8(origin) || !reader.u64(entry.seq) ||
        !reader.u64(entry.epoch) || !reader.u64(entry.revision) || !reader.u8(id_tag))
        return result;
    if (kind > static_cast<std::uint8_t>(AttentionJournal::Kind::delivered) ||
        origin > static_cast<std::uint8_t>(AttentionJournal::Origin::outcome_unknown) ||
        id_tag > 1) {
        reader.invalidate();
        result.valid = false;
    }
    entry.kind = static_cast<AttentionJournal::Kind>(kind);
    entry.origin = static_cast<AttentionJournal::Origin>(origin);
    if (id_tag == 0) {
        std::uint64_t number = 0;
        if (!reader.u64(number))
            return result;
        entry.id = static_cast<std::int64_t>(number);
    } else {
        std::string text;
        if (!reader.bytes(text))
            return result;
        entry.id = std::move(text);
    }
    if (!reader.bytes(entry.choice) || !reader.u8(has_request))
        return result;
    if (has_request > 1) {
        reader.invalidate();
        result.valid = false;
    }
    if (has_request == 1) {
        attention::Request request;
        if (!read_request(reader, entry.id, request))
            return result;
        entry.request = std::move(request);
    }
    return result;
}

std::filesystem::path rotated_path(const std::filesystem::path& path) {
    std::filesystem::path sibling = path;
    sibling += ".1";
    return sibling;
}

void sync_directory(const std::filesystem::path& file) {
    posix::UniqueFd directory{
        ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (!directory)
        throw std::runtime_error(std::string("Cannot open attention journal directory: ") +
                                 errno_message(errno));
    if (::fsync(directory.get()) != 0)
        throw std::runtime_error(std::string("Cannot flush attention journal directory: ") +
                                 errno_message(errno));
}

void read_at(int descriptor, std::uint8_t* data, std::size_t size, std::uint64_t offset) {
    std::size_t filled = 0;
    while (filled < size) {
        const auto read_bytes =
            ::pread(descriptor, data + filled, size - filled, static_cast<off_t>(offset + filled));
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
}

// Full write loop at an explicit offset; a short write is failure, so append()
// either made the whole record durable or durably removes the partial tail.
// The file position is meaningless across opens, so nothing relies on it.
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

// A checksum-valid partial tail would replay as a commit, so failed durability
// removes it durably. If that rollback itself fails, the original failure is
// still named first; the decision gate must treat the journal unavailable.
[[noreturn]] void rollback_failed(int descriptor, std::uint64_t size, std::string original) {
    if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0 || ::fsync(descriptor) != 0) {
        original += "; rollback also failed: ";
        original += errno_message();
    }
    throw std::runtime_error(original);
}

enum class RecordStatus : std::uint8_t { accepted, torn, malformed, corrupt };

struct ParsedRecord {
    AttentionJournal::Entry entry;
    std::size_t total{};
    RecordStatus status{RecordStatus::accepted};
};

ParsedRecord parse_record(const std::vector<std::uint8_t>& bytes, std::size_t loaded,
                          bool final_input, std::uint64_t expected_seq) {
    Reader reader(bytes.data(), bytes.data() + bytes.size());
    Decoded decoded = decode(reader);
    decoded.valid = decoded.valid && !reader.invalid();
    if (reader.starved())
        return {std::move(decoded.entry), 0,
                final_input ? RecordStatus::torn : RecordStatus::corrupt};
    std::uint32_t stored_checksum = 0;
    if (!reader.u32(stored_checksum))
        return {std::move(decoded.entry), 0,
                final_input ? RecordStatus::torn : RecordStatus::corrupt};
    const std::size_t total = static_cast<std::size_t>(reader.at() - bytes.data());
    const bool at_end = reader.at() == bytes.data() + bytes.size();
    if (checksum(bytes.data(), total - 4) != stored_checksum)
        return {std::move(decoded.entry), total,
                at_end && final_input ? RecordStatus::torn : RecordStatus::corrupt};
    if (!decoded.valid)
        return {std::move(decoded.entry), total,
                total == loaded && final_input ? RecordStatus::malformed : RecordStatus::corrupt};
    if (decoded.entry.seq != expected_seq)
        return {std::move(decoded.entry), total, RecordStatus::corrupt};
    return {std::move(decoded.entry), total, RecordStatus::accepted};
}
} // namespace

AttentionJournal::AttentionJournal(std::filesystem::path file, std::uint64_t rotate_bytes)
    : path_(std::move(file)),
      rotate_bytes_(rotate_bytes == 0 ? default_rotate_bytes : rotate_bytes) {
    // Lease with inode re-verification: flock alone can be held against a file
    // that was unlinked and replaced under us, and a wedged writer's lock is
    // never stolen by age (ported from dsh's persistence lease).
    for (int attempt = 0; attempt < lease_attempts; ++attempt) {
        const int leased = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        descriptor_.reset(leased);
        if (!descriptor_)
            throw std::runtime_error(std::string("Cannot open attention journal: ") +
                                     errno_message());
        if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            descriptor_.reset();
            if (error == EWOULDBLOCK)
                throw std::runtime_error("Attention journal is already owned");
            throw std::runtime_error(std::string("Cannot lock attention journal: ") +
                                     errno_message(error));
        }
        struct stat locked{};
        struct stat current{};
        if (::fstat(descriptor_.get(), &locked) == 0 && ::stat(path_.c_str(), &current) == 0 &&
            locked.st_dev == current.st_dev && locked.st_ino == current.st_ino)
            break;
        descriptor_.reset();
    }
    if (!descriptor_)
        throw std::runtime_error("Attention journal kept changing under the lease");
    struct stat leased{};
    if (::fstat(descriptor_.get(), &leased) != 0)
        throw std::runtime_error("Cannot size attention journal");
    size_ = static_cast<std::uint64_t>(leased.st_size);
    replay();
    if (size_ > rotate_bytes_ && open_questions(entries_).empty()) {
        rotate_locked();
    }
}

AttentionJournal::~AttentionJournal() {}

void AttentionJournal::open_new() {
    // Rotation path only: the previous lease is still held while the fresh
    // file is created, so a would-be second writer never finds it unlocked.
    posix::UniqueFd opened{::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
    if (!opened)
        throw std::runtime_error(std::string("Cannot create attention journal: ") +
                                 errno_message());
    if (::flock(opened.get(), LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        throw std::runtime_error(std::string("Cannot lock fresh attention journal: ") +
                                 errno_message(error));
    }
    struct stat info{};
    if (::fstat(opened.get(), &info) != 0)
        throw std::runtime_error("Cannot size attention journal");
    descriptor_ = std::move(opened);
    size_ = static_cast<std::uint64_t>(info.st_size);
}

void AttentionJournal::rotate_locked() {
    // Rotate with the old lock still held and create the fresh file before
    // releasing it, so the unowned-name window is as small as the rename
    // itself. A second service is already excluded by the endpoint lock.
    const auto sibling = rotated_path(path_);
    if (::rename(path_.c_str(), sibling.c_str()) != 0) {
        const int error = errno;
        throw std::runtime_error(std::string("Cannot rotate attention journal: ") +
                                 errno_message(error));
    }
    sync_directory(path_);
    posix::UniqueFd previous{descriptor_.release()};
    open_new();
    ensure_header();
    entries_.clear();
    next_seq_ = 1;
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
        write_all_at(descriptor_.get(), header.data(), header.size(), 0);
        if (::fsync(descriptor_.get()) != 0)
            throw std::runtime_error("Cannot initialize attention journal");
        size_ = header.size();
        sync_directory(path_);
        return;
    }
    std::array<std::uint8_t, header_bytes> stored{};
    if (::pread(descriptor_.get(), stored.data(), stored.size(), 0) !=
        static_cast<ssize_t>(header_bytes))
        throw corrupt("unreadable header");
    if (std::string_view(reinterpret_cast<const char*>(stored.data()), magic.size()) != magic)
        throw corrupt("unrecognized header");
    if (stored[magic.size()] == 0)
        throw corrupt("unsupported journal format");
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
    std::uint64_t expected_seq = 1;
    std::uint64_t record_offset = header_bytes;
    for (;;) {
        if (record_offset == size_)
            break;
        const auto remaining = static_cast<std::size_t>(size_ - record_offset);
        const auto loaded = std::min(remaining, max_record_bytes);
        std::vector<std::uint8_t> bytes(loaded);
        read_at(descriptor_.get(), bytes.data(), bytes.size(), record_offset);
        const bool final_input = record_offset + loaded == size_;
        auto parsed = parse_record(bytes, loaded, final_input, expected_seq);
        if (parsed.status == RecordStatus::torn || parsed.status == RecordStatus::malformed) {
            // A final complete-looking but invalid checksum or field is an
            // interrupted append; mid-file damage is not explained by a tear.
            truncate_to(record_offset);
            break;
        }
        if (parsed.status == RecordStatus::corrupt)
            throw corrupt("checksum, sequence, or field mismatch");
        ++expected_seq;
        entries_.push_back(std::move(parsed.entry));
        record_offset += parsed.total;
    }
    next_seq_ = expected_seq;
}

void AttentionJournal::truncate_to(std::uint64_t bytes) {
    if (::ftruncate(descriptor_.get(), static_cast<off_t>(bytes)) != 0 ||
        ::fsync(descriptor_.get()) != 0)
        throw std::runtime_error("Cannot trim torn attention journal tail");
    size_ = bytes;
}

void AttentionJournal::append(Entry entry) {
    if (size_ > rotate_bytes_ && open_questions(entries_).empty())
        rotate_locked();
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
    try {
        write_all_at(descriptor_.get(), record.data(), record.size(), size_);
    } catch (const std::exception& error) {
        rollback_failed(descriptor_.get(), size_, error.what());
    }
    if (::fsync(descriptor_.get()) != 0) {
        const int error = errno;
        rollback_failed(descriptor_.get(), size_,
                        "Attention journal record is not durable: " + errno_message(error));
    }
    size_ += record.size();
    entries_.push_back(std::move(entry));
    ++next_seq_;
    entries_ = open_questions(entries_);
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
        // A user `decided` record is durable intent, not proof of delivery.
        // It stays open until `delivered` or a compensating closure; older
        // non-user decisions remain closures for compatibility.
        if (entry.kind == AttentionJournal::Kind::decided &&
            entry.origin == AttentionJournal::Origin::user)
            continue;
        for (std::size_t index = 0; index < open.size(); ++index) {
            if (open[index].id == entry.id && open[index].epoch == entry.epoch &&
                open[index].revision == entry.revision) {
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
