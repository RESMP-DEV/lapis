#include "lapis/supervisor/state.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lapis::supervisor {
namespace {

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

std::string json_text(const QString& value, std::size_t maximum, bool allow_empty,
                      const char* message) {
    const auto bytes = value.toUtf8();
    if ((!allow_empty && bytes.isEmpty()) || bytes.size() > static_cast<qsizetype>(maximum))
        invalid(message);
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

std::string required_text(const QJsonObject& object, const QString& name, std::size_t maximum,
                          const char* message) {
    const auto value = object.value(name);
    if (!value.isString())
        invalid(message);
    return json_text(value.toString(), maximum, false, message);
}

std::string optional_text(const QJsonObject& object, const QString& name, std::size_t maximum,
                          const char* message) {
    const auto value = object.value(name);
    if (!value.isString())
        invalid(message);
    return json_text(value.toString(), maximum, true, message);
}

std::string required_hex(const QJsonObject& object, const QString& name, std::size_t length,
                         const char* message) {
    const auto value = object.value(name);
    if (!value.isString())
        invalid(message);
    auto result = json_text(value.toString(), length, false, message);
    if (!valid_hex(result, length))
        invalid(message);
    return result;
}

DesiredState desired_state(const QJsonObject& object, const char* message) {
    const auto value = object.value(QStringLiteral("desired_state"));
    if (!value.isString())
        invalid(message);
    const auto name = value.toString();
    if (name == QLatin1String("started"))
        return DesiredState::started;
    if (name == QLatin1String("stopped"))
        return DesiredState::stopped;
    invalid(message);
}

QJsonObject exact_object(const QJsonValue& value, std::initializer_list<QString> names,
                         const char* message) {
    if (!value.isObject())
        invalid(message);
    const auto object = value.toObject();
    if (object.size() != static_cast<qsizetype>(names.size()))
        invalid(message);
    for (const auto& name : names) {
        if (!object.contains(name))
            invalid(message);
    }
    return object;
}

bool version_one(const QJsonObject& object) {
    const auto value = object.value(QStringLiteral("version"));
    if (!value.isDouble())
        return false;
    const auto version = value.toDouble();
    return version == 1.0;
}

bool boolean(const QJsonObject& object, const QString& name, const char* message) {
    const auto value = object.value(name);
    if (!value.isBool())
        invalid(message);
    return value.toBool();
}

SessionIdentity identity(const QJsonObject& session, const char* message) {
    const auto object =
        exact_object(session.value(QStringLiteral("identity")),
                     {QStringLiteral("session_id"), QStringLiteral("epoch")}, message);
    return {required_hex(object, QStringLiteral("session_id"), identity_hex_bytes, message),
            required_hex(object, QStringLiteral("epoch"), identity_hex_bytes, message)};
}

std::optional<DesiredSession> state_session(const QJsonValue& value, const char* message) {
    if (value.isNull())
        return std::nullopt;
    const auto session =
        exact_object(value,
                     {QStringLiteral("endpoint"), QStringLiteral("fingerprint"),
                      QStringLiteral("identity"), QStringLiteral("desired_state"),
                      QStringLiteral("spawn_token"), QStringLiteral("blocked_reason")},
                     message);
    DesiredSession result;
    result.endpoint =
        required_text(session, QStringLiteral("endpoint"), max_endpoint_bytes, message);
    result.fingerprint =
        required_hex(session, QStringLiteral("fingerprint"), fingerprint_hex_bytes, message);
    result.identity = identity(session, message);
    result.desired_state = desired_state(session, message);
    result.spawn_token =
        optional_text(session, QStringLiteral("spawn_token"), spawn_token_hex_bytes, message);
    result.blocked_reason =
        optional_text(session, QStringLiteral("blocked_reason"), max_blocked_reason_bytes, message);
    if (!result.spawn_token.empty() && !valid_hex(result.spawn_token, spawn_token_hex_bytes))
        invalid(message);
    return result;
}

QJsonObject encode_session(const DesiredSession& session) {
    return QJsonObject{
        {QStringLiteral("endpoint"), QString::fromStdString(session.endpoint)},
        {QStringLiteral("fingerprint"), QString::fromStdString(session.fingerprint)},
        {QStringLiteral("identity"),
         QJsonObject{
             {QStringLiteral("session_id"), QString::fromStdString(session.identity.session_id)},
             {QStringLiteral("epoch"), QString::fromStdString(session.identity.epoch)}}},
        {QStringLiteral("desired_state"), session.desired_state == DesiredState::started
                                              ? QStringLiteral("started")
                                              : QStringLiteral("stopped")},
        {QStringLiteral("spawn_token"), QString::fromStdString(session.spawn_token)},
        {QStringLiteral("blocked_reason"), QString::fromStdString(session.blocked_reason)}};
}

} // namespace

std::string JsonStateCodec::encode(const SupervisorState& state) const {
    if (!valid_state(state))
        invalid("Cannot encode an invalid supervisor state");
    QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("enabled"), state.enabled},
        {QStringLiteral("instance_epoch"), QString::fromStdString(state.instance_epoch)},
        {QStringLiteral("session"), state.session ? QJsonValue{encode_session(*state.session)}
                                                  : QJsonValue{QJsonValue::Null}}};
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > static_cast<qsizetype>(max_state_bytes))
        invalid("Supervisor state exceeds its persistence bound");
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

