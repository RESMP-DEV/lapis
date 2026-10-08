#include "launch_spec.hpp"
#include "platform/posix/local_endpoint.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
void require(bool value) {
    if (!value)
        throw std::runtime_error("Launch contract expectation failed");
}
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
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
        const auto codex_fingerprint = launch_fingerprint(other);
        other.agent = AgentMode::claude;
        require(launch_fingerprint(validate_launch(other)) != codex_fingerprint);
        require(launch_fingerprint(other) != fingerprint);
        for (const auto* option : {"--settings", "--settings={}", "--bare", "--safe-mode"}) {
            other.arguments = {QString::fromLatin1(option)};
            rejects([&] { static_cast<void>(validate_launch(other)); });
            other.arguments.prepend(QStringLiteral("--"));
            require(validate_launch(other).arguments == other.arguments);
        }
        // Agents outlive builds. A new build must compute the same fingerprint
        // for an existing agent's launch, or it cannot reattach to it: these
        // values change only with a deliberate, migrated identity change.
        const LaunchSpec pinned{.program = QStringLiteral("/bin/sh"),
                                .arguments = {QStringLiteral("resume"), QStringLiteral("abc")},
                                .directory = QStringLiteral("/")};
        for (const auto& [mode, expected] :
             {std::pair{AgentMode::terminal,
                        "7843c56fa9e8312702dc9138b4738a8a380b6e911386f6baa62ac54787ab42d4"},
              std::pair{AgentMode::codex,
                        "3481055a2227832731bcea701d32fa2747c082778bda7712bea8b554ceb0f83d"},
              std::pair{AgentMode::claude,
                        "dd6176abd6e16ec30ef870d91a43209995ca94bcd7440c5f9f8474872b68ef17"}}) {
            auto spec = pinned;
            spec.agent = mode;
            // Production fingerprints canonicalized launches. Exercise the
            // same contract here so a canonicalization regression changes the
            // pinned identity instead of hiding behind an already-canonical
            // literal.
            const auto actual = launch_fingerprint(validate_launch(spec)).toHex();
            if (actual != QByteArray(expected)) {
                std::cerr << "fingerprint changed for mode " << static_cast<int>(mode) << ": "
                          << actual.constData() << '\n';
                require(false);
            }
        }
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
        // A whole terminal screen must fit in one write: local sockets
        // otherwise split a large snapshot across many round trips through
        // both event loops, which dominated input-to-frame latency.
        {
            int pair[2]{};
            require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                    "cannot create fixture socket");
            // Pin a small buffer first, then compare against what the kernel
            // actually granted. Absolute byte floors are host-dependent: Linux
            // doubles the request and caps it at net.core.wmem_max.
            int small = 4096;
            static_cast<void>(::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
            int send_before{};
            socklen_t length = sizeof(send_before);
            require(::getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &send_before, &length) == 0,
                    "cannot read the pinned buffer");
            posix::widen_socket_buffers(pair[0]);
            int send_after{};
            length = sizeof(send_after);
            require(::getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &send_after, &length) == 0,
                    "cannot read the widened buffer");
            require(send_after > send_before, "socket buffer was not widened");
            // An already larger grant must not shrink back to the floor.
            int large = 4 * 1024 * 1024;
            static_cast<void>(::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &large, sizeof(large)));
            int granted{};
            length = sizeof(granted);
            require(::getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &granted, &length) == 0,
                    "cannot read the pre-existing buffer");
            posix::widen_socket_buffers(pair[0]);
            length = sizeof(send_after);
            require(::getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &send_after, &length) == 0,
                    "cannot read the widened buffer");
            require(send_after >= granted, "widening shrank an existing buffer");
            // An invalid descriptor is ignored rather than crashing a caller.
            posix::widen_socket_buffers(-1);
            ::close(pair[0]);
            ::close(pair[1]);
        }
        std::cout << "Launch identities, bounds and private endpoints passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
