#include "launch_spec.hpp"
#include "platform/posix/local_endpoint.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("Launch contract expectation failed");
}
template <typename Operation> void rejects(Operation operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Invalid launch was accepted");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace lapis::session;
    try {
        // Keep Unix socket fixtures short and retain the canonical path used by validation.
        const auto temporary_root = QDir(QStringLiteral("/tmp")).canonicalPath();
        if (temporary_root.isEmpty())
            throw std::runtime_error("Launch fixture requires an existing /tmp directory");
        QTemporaryDir temporary(temporary_root + QStringLiteral("/lapis-launch-XXXXXX"));
        require(temporary.isValid());
        const auto launch = validate_launch({.program = QStringLiteral("/bin/sh"),
                                             .arguments = {QStringLiteral("-i")},
                                             .directory = temporary.path()});
        const auto fingerprint = launch_fingerprint(launch);
        auto other = launch;
        other.size = {80, 24};
        require(launch_fingerprint(other) == fingerprint);
        other.agent = AgentMode::codex;
        require(launch_fingerprint(validate_launch(other)) != fingerprint);
        other.arguments = {QStringLiteral("--remote=unix:///tmp/other.sock")};
        rejects([&] { static_cast<void>(validate_launch(other)); });
        other.arguments = {QStringLiteral("--"), QStringLiteral("--remote=literal-prompt")};
        require(validate_launch(other).arguments == other.arguments);
        other.agent = static_cast<AgentMode>(99);
        rejects([&] { static_cast<void>(validate_launch(other)); });
        rejects([&] { static_cast<void>(launch_fingerprint(other)); });
        other = launch;
        other.arguments = {QStringLiteral("-c"), QStringLiteral("a b")};
        const auto literal = launch_fingerprint(other);
        other.arguments = {QStringLiteral("-c"), QStringLiteral("a"), QStringLiteral("b")};
        require(launch_fingerprint(other) != literal);
        other = launch;
        other.arguments.append(QString{});
        require(launch_fingerprint(other) != fingerprint);
        other.arguments = {QStringLiteral("bad") + QChar::Null};
        rejects([&] { static_cast<void>(validate_launch(other)); });
        other = launch;
        other.arguments = {QString(65537, QLatin1Char('x'))};
        rejects([&] { static_cast<void>(validate_launch(other)); });
        other = launch;
        other.size = {0, 24};
        rejects([&] { static_cast<void>(validate_launch(other)); });

        const auto endpoint = temporary.filePath(QStringLiteral("private/session.sock"));
        require(posix::prepare_endpoint(endpoint) == endpoint);
        const auto shared = temporary.filePath(QStringLiteral("shared"));
        require(QDir().mkdir(shared));
        require(QFile::setPermissions(shared, QFile::ReadOwner | QFile::WriteOwner |
                                                  QFile::ExeOwner | QFile::ReadOther));
        rejects([&] {
            static_cast<void>(posix::prepare_endpoint(shared + QStringLiteral("/session.sock")));
        });
        require(QFile::permissions(shared).testFlag(QFile::ReadOther));
        const auto existing = temporary.filePath(QStringLiteral("existing"));
        require(QDir().mkdir(existing));
        require(QFile::setPermissions(existing,
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto nested = existing + QStringLiteral("/nested");
        require(QDir().mkdir(nested));
        require(
            QFile::setPermissions(nested, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto original = QFile::permissions(existing);
        require(QFile::setPermissions(existing, QFile::ReadOwner | QFile::WriteOwner |
                                                    QFile::ExeOwner | QFile::WriteGroup));
        rejects([&] {
            static_cast<void>(posix::prepare_endpoint(nested + QStringLiteral("/session.sock")));
        });
        require(QFile::setPermissions(existing, original));
        require(!posix::prepare_endpoint(nested + QStringLiteral("/session.sock")).isEmpty());
        QTemporaryDir sticky_parent(QStringLiteral("/tmp/lapis-endpoint-XXXXXX"));
        require(sticky_parent.isValid());
        require(!posix::prepare_endpoint(sticky_parent.filePath(QStringLiteral("session.sock")))
                     .isEmpty());
        const auto occupied = temporary.filePath(QStringLiteral("ordinary-file"));
        QFile ordinary(occupied);
        require(ordinary.open(QIODevice::WriteOnly));
        ordinary.close();
        rejects([&] { static_cast<void>(posix::prepare_endpoint(occupied)); });
        require(QFile::exists(occupied));
        const auto link = temporary.filePath(QStringLiteral("linked.sock"));
        require(QFile::link(endpoint, link));
        rejects([&] { static_cast<void>(posix::prepare_endpoint(link)); });
        std::cout << "Launch identities, bounds and private endpoints passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
