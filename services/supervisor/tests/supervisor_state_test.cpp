#include "lapis/supervisor/state.hpp"

#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <sys/stat.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace lapis::supervisor;

void require(bool value, const char* message,
             std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error(std::string{"Supervisor check failed at line "} +
                                 std::to_string(where.line()) + ": " + message);
}

template <typename Function> void rejects(const char* name, Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string{name} + " unexpectedly accepted invalid input");
}

std::string repeat(char value, std::size_t length) { return std::string(length, value); }

DesiredSession record() {
    DesiredSession result;
    result.endpoint = "/tmp/lapis-supervisor.sock";
    result.fingerprint = repeat('a', fingerprint_hex_bytes);
    result.identity = {repeat('1', identity_hex_bytes), repeat('2', identity_hex_bytes)};
    result.desired_state = DesiredState::stopped;
    return result;
}

SupervisorState state_with(DesiredSession session) {
    SupervisorState result;
    result.instance_epoch = repeat('3', epoch_hex_bytes);
    result.session = std::move(session);
    return result;
}

class DeterministicIdentity final : public IdentityProvider {
  public:
    [[nodiscard]] std::string instance_epoch() override { return repeat('4', epoch_hex_bytes); }
    [[nodiscard]] std::string session_id() override { return repeat('5', identity_hex_bytes); }
    [[nodiscard]] std::string session_epoch() override { return repeat('6', identity_hex_bytes); }
    [[nodiscard]] std::string spawn_token() override {
        const auto value = static_cast<char>('0' + token_serial_++ % 10);
        return repeat(value, spawn_token_hex_bytes);
    }

  private:
    unsigned token_serial_{0};
};

std::shared_ptr<SupervisorRegistry> registry(std::shared_ptr<StateStorage> storage) {
    return std::make_shared<SupervisorRegistry>(std::move(storage),
                                                std::make_shared<JsonStateCodec>(),
                                                std::make_shared<DeterministicIdentity>());
}

QJsonObject control_document(ControlAction action, const std::string& epoch,
                             const std::string& token, const QJsonValue& session = {}) {
    QString name;
    if (action == ControlAction::start)
        name = QStringLiteral("start");
    else if (action == ControlAction::stop)
        name = QStringLiteral("stop");
    else
        name = QStringLiteral("disable");
    QJsonObject result{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), name},
        {QStringLiteral("supervisor_epoch"), QString::fromStdString(epoch)},
        {QStringLiteral("token"), QString::fromStdString(token)},
        {QStringLiteral("session"), session},
    };
    return result;
}

QJsonObject start_payload() {
    const auto session = record();
    return QJsonObject{
        {QStringLiteral("endpoint"), QString::fromStdString(session.endpoint)},
        {QStringLiteral("fingerprint"), QString::fromStdString(session.fingerprint)},
        {QStringLiteral("identity"),
         QJsonObject{
             {QStringLiteral("session_id"), QString::fromStdString(session.identity.session_id)},
             {QStringLiteral("epoch"), QString::fromStdString(session.identity.epoch)}}}};
}

