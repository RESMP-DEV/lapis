#include "workspace_registry.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {
using lapis::desktop::WorkspaceEntry;
namespace session = lapis::session;
constexpr qsizetype max_text_bytes = 4096;
void require(bool value, const char* message = "condition",
             std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error(std::string("Workspace registry expectation failed: ") + message +
                                 " at line " + std::to_string(where.line()));
}

template <typename Operation> void rejects(Operation operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Invalid workspace registry state was accepted");
}

template <typename Operation> std::string rejected_message(Operation operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("Expected failure diagnostic was not produced");
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
            entries.back().agent = session::AgentMode::claude;
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

        // The registry does not reserve a guard filename; unrelated files in the
        // same trusted directory must remain valid.
        {
            QFile guard(root + QStringLiteral("/registry.guard"));
            require(guard.open(QIODevice::WriteOnly));
            require(guard.write("unrelated") > 0);
        }

        {
            WorkspaceRegistry registry{path};
            require(registry.read().size() == 8);
            const auto locked_message =
                rejected_message([&] { static_cast<void>(WorkspaceRegistry{path}); });
            require(locked_message.find("already locked") != std::string::npos &&
                        locked_message.find(lock_path.toStdString()) != std::string::npos,
                    "Lock diagnostic omitted contention and path");
        }

        {
            // Manifest storage is not an AF_UNIX endpoint and therefore must not
            // inherit sockaddr_un::sun_path's length restriction.
            const QString deep_name(120, QLatin1Char('d'));
            const QString deep_root = root + QStringLiteral("/") + deep_name;
            const QString deep_path = deep_root + QStringLiteral("/registry.json");
            require(QDir(root).mkpath(deep_root));
            require(QFile::setPermissions(deep_root,
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            WorkspaceRegistry registry{deep_path};
            const std::vector<WorkspaceEntry> deep_entries = {
                entry(temporary, 1, QByteArray{1, '\x01'})};
            registry.write(deep_entries);
            require(registry.read() == deep_entries);
        }

        // A dangling manifest must be detected even though exists() is false, and
        // its unsafe target must not be created or replaced.
        const QString dangling = root + QStringLiteral("/dangling.json");
        const QString unsafe_target = temporary.filePath(QStringLiteral("unsafe-target"));
        {
            WorkspaceRegistry registry{dangling};
            require(::symlink(QFile::encodeName(unsafe_target).constData(),
                              QFile::encodeName(dangling).constData()) == 0);
            rejects([&] { registry.write({}); });
        }
        require(QFileInfo{dangling}.isSymLink());
        require(QFileInfo{dangling}.symLinkTarget() == unsafe_target);
        require(!QFile::exists(unsafe_target));

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
            const auto corrupt_message = rejected_message([&] { registry.write({}); });
            require(corrupt_message.find(path.toStdString()) != std::string::npos,
                    "Corrupt-write diagnostic omitted the storage path");
            require(QFileInfo(path).size() == QByteArrayLiteral("{\"schema\":2}").size());
        }
        {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly));
            require(file.write(original) == original.size());
        }

        const QString link = root + QStringLiteral("/linked.json");
        require(QFile::link(path, link));
        const auto symlink_message =
            rejected_message([&] { static_cast<void>(WorkspaceRegistry{link}); });
        require(symlink_message.find(link.toStdString()) != std::string::npos &&
                    symlink_message.find("filename is unsafe") != std::string::npos,
                "Symlink diagnostic omitted path or validation reason");
        require(QFileInfo(link).isSymLink());
        QFile::remove(link);

        const QString fifo = root + QStringLiteral("/fifo");
        require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        rejects([&] { static_cast<void>(WorkspaceRegistry{fifo}); });
        require(QFile::remove(fifo));

        {
            WorkspaceRegistry registry{path};
            bool rejected = false;
            try {
                static_cast<void>(WorkspaceRegistry{path});
            } catch (const std::runtime_error& error) {
                require(std::string{error.what()}.find("already locked") != std::string::npos);
                rejected = true;
            }
            require(rejected);
        }

        {
            WorkspaceRegistry registry{path};
            const QString hard_link = temporary.filePath(QStringLiteral("hard.json"));
            require(::link(QFile::encodeName(path).constData(),
                           QFile::encodeName(hard_link).constData()) == 0);
            const auto hard_link_message = rejected_message(
                [&] { registry.write({entry(temporary, 1, QByteArray{1, '\x01'})}); });
            require(hard_link_message.find(path.toStdString()) != std::string::npos &&
                        hard_link_message.find("nlink=2") != std::string::npos,
                    "Hard-link diagnostic omitted path and identity metadata");
            require(QFileInfo(path).size() == original.size());
            require(QFile::remove(hard_link));
            set_mode(path, 0644);
            const auto mode_message = rejected_message(
                [&] { registry.write({entry(temporary, 1, QByteArray{1, '\x01'})}); });
            require(mode_message.find(path.toStdString()) != std::string::npos &&
                        mode_message.find("mode=644") != std::string::npos,
                    "Permission diagnostic omitted path and mode");
            require(QFileInfo(path).size() == original.size());
            set_mode(path, 0600);
            rejects([&] {
                auto invalid = entry(temporary, 1, QByteArray{1, '\x01'});
                invalid.fingerprint = QByteArray{31, 'f'};
                registry.write({invalid});
            });
        }

        set_mode(lock_path, 0644);
        const auto lock_mode_message =
            rejected_message([&] { static_cast<void>(WorkspaceRegistry{path}); });
        require(lock_mode_message.find(lock_path.toStdString()) != std::string::npos &&
                    lock_mode_message.find("mode=644") != std::string::npos,
                "Lock-permission diagnostic omitted path and mode");
        set_mode(lock_path, 0600);
        set_mode(root, 0755);
        const auto directory_mode_message =
            rejected_message([&] { static_cast<void>(WorkspaceRegistry{path}); });
        require(directory_mode_message.find(root.toStdString()) != std::string::npos &&
                    directory_mode_message.find("mode=755") != std::string::npos,
                "Directory-permission diagnostic omitted path and mode");
        set_mode(root, 0700);

        std::cout
            << "Workspace registry roundtrip, deep paths, locking and unsafe storage passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
