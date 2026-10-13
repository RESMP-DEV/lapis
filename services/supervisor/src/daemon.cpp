#include "lapis/supervisor/birth.hpp"
#include "lapis/supervisor/control_server.hpp"
#include "lapis/supervisor/session_service_launcher.hpp"

#include "launch_spec.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace lapis::supervisor;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

std::string option(int argc, char** argv, std::string_view name) {
    const std::string prefix{std::string{name} + "="};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument.starts_with(prefix))
            return std::string{argument.substr(prefix.size())};
    }
    throw std::runtime_error("Missing required daemon option: " + std::string{name});
}

std::string random_hex(std::size_t bytes) {
    std::string result;
    while (result.size() < bytes * 2) {
        const auto random = QUuid::createUuid().toRfc4122().toHex();
        result += std::string{random.constData(), static_cast<std::size_t>(random.size())};
    }
    return result.substr(0, bytes * 2);
}

DesiredSessionLaunch persisted_launch(const BirthAgent& agent) {
    namespace session = lapis::session;
    session::AgentMode mode = session::AgentMode::terminal;
    if (agent.mode == BirthAgentMode::codex)
        mode = session::AgentMode::codex;
    else if (agent.mode == BirthAgentMode::claude)
        mode = session::AgentMode::claude;
    const session::LaunchSpec validated =
        session::validate_launch({.program = agent.program,
                                  .arguments = agent.arguments,
                                  .directory = agent.working_directory,
                                  .size = {static_cast<std::uint16_t>(agent.columns),
                                           static_cast<std::uint16_t>(agent.rows)},
                                  .agent = mode});
    DesiredSessionLaunch result;
    result.program = validated.program.toStdString();
    result.arguments.reserve(static_cast<std::size_t>(validated.arguments.size()));
    for (const auto& argument : validated.arguments)
        result.arguments.push_back(argument.toStdString());
    result.working_directory = validated.directory.toStdString();
    result.columns = validated.size.columns;
    result.rows = validated.size.rows;
    result.mode = agent.mode == BirthAgentMode::codex    ? DesiredAgentMode::codex
                  : agent.mode == BirthAgentMode::claude ? DesiredAgentMode::claude
                                                         : DesiredAgentMode::terminal;
    return result;
}

SessionServiceLaunch service_launch(const DesiredSessionLaunch& launch) {
    SessionServiceLaunch result;
    result.service_program = QString::fromLatin1(LAPIS_SUPERVISOR_SESSION_SERVICE);
    result.agent_program = QString::fromStdString(launch.program);
    result.agent_arguments.reserve(static_cast<qsizetype>(launch.arguments.size()));
    for (const auto& argument : launch.arguments)
        result.agent_arguments.push_back(QString::fromStdString(argument));
    result.working_directory = QString::fromStdString(launch.working_directory);
    result.columns = launch.columns;
    result.rows = launch.rows;
    result.codex = launch.mode == DesiredAgentMode::codex;
    result.claude = launch.mode == DesiredAgentMode::claude;
    return result;
}

class RandomIdentity final : public IdentityProvider {
  public:
    [[nodiscard]] std::string instance_epoch() override { return random_hex(epoch_hex_bytes / 2); }
    [[nodiscard]] std::string session_id() override { return random_hex(identity_hex_bytes / 2); }
    [[nodiscard]] std::string session_epoch() override {
        return random_hex(identity_hex_bytes / 2);
    }
    [[nodiscard]] std::string spawn_token() override {
        return random_hex(spawn_token_hex_bytes / 2);
    }
};

struct ParsedArguments {
    std::string state_directory;
    std::string control_endpoint;
};

ParsedArguments parse_arguments(int argc, char** argv) {
    if (argc != 3)
        throw std::runtime_error("Usage: lapis_supervisor_daemon --state-directory=DIR "
                                 "--control-endpoint=SOCKET");
    ParsedArguments result{option(argc, argv, "--state-directory"),
                           option(argc, argv, "--control-endpoint")};
    if (result.state_directory.empty() || result.state_directory.front() != '/' ||
        result.control_endpoint.empty() || result.control_endpoint.front() != '/')
        throw std::runtime_error("Supervisor daemon paths must be absolute");
    return result;
}

