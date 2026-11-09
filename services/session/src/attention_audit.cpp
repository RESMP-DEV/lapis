#include "attention_audit.hpp"

#include <string>
#include <utility>

namespace lapis::session {
namespace {
AttentionJournal::Entry asked_entry(const attention::State& state,
                                    const attention::Pending& pending) {
    return {.kind = AttentionJournal::Kind::asked,
            .seq = 0,
            .epoch = state.epoch(),
            .id = pending.request.id,
            .revision = pending.revision,
            .choice = {},
            .origin = AttentionJournal::Origin::user,
            .request = pending.request};
}

AttentionJournal::Entry closed_entry(AttentionJournal::Kind kind, std::uint64_t epoch,
                                     const attention::RequestId& id, std::uint64_t revision,
                                     std::string choice, AttentionJournal::Origin origin) {
    return {.kind = kind,
            .seq = 0,
            .epoch = epoch,
            .id = id,
            .revision = revision,
            .choice = std::move(choice),
            .origin = origin,
            .request = std::nullopt};
}
} // namespace

void journal_attention(AttentionJournal& journal, const attention::State& state,
                       JournaledAsks& journaled) {
    for (const auto& [id, pending] : state.pending()) {
        if (pending.submitted)
            continue;
        const auto known = journaled.find(id);
        if (known != journaled.end() &&
            known->second == JournaledAskPosition{state.epoch(), pending.revision})
            continue;
        journal.append(asked_entry(state, pending));
        journaled.insert_or_assign(id, JournaledAskPosition{state.epoch(), pending.revision});
    }
    for (auto entry = journaled.begin(); entry != journaled.end();) {
        if (state.pending().contains(entry->first)) {
            ++entry;
            continue;
        }
        journal.append(closed_entry(AttentionJournal::Kind::resolved, entry->second.epoch,
                                    entry->first, entry->second.revision, {},
                                    AttentionJournal::Origin::agent));
        entry = journaled.erase(entry);
    }
}
} // namespace lapis::session
