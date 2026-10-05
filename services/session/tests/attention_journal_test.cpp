#include "attention_journal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace lapis::session;
using Kind = AttentionJournal::Kind;
using Origin = AttentionJournal::Origin;
constexpr std::size_t max_string_bytes = std::size_t{64} * 1024U;
constexpr std::size_t max_choices = 64;

void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Attention journal check failed at line " +
                                 std::to_string(where.line()));
}

int next_directory = 0;

std::uint32_t crc32(const std::vector<std::uint8_t>& bytes, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}

std::filesystem::path make_directory() {
    auto directory = std::filesystem::temp_directory_path() /
                     ("lapis-att-journal-" + std::to_string(::getpid()) + "-" +
                      std::to_string(++next_directory));
    std::filesystem::create_directories(directory);
    return directory;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

attention::Request request(const std::string& summary) {
    return {.id = std::int64_t{7},
            .thread_id = "thread-1",
            .turn_id = "turn-2",
            .item_id = "item-3",
            .reason = "approval",
            .summary = summary,
            .choices = {"allow", "reject"},
            .priority = 2};
}

// Every field, so no initializer ever relies on a skipped default.
AttentionJournal::Entry asked(std::uint64_t epoch, attention::RequestId id, std::uint64_t revision,
                              std::optional<attention::Request> payload = std::nullopt) {
    return {Kind::asked, 0, epoch, std::move(id), revision, {}, Origin::user, std::move(payload)};
}
AttentionJournal::Entry closed(Kind kind, std::uint64_t epoch, attention::RequestId id,
                               std::uint64_t revision, std::string choice = {},
                               Origin origin = Origin::user) {
    return {kind, 0, epoch, std::move(id), revision, std::move(choice), origin, std::nullopt};
}

void round_trip() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(3, std::int64_t{42}, 5, request("Run the fixture?")));
        journal.append(closed(Kind::delivered, 3, std::int64_t{42}, 5, "allow"));
        journal.append(asked(4, std::string{"tool_9"}, 1));
        journal.append(closed(Kind::resolved, 4, std::string{"tool_9"}, 1, {}, Origin::agent));
        const auto& entries = journal.entries();
        // Append retains only the open working set; the file retains all four
        // records for replay and audit.
        require(entries.empty());
    }
    AttentionJournal reopened(path);
    const auto& entries = reopened.entries();
    require(entries.size() == 4);
    require(entries[0].kind == Kind::asked && entries[0].epoch == 3 &&
            std::get<std::int64_t>(entries[0].id) == 42 && entries[0].revision == 5);
    const auto& decoded_request = entries[0].request;
    require(decoded_request.has_value() && std::get<std::int64_t>(decoded_request->id) == 42);
    require(entries[1].kind == Kind::delivered && entries[1].choice == "allow" &&
            entries[1].origin == Origin::user);
    require(std::get<std::string>(entries[2].id) == "tool_9" && !entries[2].request);
    require(entries[3].kind == Kind::resolved && entries[3].origin == Origin::agent);
    reopened.append(closed(Kind::resolved, 3, std::int64_t{42}, 5));
    require(reopened.entries().empty());
    std::filesystem::remove_all(directory);
}

void torn_tail_is_dropped() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(1, std::int64_t{1}, 1));
        journal.append(closed(Kind::delivered, 1, std::int64_t{1}, 1, "allow"));
    }
    auto bytes = read_bytes(path);
    require(bytes.size() > 20);
    bytes.resize(bytes.size() - 6); // cut into the final record's body
    write_bytes(path, bytes);
    {
        AttentionJournal reopened(path);
        require(reopened.entries().size() == 1);
        require(reopened.entries().front().kind == Kind::asked);
        reopened.append(closed(Kind::resolved, 1, std::int64_t{1}, 1, {}, Origin::agent));
        require(reopened.entries().empty());
    }
    AttentionJournal verified(path);
    require(verified.entries().size() == 2 && verified.entries().back().seq == 2);
    std::filesystem::remove_all(directory);
}