std::string internal_stop_request(const SupervisorRuntime& runtime) {
    const auto& state = runtime.state();
    if (!state.session)
        throw std::runtime_error("No active supervisor session to stop");
    const auto bytes = QJsonDocument{
        QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("action"), QStringLiteral("stop")},
            {QStringLiteral("supervisor_epoch"), QString::fromStdString(state.instance_epoch)},
            {QStringLiteral("token"), QString::fromStdString(state.session->spawn_token)},
            {QStringLiteral("session"), QJsonValue::Null},
        }}.toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

void stop_runtime(const std::unique_ptr<SupervisorRuntime>& runtime) noexcept {
    if (!runtime)
        return;
    try {
        static_cast<void>(runtime->control(::geteuid(), internal_stop_request(*runtime)));
        static_cast<void>(runtime->converge());
    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch) -- shutdown must not
        // displace the original daemon failure or throw from this noexcept path.
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    std::unique_ptr<SupervisorRuntime> runtime;
    try {
        const auto arguments = parse_arguments(argc, argv);
        auto storage = std::make_shared<PosixStateStorage>(arguments.state_directory);
        auto registry = std::make_shared<SupervisorRegistry>(
            storage, std::make_shared<JsonStateCodec>(), std::make_shared<RandomIdentity>());
        if (registry->state().session) {
            const DesiredSession persisted = *registry->state().session;
            if (registry->state().version != 2)
                throw std::runtime_error("Persisted supervisor state version cannot be "
                                         "reconstructed; explicit operator recovery is required");
            if (!persisted.launch)
                throw std::runtime_error("Version-two supervisor state is missing its launch");
            auto reconstructed_launcher = std::make_unique<SessionServiceLauncher>(
                service_launch(*persisted.launch), persisted.fingerprint);
            auto reconstructed_runtime =
                std::make_unique<SupervisorRuntime>(registry, std::move(reconstructed_launcher));
            const auto& converged = *reconstructed_runtime->state().session;
            // A stopped v2 slot still validates launcher construction above, but has no
            // child to adopt. Birth stays refused until an explicit reset protocol
            // replaces this one-shot experimental daemon.
            if (persisted.desired_state == DesiredState::started &&
                (converged.desired_state != DesiredState::started ||
                 reconstructed_runtime->child_pid() <= 0))
                throw std::runtime_error("Supervisor daemon reconstruction failed");
            runtime = std::move(reconstructed_runtime);
        }

        struct sigaction stop_action{};
        stop_action.sa_handler = request_stop;
        sigemptyset(&stop_action.sa_mask);
        stop_action.sa_flags = SA_RESTART;
        if (::sigaction(SIGINT, &stop_action, nullptr) != 0 ||
            ::sigaction(SIGTERM, &stop_action, nullptr) != 0)
            throw std::runtime_error("Could not install supervisor stop handlers");
        LocalControlServer server{
            arguments.control_endpoint, [&](uid_t peer_uid, const std::string& request) {
                if (peer_uid != ::geteuid())
                    return encode_birth_rejected();
                if (runtime)
                    return encode_birth_rejected();
                // A convergence failure deliberately retains its stopped slot: this
                // experimental daemon is one-shot, and silent replacement could hide
                // a launch failure or create a retry storm. Reconstruction and an
                // explicit reset protocol remain later supervisor work.
                if (registry->state().session)
                    return encode_birth_rejected();
                try {
                    const auto birth = decode_birth_request(request);
                    const auto launch = persisted_launch(birth.agent);
                    auto service = service_launch(launch);
                    auto next_launcher =
                        std::make_unique<SessionServiceLauncher>(service, birth.fingerprint);
                    auto next_runtime =
                        std::make_unique<SupervisorRuntime>(registry, std::move(next_launcher));
                    DesiredSession desired;
                    desired.endpoint = birth.endpoint;
                    desired.fingerprint = birth.fingerprint;
                    desired.launch = launch;
                    const auto born = next_runtime->bootstrap(desired);
                    const auto& converged = next_runtime->state();
                    if (!born.session || !converged.session ||
                        converged.session->desired_state != DesiredState::started ||
                        next_runtime->child_pid() <= 0)
                        throw std::runtime_error("Supervisor birth produced no session");
                    runtime = std::move(next_runtime);
                    return encode_birth_reply(born.session->identity);
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch) -- the
                    // bounded protocol reply is rejection, never internal diagnostics.
                    return encode_birth_rejected();
                }
            }};

        while (!stop_requested) {
            server.poll_once(100);
        }
        stop_runtime(runtime);
        return 0;
    } catch (const std::exception& error) {
        stop_runtime(runtime);
        std::cerr << "lapis_supervisor_daemon: " << error.what() << '\n';
        return 1;
    }
}
