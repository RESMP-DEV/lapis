#ifndef LAPIS_SESSION_ATTENTION_AUDIT_HPP
#define LAPIS_SESSION_ATTENTION_AUDIT_HPP

#include "attention_journal.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace lapis::session {

// The exact source derivation recorded with an ask. A changed request body or
// reconnect epoch creates a new derivation even when the agent reuses its ID.
struct JournaledAskPosition {
    std::uint64_t epoch{};
    std::uint64_t revision{};
    bool operator==(const JournaledAskPosition&) const = default;
};
using JournaledAsks = std::map<attention::RequestId, JournaledAskPosition>;

// Local, transport-independent decision fields the service checks before it
// writes `decided`. The observer still owns request details and delivery.
struct AttentionApproval {
    std::uint64_t epoch{};
    attention::RequestId id{std::int64_t{0}};
    std::uint64_t revision{};
    std::string choice;
};

// Appends asks for new or re-derived pending requests, records agent closure,
// and refreshes stale positions. Throws when an append is not durable; the
// caller decides whether presentation is soft or decision delivery is gated.
void journal_attention(AttentionJournal& journal, const attention::State& state,
                       JournaledAsks& journaled);

// Returns false without a journal write for an unknown, stale, submitted,
// non-pending, or unsupported choice. A true result means `decided` is durable.
[[nodiscard]] bool record_intended_decision(AttentionJournal& journal,
                                            const attention::State& state,
                                            const AttentionApproval& approval);

// Durably confirms that the adapter accepted an intended decision. Until this
// succeeds, restart recovery treats the approval outcome as unknown.
void record_delivered_decision(AttentionJournal& journal, const AttentionApproval& approval);

// Compensates a locally valid decision the adapter could not deliver. This
// closes the exact ask so the audit never leaves a false user approval open.
void record_refused_decision(AttentionJournal& journal, const AttentionApproval& approval);
} // namespace lapis::session

#endif