void torn_final_record_with_intact_frame_is_dropped() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(1, std::int64_t{1}, 1));
        journal.append(closed(Kind::resolved, 1, std::int64_t{1}, 1, {}, Origin::agent));
    }
    auto bytes = read_bytes(path);
    bytes.back() ^= 0xFFU; // damage the final record's checksum
    write_bytes(path, bytes);
    AttentionJournal reopened(path);
    require(reopened.entries().size() == 1);
    std::filesystem::remove_all(directory);
}

void mid_file_corruption_throws() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(1, std::int64_t{1}, 1));
        journal.append(closed(Kind::resolved, 1, std::int64_t{1}, 1, {}, Origin::agent));
        journal.append(asked(1, std::int64_t{2}, 1));
    }
    auto bytes = read_bytes(path);
    bytes[20] ^= 0xFFU; // inside the first record
    write_bytes(path, bytes);
    bool threw = false;
    try {
        AttentionJournal reopened(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw);
    std::filesystem::remove_all(directory);
}

void newer_format_refused() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(1, std::int64_t{1}, 1));
    }
    auto bytes = read_bytes(path);
    ++bytes[8]; // header version
    write_bytes(path, bytes);
    bool threw = false;
    try {
        AttentionJournal reopened(path);
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("newer") != std::string::npos;
    }
    require(threw);
    std::filesystem::remove_all(directory);
}

void torn_header_is_rewritten() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    write_bytes(path, {std::uint8_t{'L'}, std::uint8_t{'A'}, std::uint8_t{'P'}});
    {
        AttentionJournal journal(path);
        require(journal.entries().empty());
        journal.append(asked(1, std::int64_t{1}, 1));
    }
    AttentionJournal reopened(path);
    require(reopened.entries().size() == 1);
    std::filesystem::remove_all(directory);
}

void second_writer_rejected() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    AttentionJournal journal(path);
    bool threw = false;
    try {
        AttentionJournal second(path);
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("already owned") != std::string::npos;
    }
    require(threw);
    std::filesystem::remove_all(directory);
}

void recovery_classification() {
    const auto old_ask = asked(1, std::int64_t{1}, 4);
    const auto new_ask = asked(2, std::int64_t{1}, 5);
    const auto same_id_wrong_revision = closed(Kind::decided, 2, std::int64_t{1}, 4, "allow");
    const auto same_revision_wrong_epoch =
        closed(Kind::resolved, 1, std::int64_t{1}, 5, {}, Origin::agent);
    auto open =
        open_questions({old_ask, new_ask, same_id_wrong_revision, same_revision_wrong_epoch});
    require(open.size() == 1 && open.front().epoch == 2 && open.front().revision == 5);
    open = open_questions({new_ask, closed(Kind::decided, 2, std::int64_t{1}, 5, "reject")});
    require(open.size() == 1);
    open = open_questions({new_ask, closed(Kind::delivered, 2, std::int64_t{1}, 5, "reject")});
    require(open.empty());
    const auto unknown = outcome_unknown_for(new_ask);
    require(unknown.kind == Kind::decided && unknown.origin == Origin::outcome_unknown &&
            unknown.epoch == 2 && unknown.revision == 5 && unknown.choice.empty() &&
            !unknown.request.has_value());
    auto orphan_intent = closed(Kind::decided, 3, std::int64_t{1}, 7, "allow");
    orphan_intent.seq = 1;
    open = open_questions({orphan_intent});
    require(open.size() == 1 &&
            outcome_unknown_for(open.front()).origin == Origin::outcome_unknown);
}

void maximum_record_replays_without_a_short_read() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    const std::string large(max_string_bytes, 'x');
    attention::Request payload{
        .id = std::string(max_string_bytes, 'i'),
        .thread_id = large,
        .turn_id = large,
        .item_id = large,
        .reason = large,
        .summary = large,
        .choices = std::vector<std::string>(max_choices, large),
        .priority = 3,
    };
    {
        AttentionJournal journal{path, std::uint64_t{16} * 1024U * 1024U};
        journal.append(asked(1, payload.id, 1, payload));
    }
    AttentionJournal reopened{path, std::uint64_t{16} * 1024U * 1024U};
    require(reopened.entries().size() == 1 && reopened.entries().front().request == payload);
    std::filesystem::remove_all(directory);
}

