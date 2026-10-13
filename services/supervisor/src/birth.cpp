#include "lapis/supervisor/birth.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <stdexcept>

namespace lapis::supervisor {
namespace {

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

bool version_one(const QJsonObject& object) {
    const auto value = object.value(QStringLiteral("version"));
    return value.isDouble() && value.toDouble() == 1.0;
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

QString required_string(const QJsonObject& object, const QString& name, const char* message) {
    const auto value = object.value(name);
    if (!value.isString() || value.toString().isEmpty())
        invalid(message);
    return value.toString();
}

unsigned required_dimension(const QJsonObject& object, const QString& name, const char* message) {
    const auto value = object.value(name);
    if (!value.isDouble())
        invalid(message);
    const double number = value.toDouble();
    if (number < 1 || number > 1024 || number != static_cast<double>(static_cast<unsigned>(number)))
        invalid(message);
    return static_cast<unsigned>(number);
}

BirthAgentMode agent_mode(const QString& value, const char* message) {
    if (value == QLatin1String("terminal"))
        return BirthAgentMode::terminal;
    if (value == QLatin1String("codex"))
        return BirthAgentMode::codex;
    if (value == QLatin1String("claude"))
        return BirthAgentMode::claude;
    invalid(message);
}

} // namespace

BirthRequest decode_birth_request(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > max_control_bytes)
        invalid("Supervisor birth request is outside its bound");
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(
        QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        invalid("Supervisor birth request is not valid JSON");
    constexpr auto* message = "Invalid supervisor birth request";
    const auto root = exact_object(document.object(),
                                   {QStringLiteral("version"), QStringLiteral("action"),
                                    QStringLiteral("endpoint"), QStringLiteral("fingerprint"),
                                    QStringLiteral("agent"), QStringLiteral("service")},
                                   message);
    if (!version_one(root) ||
        required_string(root, QStringLiteral("action"), message) != QLatin1String("birth"))
        invalid(message);

    BirthRequest result;
    const auto endpoint = required_string(root, QStringLiteral("endpoint"), message).toUtf8();
    if (endpoint.isEmpty() || static_cast<std::size_t>(endpoint.size()) > max_endpoint_bytes ||
        !endpoint.startsWith('/'))
        invalid(message);
    result.endpoint = std::string{endpoint.constData(), static_cast<std::size_t>(endpoint.size())};
    const auto fingerprint =
        required_string(root, QStringLiteral("fingerprint"), message).toLatin1();
    result.fingerprint =
        std::string{fingerprint.constData(), static_cast<std::size_t>(fingerprint.size())};
    if (!valid_hex(result.fingerprint, fingerprint_hex_bytes))
        invalid(message);

    const auto agent = exact_object(
        root.value(QStringLiteral("agent")),
        {QStringLiteral("mode"), QStringLiteral("program"), QStringLiteral("arguments"),
         QStringLiteral("working_directory"), QStringLiteral("columns"), QStringLiteral("rows")},
        message);
    result.agent.mode =
        agent_mode(required_string(agent, QStringLiteral("mode"), message), message);
    result.agent.program = required_string(agent, QStringLiteral("program"), message);
    result.agent.working_directory =
        required_string(agent, QStringLiteral("working_directory"), message);
    result.agent.columns = required_dimension(agent, QStringLiteral("columns"), message);
    result.agent.rows = required_dimension(agent, QStringLiteral("rows"), message);
    const auto arguments = agent.value(QStringLiteral("arguments"));
    if (!arguments.isArray() || arguments.toArray().size() > 256)
        invalid(message);
    for (const auto& argument : arguments.toArray()) {
        if (!argument.isString() || argument.toString().contains(QChar::Null))
            invalid(message);
        result.agent.arguments.push_back(argument.toString());
    }

    const auto service = exact_object(root.value(QStringLiteral("service")),
                                      {QStringLiteral("environment")}, message);
    const auto environment = service.value(QStringLiteral("environment"));
    if (!environment.isObject() || !environment.toObject().isEmpty())
        invalid("Supervisor service environment overrides are not admitted");
    return result;
}

std::string encode_birth_reply(const SessionIdentity& identity) {
    if (!valid_hex(identity.session_id, identity_hex_bytes) ||
        !valid_hex(identity.epoch, identity_hex_bytes))
        invalid("Cannot encode an invalid supervisor birth identity");
    const auto bytes = QJsonDocument{
        QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("status"), QStringLiteral("born")},
            {QStringLiteral("session_id"), QString::fromStdString(identity.session_id)},
            {QStringLiteral("epoch"), QString::fromStdString(identity.epoch)},
        }}.toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

std::string encode_birth_rejected() {
    const auto bytes = QJsonDocument{
        QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("status"), QStringLiteral("rejected")},
        }}.toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace lapis::supervisor