std::string json_bytes(const QJsonObject& object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

void schema_and_codec() {
    const JsonStateCodec codec;
    auto empty = state_with(record());
    empty.session.reset();
    require(codec.decode(codec.encode(empty)) == empty, "empty state round trip");

    auto filled = state_with(record());
    filled.session->desired_state = DesiredState::started;
    filled.enabled = true;
    filled.session->spawn_token = repeat('7', spawn_token_hex_bytes);
    require(codec.decode(codec.encode(filled)) == filled, "session state round trip");

    rejects("an unsupported version", [&] {
        auto invalid = empty;
        invalid.version = 0;
        static_cast<void>(codec.encode(invalid));
    });
    rejects("a relative endpoint", [&] {
        auto invalid = record();
        invalid.endpoint = "relative.sock";
        static_cast<void>(codec.encode(state_with(invalid)));
    });
    rejects("a malformed identity", [&] {
        auto invalid = record();
        invalid.identity.epoch.resize(identity_hex_bytes - 1);
        static_cast<void>(codec.encode(state_with(invalid)));
    });
    rejects("an oversized diagnostic", [&] {
        auto invalid = record();
        invalid.blocked_reason = std::string(max_blocked_reason_bytes + 1, 'x');
        static_cast<void>(codec.encode(state_with(invalid)));
    });
    rejects("a started but disabled supervisor", [&] {
        auto invalid = filled;
        invalid.enabled = false;
        static_cast<void>(codec.encode(invalid));
    });
    rejects("oversized state bytes",
            [&] { static_cast<void>(codec.decode(std::string(max_state_bytes + 1, '{'))); });
    rejects("a non-object state", [&] { static_cast<void>(codec.decode("[]")); });
}

void transitions_and_persistence() {
    auto storage = std::make_shared<MemoryStateStorage>();
    auto owner = registry(storage);
    require(!owner->state().enabled && !owner->state().session, "fresh supervisor state");

    const auto started = owner->start(record());
    require(started.enabled, "start enables the supervisor");
    require(started.session.has_value(), "start creates the one desired session");
    require(started.session->desired_state == DesiredState::started, "start state");
    require(started.session->spawn_token.size() == spawn_token_hex_bytes, "start token");
    require(started.session->blocked_reason.empty(), "a start has no blocker");
    require(storage->write_count() == 1 && storage->fsynced() && storage->mode_is_owner_only(),
            "a start commits privately");

    const auto start_token = started.session->spawn_token;
    const auto stopped = owner->stop();
    require(stopped.enabled, "stop is distinct from disable");
    require(stopped.session.has_value() && stopped.session->desired_state == DesiredState::stopped,
            "stop state");
    require(stopped.session->spawn_token != start_token && !stopped.session->spawn_token.empty(),
            "stop retires the start token");

    const auto disabled = owner->disable();
    require(!disabled.enabled && disabled.session.has_value() &&
                disabled.session->desired_state == DesiredState::stopped &&
                disabled.session->spawn_token.empty() &&
                disabled.session->blocked_reason == "supervisor disabled",
            "disable state");
    owner.reset();

    auto reloaded = registry(storage);
    require(reloaded->state() == disabled, "state reloads exactly");
    rejects("a second memory registry", [&] { static_cast<void>(registry(storage)); });
    reloaded.reset();

    rejects("corrupt saved state",
            [&] { static_cast<void>(registry(std::make_shared<MemoryStateStorage>("{}"))); });
}

void posix_private_atomic_storage() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary state directory");
    const auto root = directory.path().toStdString();
    {
        auto unlocked = std::make_shared<PosixStateStorage>(root);
        rejects("an unlocked POSIX load", [&] { static_cast<void>(unlocked->load()); });
        rejects("an unlocked POSIX store", [&] { unlocked->store("{\"version\":1}"); });
    }
    SupervisorState saved;
    {
        auto storage = std::make_shared<PosixStateStorage>(root);
        auto owner = registry(storage);
        saved = owner->start(record());
        auto competing = std::make_shared<PosixStateStorage>(root);
        rejects("a second POSIX registry", [competing] { competing->acquire(); });
        competing.reset();
        struct stat info{};
        require(::stat((root + "/supervisor.json").c_str(), &info) == 0, "state file exists");
        require(S_ISREG(info.st_mode) && (info.st_mode & 07777U) == 0600U, "state file is 0600");
        struct stat lock_info{};
        require(::stat((root + "/.supervisor.lock").c_str(), &lock_info) == 0, "lock exists");
        require(S_ISREG(lock_info.st_mode) && (lock_info.st_mode & 07777U) == 0600U,
                "lock file is 0600");
        require(::lstat((root + "/.supervisor.json.pending").c_str(), &info) != 0,
                "atomic rename removed its pending file");
    }
    auto storage = std::make_shared<PosixStateStorage>(root);
    auto owner = registry(storage);
    require(owner->state() == saved, "private state reloads");
    owner.reset();
    storage.reset();

    rejects("an existing public state file", [&] {
        QTemporaryDir bad_directory;
        require(bad_directory.isValid(), "second temporary directory");
        const auto bad_root = bad_directory.path().toStdString();
        {
            QFile file(bad_directory.filePath(QStringLiteral("supervisor.json")));
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write fixture");
            require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup),
                    "public fixture permissions");
            file.write("{}");
        }
        auto unsafe = std::make_shared<PosixStateStorage>(bad_root);
        unsafe->acquire();
        static_cast<void>(unsafe->load());
    });
}

