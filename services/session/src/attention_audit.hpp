#ifndef LAPIS_SESSION_ATTENTION_AUDIT_HPP
#define LAPIS_SESSION_ATTENTION_AUDIT_HPP

#include "attention_journal.hpp"

#include <cstdint>
#include <map>

namespace lapis::session {

// The exact source derivation recorded with an ask. A changed request body or
// reconnect epoch creates a new derivation even when the agent reuses its ID.
struct JournaledAskPosition {
    std::uint64_t epoch{};
    std::uint64_t revision{};
    bool operator==(const JournaledAskPosition&) const = default;
};
using JournaledAsks = std::map<attention::RequestId, JournaledAskPosition>;

// Appends asks for new or re-derived pending requests, records agent closure,
// and refreshes stale positions. Throws when an append is not durable; the
// caller keeps presentation soft. lapis delivers no decisions: every agent
// takes its answers in its own terminal, so the agent closes every ask.
void journal_attention(AttentionJournal& journal, const attention::State& state,
                       JournaledAsks& journaled);
} // namespace lapis::session

#endif
