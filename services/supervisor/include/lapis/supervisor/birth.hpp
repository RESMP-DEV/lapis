#ifndef LAPIS_SUPERVISOR_BIRTH_HPP
#define LAPIS_SUPERVISOR_BIRTH_HPP

#include "lapis/supervisor/state.hpp"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <string>

namespace lapis::supervisor {

// NOLINTNEXTLINE(performance-enum-size) -- the explicit compact base is ignored by this
// checker.
enum class BirthAgentMode : std::uint8_t { terminal, codex, claude };

struct BirthAgent {
    QString program;
    QStringList arguments;
    QString working_directory;
    unsigned columns{80};
    unsigned rows{24};
    BirthAgentMode mode{BirthAgentMode::terminal};
};

struct BirthRequest {
    std::string endpoint;
    std::string fingerprint;
    BirthAgent agent;
};

[[nodiscard]] BirthRequest decode_birth_request(const std::string& bytes);
[[nodiscard]] std::string encode_birth_reply(const SessionIdentity& identity);
[[nodiscard]] std::string encode_birth_rejected();

} // namespace lapis::supervisor

#endif
