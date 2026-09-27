#ifndef LAPIS_SESSION_ATTENTION_JOURNAL_HPP
#define LAPIS_SESSION_ATTENTION_JOURNAL_HPP
// Durable attention audit trail, ported from DeepSeek Harness's durable
// approval/asked + approval/decided pairs and its fail-closed checkpoint
// barriers (services/session/session-checkpoint-policy). Service-side only:
// append is the barrier, so callers forward a decision only after append()
// returned.
#include <lapis/session/attention.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lapis::session {
class AttentionJournal {
  public:
    enum class Kind : std::uint8_t { asked, decided, resolved };
    // Who closed a request: a decision lapis forwarded (user), the agent
    // resolving or retiring it on its own (agent), or a restart that found it
    // still open after a crash (outcome_unknown).
    enum class Origin : std::uint8_t { user, agent, outcome_unknown };
    struct Entry {
        Kind kind{};
        std::uint64_t seq{}; // Journal-local, dense from 1; assigned by append().
        std::uint64_t epoch{};
        attention::RequestId id{std::int64_t{0}};
        std::uint64_t revision{};
        std::string choice; // decided only
        Origin origin{Origin::user};
        std::optional<attention::Request> request; // asked only
        bool operator==(const Entry&) const = default;
    };

    // Journals are rotated one generation (to <path>.1, replacing it) once
    // they exceed this many bytes at open, and only when no question is open,
    // so recovery always reads a self-consistent log.
    static constexpr std::uint64_t default_rotate_bytes = std::uint64_t{4} * 1024U * 1024U;

    // Opens (creating if absent), takes the exclusive writer lock, replays,
    // and truncates a torn tail left by a crash mid-append. Throws
    // std::runtime_error when another writer holds the lock, the file carries
    // a newer format, or a mid-file record is corrupt; those are conditions a
    // fresh service must not paper over. Oversized journals rotate before
    // returning. When a question is open, rotation is skipped so recovery
    // appends land in the log that asked it.
    explicit AttentionJournal(std::filesystem::path file,
                              std::uint64_t rotate_bytes = default_rotate_bytes);
    ~AttentionJournal();
    AttentionJournal(const AttentionJournal&) = delete;
    AttentionJournal& operator=(const AttentionJournal&) = delete;

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
    // Makes the record durable (write and fsync) before returning and assigns
    // its seq. Throws std::runtime_error when the record cannot be made
    // durable; the caller must then not perform whatever the record gates.
    void append(Entry entry);

  private:
    void open_new();
    void ensure_header();
    void replay();
    void truncate_to(std::uint64_t bytes);
    std::filesystem::path path_;
    std::uint64_t rotate_bytes_;
    std::uint64_t size_{};
    int descriptor_{-1};
    std::vector<Entry> entries_;
    std::uint64_t next_seq_{1};
};

// Asked entries never closed by a later decided or resolved carrying the same
// id, in journal order. Identity survives epochs: a request re-derived under
// a new source epoch closes the question its earlier ask opened. Pure.
[[nodiscard]] std::vector<AttentionJournal::Entry>
open_questions(const std::vector<AttentionJournal::Entry>& entries);

// The synthetic decision recorded for a question whose outcome a crash left
// unknown: distinct from a user decision, so an audit never implies an
// approval that may not have happened.
[[nodiscard]] AttentionJournal::Entry outcome_unknown_for(const AttentionJournal::Entry& asked);
} // namespace lapis::session
#endif
