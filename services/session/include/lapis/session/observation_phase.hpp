#ifndef LAPIS_SESSION_OBSERVATION_PHASE_HPP
#define LAPIS_SESSION_OBSERVATION_PHASE_HPP

#include <cstdint>

namespace lapis::session::attention {
// Producer-owned lifecycle state, independent of activity, readiness and
// requests. Diagnostics are display-only and must never be parsed for it.
enum class ObservationPhase : std::uint8_t {
    unknown = 0,
    awaiting_first_prompt = 1,
    reconciling = 2,
};
} // namespace lapis::session::attention

#endif