void fresh_registry_rejects_every_control_token() {
    auto storage = std::make_shared<MemoryStateStorage>();
    auto owner = registry(storage);
    const auto epoch = owner->state().instance_epoch;
    const auto zero = repeat('0', spawn_token_hex_bytes);
    for (const auto action : {ControlAction::start, ControlAction::stop, ControlAction::disable}) {
        const ControlRequest request{
            .instance_epoch = epoch, .token = zero, .action = action, .session = std::nullopt};
        require(owner->control(request).status == ControlStatus::unauthorized,
                "A fresh registry accepted the all-zero token");
    }
    owner.reset();
}

void control_parsing_and_authentication() {
    const JsonStateCodec codec;
    const auto epoch = repeat('4', epoch_hex_bytes);
    const auto token = repeat('7', spawn_token_hex_bytes);
    const auto stop_bytes = json_bytes(
        control_document(ControlAction::stop, epoch, token, QJsonValue{QJsonValue::Null}));
    const auto request = codec.decode_control(stop_bytes);
    require(request.action == ControlAction::stop && request.instance_epoch == epoch &&
                request.token == token && !request.session.has_value(),
            "bounded stop request");

    const auto start_bytes =
        json_bytes(control_document(ControlAction::start, epoch, token, start_payload()));
    const auto start_request = codec.decode_control(start_bytes);
    require(start_request.session.has_value() &&
                start_request.session->desired_state == DesiredState::started,
            "bounded start request");

    rejects("an oversized request", [&] {
        static_cast<void>(codec.decode_control(std::string(max_control_bytes + 1, '{')));
    });
    rejects("a malformed token", [&] {
        auto malformed = control_document(ControlAction::stop, epoch, token);
        malformed.insert(QStringLiteral("token"), QStringLiteral("short"));
        static_cast<void>(codec.decode_control(json_bytes(malformed)));
    });
    rejects("an unknown action", [&] {
        auto malformed = control_document(ControlAction::stop, epoch, token);
        malformed.insert(QStringLiteral("action"), QStringLiteral("launch"));
        static_cast<void>(codec.decode_control(json_bytes(malformed)));
    });
    rejects("a start without its session", [&] {
        static_cast<void>(
            codec.decode_control(json_bytes(control_document(ControlAction::start, epoch, token))));
    });

    auto owner = registry(std::make_shared<MemoryStateStorage>());
    const auto started = owner->start(record());
    ControlRequest authenticated_start;
    authenticated_start.instance_epoch = epoch;
    authenticated_start.token = started.session->spawn_token;
    authenticated_start.action = ControlAction::start;
    authenticated_start.session = started.session;
    require(owner->control(authenticated_start).status == ControlStatus::applied,
            "an authenticated start applies");
    const auto restarted = owner->state();
    require(restarted.session.has_value() &&
                restarted.session->spawn_token != started.session->spawn_token,
            "an applied start rotates its token");
    ControlRequest authenticated_stop;
    authenticated_stop.instance_epoch = epoch;
    authenticated_stop.token = restarted.session->spawn_token;
    authenticated_stop.action = ControlAction::stop;
    authenticated_stop.session = std::nullopt;
    require(owner->control(authenticated_stop).status == ControlStatus::applied,
            "an authenticated stop applies");
    const auto stopped = owner->state();
    ControlRequest stale_token;
    stale_token.instance_epoch = epoch;
    stale_token.token = started.session->spawn_token;
    stale_token.action = ControlAction::stop;
    stale_token.session = std::nullopt;
    require(owner->control(stale_token).status == ControlStatus::unauthorized,
            "a stale token is rejected");
    require(owner->state() == stopped, "a rejected request has no side effect");
    ControlRequest stale_epoch;
    stale_epoch.instance_epoch = repeat('9', epoch_hex_bytes);
    stale_epoch.token = stopped.session->spawn_token;
    stale_epoch.action = ControlAction::disable;
    stale_epoch.session = std::nullopt;
    require(owner->control(stale_epoch).status == ControlStatus::stale_epoch,
            "a stale supervisor epoch is rejected");
    ControlRequest missing_session;
    missing_session.instance_epoch = epoch;
    missing_session.token = stopped.session->spawn_token;
    missing_session.action = ControlAction::start;
    missing_session.session = std::nullopt;
    require(owner->control(missing_session).status == ControlStatus::rejected,
            "start requires its session payload");
}

} // namespace

int main() {
    try {
        schema_and_codec();
        transitions_and_persistence();
        fresh_registry_rejects_every_control_token();
        posix_private_atomic_storage();
        control_parsing_and_authentication();
        std::cout << "Supervisor schema, storage, transitions and control passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