SupervisorState JsonStateCodec::decode(const std::string& bytes) const {
    if (bytes.empty() || bytes.size() > max_state_bytes)
        invalid("Supervisor state is outside its persistence bound");
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(
        QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        invalid("Supervisor state is not valid JSON");
    constexpr auto* message = "Invalid supervisor state schema";
    const auto root = exact_object(document.object(),
                                   {QStringLiteral("version"), QStringLiteral("enabled"),
                                    QStringLiteral("instance_epoch"), QStringLiteral("session")},
                                   message);
    if (!version_one(root))
        invalid("Unsupported supervisor state version");
    SupervisorState result;
    result.enabled = boolean(root, QStringLiteral("enabled"), message);
    result.instance_epoch = required_hex(root, QStringLiteral("instance_epoch"), epoch_hex_bytes,
                                         "Invalid supervisor instance epoch");
    result.session = state_session(root.value(QStringLiteral("session")), message);
    if (!valid_state(result))
        invalid("Invalid supervisor state values");
    return result;
}

ControlRequest JsonStateCodec::decode_control(const std::string& bytes) const {
    if (bytes.empty() || bytes.size() > max_control_bytes)
        invalid("Supervisor control request is outside its bound");
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(
        QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        invalid("Supervisor control request is not valid JSON");
    constexpr auto* message = "Invalid supervisor control schema";
    const auto root = exact_object(document.object(),
                                   {QStringLiteral("version"), QStringLiteral("action"),
                                    QStringLiteral("supervisor_epoch"), QStringLiteral("token"),
                                    QStringLiteral("session")},
                                   message);
    if (!version_one(root))
        invalid("Unsupported supervisor control version");
    ControlRequest result;
    result.instance_epoch = required_hex(root, QStringLiteral("supervisor_epoch"), epoch_hex_bytes,
                                         "Invalid supervisor epoch");
    result.token = required_hex(root, QStringLiteral("token"), spawn_token_hex_bytes,
                                "Invalid supervisor token");
    const auto action = root.value(QStringLiteral("action"));
    if (!action.isString())
        invalid("Invalid supervisor control action");
    const auto name = action.toString();
    if (name == QLatin1String("start")) {
        constexpr auto* start_message = "Invalid supervisor start request";
        result.action = ControlAction::start;
        const auto session = exact_object(
            root.value(QStringLiteral("session")),
            {QStringLiteral("endpoint"), QStringLiteral("fingerprint"), QStringLiteral("identity")},
            start_message);
        DesiredSession requested;
        requested.endpoint =
            required_text(session, QStringLiteral("endpoint"), max_endpoint_bytes, start_message);
        requested.fingerprint = required_hex(session, QStringLiteral("fingerprint"),
                                             fingerprint_hex_bytes, start_message);
        requested.identity = identity(session, start_message);
        requested.desired_state = DesiredState::started;
        if (!valid_desired_session(requested))
            invalid(start_message);
        result.session = std::move(requested);
    } else if (name == QLatin1String("stop")) {
        result.action = ControlAction::stop;
        if (!root.value(QStringLiteral("session")).isNull())
            invalid("A stop request has a session payload");
    } else if (name == QLatin1String("disable")) {
        result.action = ControlAction::disable;
        if (!root.value(QStringLiteral("session")).isNull())
            invalid("A disable request has a session payload");
    } else {
        invalid("Unknown supervisor control action");
    }
    return result;
}

} // namespace lapis::supervisor
