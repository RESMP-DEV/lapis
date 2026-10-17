#include "lapis/supervisor/client.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <exception>
#include <iostream>
#include <string>

namespace {
using namespace lapis::supervisor;

std::string reply_status(const std::string& bytes) {
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    if (!document.isObject())
        throw std::runtime_error{"Supervisor reply is not JSON"};
    return document.object().value(QStringLiteral("status")).toString().toStdString();
}

void print_status(const std::string& bytes, bool json) {
    if (json) {
        std::cout << bytes << '\n';
        return;
    }
    const auto status = decode_status_reply(bytes);
    std::cout << "schema " << status.schema_version << " slot " << status.slot << " enabled "
              << (status.enabled ? "true" : "false") << " service "
              << (status.service_live ? "live" : "absent") << " epoch " << status.instance_epoch
              << '\n';
    if (!status.blocked_reason.empty())
        std::cout << "blocked: " << status.blocked_reason << '\n';
    if (!status.endpoint.empty())
        std::cout << "endpoint: " << status.endpoint << '\n';
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Headless supervisor status and explicit stop client"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("control-endpoint"),
                      QStringLiteral("Absolute supervisor control socket path"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("state-directory"),
                      QStringLiteral("Private supervisor state directory required by stop"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Print machine-readable JSON")});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("status | stop"));
    parser.process(application.arguments());

    const auto positional = parser.positionalArguments();
    const auto command = positional.isEmpty() ? QString{} : positional.first();
    if (positional.size() != 1 ||
        (command != QLatin1String("status") && command != QLatin1String("stop"))) {
        std::cerr << "Expected exactly one command: status or stop\n";
        return 2;
    }
    if (!parser.isSet(QStringLiteral("control-endpoint"))) {
        std::cerr << "--control-endpoint is required\n";
        return 2;
    }

    try {
        const std::string endpoint = parser.value(QStringLiteral("control-endpoint")).toStdString();
        if (command == QLatin1String("status")) {
            const auto reply = send_control_request(endpoint, encode_status_request());
            print_status(reply, parser.isSet(QStringLiteral("json")));
            return 0;
        }
        if (!parser.isSet(QStringLiteral("state-directory"))) {
            std::cerr << "--state-directory is required by stop\n";
            return 2;
        }
        const auto state =
            read_control_state(parser.value(QStringLiteral("state-directory")).toStdString());
        const auto reply = send_control_request(endpoint, encode_stop_request(state));
        if (parser.isSet(QStringLiteral("json")))
            std::cout << reply << '\n';
        else
            std::cout << "supervisor stop: " << reply_status(reply) << '\n';
        return reply_status(reply) == "applied" ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "lapis supervisor: " << error.what() << '\n';
        return 1;
    }
}