void invalid_final_field_is_dropped() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(1, std::int64_t{1}, 1));
    }
    auto bytes = read_bytes(path);
    constexpr std::size_t record_begin = 10; // magic, version and newline.
    bytes[record_begin] = static_cast<std::uint8_t>(Kind::delivered) + 1;
    const std::uint32_t sum = crc32(bytes, bytes.size() - 4);
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[bytes.size() - 4 + shift / 8] = static_cast<std::uint8_t>(sum >> shift);
    write_bytes(path, bytes);
    AttentionJournal reopened(path);
    require(reopened.entries().empty());
    std::filesystem::remove_all(directory);
}

void oversized_journal_rotates() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    auto sibling = path;
    sibling += ".1";
    {
        AttentionJournal journal(path, 64); // rotate on every eligible reopen
        journal.append(asked(1, std::int64_t{1}, 1));
        journal.append(closed(Kind::resolved, 1, std::int64_t{1}, 1, {}, Origin::agent));
    }
    {
        AttentionJournal journal(path, 64);
        require(journal.entries().empty());
        require(std::filesystem::exists(sibling));
        {
            AttentionJournal previous(sibling);
            require(previous.entries().size() == 2);
        }
    }
    // An open question blocks rotation so recovery stays in the asking log.
    {
        // 64 bytes: an empty journal (header only) stays; two records exceed it.
        AttentionJournal held(path, 64);
        held.append(asked(2, std::int64_t{2}, 1));
    }
    {
        AttentionJournal again(path, 64);
        require(again.entries().size() == 1);
        require(open_questions(again.entries()).size() == 1);
    }
    // Closing the question makes the oversized generation eligible again; the
    // fresh active file remains bounded instead of growing without a reopen.
    {
        AttentionJournal held(path, 64);
        held.append(closed(Kind::resolved, 2, std::int64_t{2}, 1, {}, Origin::agent));
        require(held.entries().empty());
    }
    {
        AttentionJournal rotated(path, 64);
        require(rotated.entries().empty());
    }
    {
        AttentionJournal archive(sibling);
        require(archive.entries().size() == 2);
    }
    std::filesystem::remove_all(directory);
}

void failed_preappend_rotation_does_not_commit_record() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    auto sibling = path;
    sibling += ".1";
    AttentionJournal journal(path, 64);
    journal.append(asked(1, std::int64_t{1}, 1));
    journal.append(closed(Kind::resolved, 1, std::int64_t{1}, 1, {}, Origin::agent));
    const auto size_before = std::filesystem::file_size(path);
    require(size_before > 64);
    require(std::filesystem::create_directory(sibling));
    bool refused = false;
    try {
        journal.append(asked(2, std::int64_t{2}, 1));
    } catch (const std::runtime_error& error) {
        refused = std::string_view{error.what()}.find("Cannot rotate attention journal") !=
                  std::string_view::npos;
    }
    require(refused);
    require(std::filesystem::file_size(path) == size_before);
    std::filesystem::remove_all(directory);
}
} // namespace

int main() {
    const struct {
        const char* name;
        void (*run)();
    } tests[] = {
        {"round-trip", round_trip},
        {"torn-tail", torn_tail_is_dropped},
        {"torn-final-checksum", torn_final_record_with_intact_frame_is_dropped},
        {"mid-file-corruption", mid_file_corruption_throws},
        {"newer-format", newer_format_refused},
        {"torn-header", torn_header_is_rewritten},
        {"single-writer", second_writer_rejected},
        {"recovery-classification", recovery_classification},
        {"maximum-record", maximum_record_replays_without_a_short_read},
        {"rotation", oversized_journal_rotates},
        {"preappend-rotation-failure", failed_preappend_rotation_does_not_commit_record},
        {"invalid-final-field", invalid_final_field_is_dropped},
    };
    try {
        for (const auto& test : tests) {
            std::cout << test.name << "... " << std::flush;
            test.run();
            std::cout << "ok\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
