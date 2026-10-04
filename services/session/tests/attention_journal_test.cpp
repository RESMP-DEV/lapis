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

void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Attention journal check failed at line " +
                                 std::to_string(where.line()));
}

int next_directory = 0;

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
        journal.append(closed(Kind::decided, 3, std::int64_t{42}, 5, "allow"));
        journal.append(asked(4, std::string{"tool_9"}, 1));
        journal.append(closed(Kind::resolved, 4, std::string{"tool_9"}, 1, {}, Origin::agent));
        const auto& entries = journal.entries();
        require(entries.size() == 4);
        require(entries[0].seq == 1 && entries[3].seq == 4);
        const std::optional<attention::Request> decoded_request = entries[0].request;
        if (!decoded_request)
            throw std::runtime_error("Decoded journal entry lost its request");
        require(decoded_request->summary == "Run the fixture?" &&
                decoded_request->choices.size() == 2);
    }
    AttentionJournal reopened(path);
    const auto& entries = reopened.entries();
    require(entries.size() == 4);
    require(entries[0].kind == Kind::asked && entries[0].epoch == 3 &&
            std::get<std::int64_t>(entries[0].id) == 42 && entries[0].revision == 5);
    require(entries[1].kind == Kind::decided && entries[1].choice == "allow" &&
            entries[1].origin == Origin::user);
    require(std::get<std::string>(entries[2].id) == "tool_9" && !entries[2].request);
    require(entries[3].kind == Kind::resolved && entries[3].origin == Origin::agent);
    reopened.append(closed(Kind::resolved, 3, std::int64_t{42}, 5));
    require(reopened.entries().back().seq == 5);
    std::filesystem::remove_all(directory);
}

void torn_tail_is_dropped() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    {
        AttentionJournal journal(path);
        journal.append(asked(1, std::int64_t{1}, 1));
        journal.append(closed(Kind::decided, 1, std::int64_t{1}, 1, "allow"));
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
        require(reopened.entries().back().seq == 2);
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
    const auto asked_a = asked(1, std::int64_t{1}, 2);
    const auto asked_b = asked(1, std::string{"b"}, 1);
    const auto decided_a = closed(Kind::decided, 1, std::int64_t{1}, 0);
    const auto resolved_b = closed(Kind::resolved, 2, std::string{"b"}, 0, {}, Origin::agent);
    const auto asked_c = asked(2, std::int64_t{1}, 9);
    const auto asked_replaced = asked(1, std::int64_t{3}, 4);
    const auto asked_current = asked(2, std::int64_t{3}, 5);
    const auto resolved_current = closed(Kind::resolved, 2, std::int64_t{3}, 5, {}, Origin::agent);
    // b closed in a later epoch still closes. A stale decision cannot close a
    // same-id request re-derived in a newer epoch.
    const auto open = open_questions({asked_a, asked_b, asked_replaced, decided_a, resolved_b,
                                      asked_current, asked_c, resolved_current});
    require(open.size() == 1 && open.front().revision == 9 && open.front().epoch == 2);
    require(open.front().id == asked_c.id);
    const auto unknown = outcome_unknown_for(open.front());
    require(unknown.kind == Kind::decided && unknown.origin == Origin::outcome_unknown &&
            unknown.epoch == 2 && unknown.revision == 9 && unknown.choice.empty() &&
            !unknown.request.has_value());
}

void oversized_journal_rotates() {
    const auto directory = make_directory();
    const auto path = directory / "journal.attention";
    auto sibling = path;
    sibling += ".1";
    {
        AttentionJournal journal(path, 1); // rotate on every eligible reopen
        journal.append(asked(1, std::int64_t{1}, 1));
        journal.append(closed(Kind::resolved, 1, std::int64_t{1}, 1, {}, Origin::agent));
    }
    {
        AttentionJournal journal(path, 1);
        require(journal.entries().empty());
        require(std::filesystem::exists(sibling));
        AttentionJournal previous(sibling);
        require(previous.entries().size() == 2);
    }
    // An open question blocks rotation so recovery stays in the asking log.
    {
        // 11 bytes: an empty journal (header only) stays; one record exceeds it.
        AttentionJournal held(path, 11);
        held.append(asked(2, std::int64_t{2}, 1));
    }
    AttentionJournal again(path, 11);
    require(again.entries().size() == 1);
    require(open_questions(again.entries()).size() == 1);
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
        {"rotation", oversized_journal_rotates},
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
