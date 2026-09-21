#include "workspace_registry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {
using lapis::desktop::WorkspaceEntry;
namespace session = lapis::session;
constexpr qsizetype max_text_bytes = 4096;
void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Workspace registry expectation failed at line " +
                                 std::to_string(where.line()));
}

template <typename Operation> void rejects(Operation operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Invalid workspace registry state was accepted");
}

WorkspaceEntry entry(const QTemporaryDir& directory, int index,
                     const QByteArray& session_delta = QByteArray{1, '\0'}) {
    WorkspaceEntry result;
    result.endpoint = QDir(directory.filePath(QStringLiteral("sockets")))
                          .filePath(QStringLiteral("session-%1.sock").arg(index));
    result.identity.session_id = QByteArray{"0123456789abcdef"};
    result.identity.session_id[15] = session_delta[0];
    result.identity.epoch = QByteArray{"fedcba9876543210"};
    result.fingerprint = QByteArray{32, 'f'};
    result.title = QStringLiteral("Session %1").arg(index);
    result.directory = QStringLiteral("/tmp/lapis-%1").arg(index);
    return result;
}

void set_mode(const QString& path, mode_t mode) {
    struct stat status{};
    require(::stat(QFile::encodeName(path).constData(), &status) == 0);
    require(::chmod(QFile::encodeName(path).constData(), mode) == 0);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace lapis::desktop;
    try {
        QTemporaryDir temporary(QStringLiteral("/private/tmp/lapis-registry-XXXXXX"));
        require(temporary.isValid());
        const QString root = temporary.filePath(QStringLiteral("owner"));
        const QString path = root + QStringLiteral("/registry.json");
        const QString lock_path = root + QStringLiteral("/.lock");

        {
            WorkspaceRegistry registry{path};
            require(registry.read().empty());
            registry.write({});
            require(registry.read().empty());
            std::vector<WorkspaceEntry> entries;
            entries.reserve(8);
            for (int index = 0; index < 8; ++index)
                entries.push_back(entry(temporary, index, QByteArray{1, char(index)}));
            entries.front().agent = session::AgentMode::codex;
            entries.front().title = QStringLiteral("日本語");
            registry.write(entries);
            require(registry.read() == entries);
            for (const auto& private_file : {path, lock_path}) {
                struct stat metadata{};
                require(::stat(QFile::encodeName(private_file).constData(), &metadata) == 0);
                require((metadata.st_mode & 07777U) == 0600U);
            }

            auto duplicate_endpoint = entries;
            duplicate_endpoint.back().endpoint =
                QString(entries.front().endpoint)
                    .replace(QStringLiteral("/sockets/"), QStringLiteral("/./sockets/"));
            rejects([&] { registry.write(duplicate_endpoint); });
            auto duplicate_session = entries;
            duplicate_session.back().identity.session_id = entries.front().identity.session_id;
            rejects([&] { registry.write(duplicate_session); });
            auto too_many = entries;
            too_many.push_back(entry(temporary, 8, QByteArray{1, '\x08'}));
            rejects([&] { registry.write(too_many); });
            auto oversized = entries;
            oversized.front().title = QString(max_text_bytes + 1, QLatin1Char('x'));
            rejects([&] { registry.write(oversized); });
            auto bounded_json = entries;
            const QString long_utf8(max_text_bytes / 3, QChar{0x65e5});
            for (auto& item : bounded_json) {
                item.title = long_utf8;
                item.directory = long_utf8;
            }
            rejects([&] { registry.write(bounded_json); });
            auto invalid_identity = entries;
            invalid_identity.front().identity.session_id = QByteArray{15, 'x'};
            rejects([&] { registry.write(invalid_identity); });
            auto invalid_utf8 = entries;
            invalid_utf8.front().title = QStringLiteral("prefix ") + QChar{0xd800};
            rejects([&] { registry.write(invalid_utf8); });
            auto invalid_agent = entries;
            invalid_agent.front().agent = static_cast<session::AgentMode>(99);
            rejects([&] { registry.write(invalid_agent); });
            require(registry.read() == entries);
        }

        {
            WorkspaceRegistry registry{path};
            require(registry.read().size() == 8);
            rejects([&] { static_cast<void>(WorkspaceRegistry{path}); });
        }

        const QByteArray original = [&] {
            QFile file(path);
            require(file.open(QIODevice::ReadOnly));
            return file.readAll();
        }();
        {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly));
            require(file.write("{\"schema\":2}") > 0);
        }
        rejects([&] {
            WorkspaceRegistry registry{path};
            static_cast<void>(registry.read());
        });
        require(QFileInfo(path).size() == QByteArrayLiteral("{\"schema\":2}").size());

        {
            WorkspaceRegistry registry{path};
            rejects([&] { registry.write({}); });
            require(QFileInfo(path).size() == QByteArrayLiteral("{\"schema\":2}").size());
        }
        {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly));
            require(file.write(original) == original.size());
        }

        const QString link = root + QStringLiteral("/linked.json");
        require(QFile::link(path, link));
        rejects([&] { static_cast<void>(WorkspaceRegistry{link}); });
        require(QFileInfo(link).isSymLink());
        QFile::remove(link);

        const QString fifo = root + QStringLiteral("/fifo");
        require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        rejects([&] { static_cast<void>(WorkspaceRegistry{fifo}); });
        require(QFile::remove(fifo));

        {
            WorkspaceRegistry registry{path};
            const QString hard_link = temporary.filePath(QStringLiteral("hard.json"));
            require(::link(QFile::encodeName(path).constData(),
                           QFile::encodeName(hard_link).constData()) == 0);
            rejects([&] { registry.write({entry(temporary, 1, QByteArray{1, '\x01'})}); });
            require(QFileInfo(path).size() == original.size());
            require(QFile::remove(hard_link));
            set_mode(path, 0644);
            rejects([&] { registry.write({entry(temporary, 1, QByteArray{1, '\x01'})}); });
            require(QFileInfo(path).size() == original.size());
            set_mode(path, 0600);
            rejects([&] {
                auto invalid = entry(temporary, 1, QByteArray{1, '\x01'});
                invalid.fingerprint = QByteArray{31, 'f'};
                registry.write({invalid});
            });
        }

        set_mode(lock_path, 0644);
        rejects([&] { static_cast<void>(WorkspaceRegistry{path}); });
        set_mode(lock_path, 0600);
        set_mode(root, 0755);
        rejects([&] { static_cast<void>(WorkspaceRegistry{path}); });
        set_mode(root, 0700);

        std::cout << "Workspace registry roundtrip, bounds, locking and unsafe storage passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
