#include "platform/posix/private_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using lapis::session::posix::write_private_file;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

struct stat status(const QString& path) {
    struct stat info{};
    require(::lstat(QFile::encodeName(path).constData(), &info) == 0, "stat the file");
    return info;
}

mode_t mode(const QString& path) { return status(path).st_mode & 07777U; }

QByteArray contents(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "open the file to read it");
    return file.readAll();
}

// Only the target remains: no temporary is left beside it.
bool only(const QDir& directory, const QString& name) {
    return directory.entryList({name + QStringLiteral("*")},
                               QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot) ==
           QStringList{name};
}

// Created 0600 even with a umask that would leave it world-readable.
void createsOwnerOnly(const QDir& directory) {
    const auto path = directory.filePath(QStringLiteral("new.json"));
    const auto previous = ::umask(0);
    QString error;
    const bool written = write_private_file(path, "fresh", &error);
    ::umask(previous);
    require(written && error.isEmpty(), "a new file is written");
    require(mode(path) == 0600U, "a new file is owner read/write only");
    require(contents(path) == "fresh", "a new file holds the bytes");
    require(only(directory, QStringLiteral("new.json")), "no temporary is left");
}

// A reader holding the old file keeps reading it: the target is replaced by
// rename, not truncated and rewritten in place.
void replacesAtomically(const QDir& directory) {
    const auto path = directory.filePath(QStringLiteral("state.json"));
    require(write_private_file(path, "old"), "write the first version");
    const auto before = status(path).st_ino;
    QFile reader(path);
    require(reader.open(QIODevice::ReadOnly), "hold the old file open");
    require(write_private_file(path, "replacement"), "replace the file");
    require(status(path).st_ino != before, "the target is a new file");
    require(reader.readAll() == "old", "an open reader still sees the old bytes");
    require(contents(path) == "replacement", "the path holds the new bytes");
    require(mode(path) == 0600U, "the replacement is owner-only");
    require(only(directory, QStringLiteral("state.json")), "no temporary is left");
}

// A target another writer left 0644 ends owner-only.
void narrowsAnExistingTarget(const QDir& directory) {
    const auto path = directory.filePath(QStringLiteral("shared.json"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write("wide") == 4, "write a 0644 file");
    file.close();
    require(::chmod(QFile::encodeName(path).constData(), 0644) == 0 && mode(path) == 0644U,
            "make it 0644");
    require(write_private_file(path, "narrow"), "replace the 0644 file");
    require(mode(path) == 0600U, "the 0644 target ends 0600");
    require(contents(path) == "narrow", "the target holds the new bytes");
}

// A link is followed, as QSaveFile follows it; the link itself stays.
void followsASymbolicLink(const QDir& directory) {
    const auto target = directory.filePath(QStringLiteral("target.json"));
    const auto link = directory.filePath(QStringLiteral("link.json"));
    require(write_private_file(target, "before"), "write the link's target");
    require(QFile::link(target, link), "link to the target");
    require(write_private_file(link, "through"), "write through the link");
    require(QFileInfo(link).isSymLink(), "the link is kept");
    require(contents(target) == "through" && mode(target) == 0600U,
            "the target holds the bytes, owner-only");
}

// A relative target is resolved from the link's directory, never the process's
// current directory.
void followsARelativeSymbolicLink(const QDir& directory) {
    require(directory.mkdir(QStringLiteral("nested")), "make the nested directory");
    const QDir nested(directory.filePath(QStringLiteral("nested")));
    const auto target = directory.filePath(QStringLiteral("relative-target.json"));
    const auto link = nested.filePath(QStringLiteral("relative-link.json"));
    require(write_private_file(target, "before"), "write the relative link target");
    require(QFile::link(QStringLiteral("../relative-target.json"), link),
            "link with a relative target");
    require(write_private_file(link, "through"), "write through the relative link");
    require(QFileInfo(link).isSymLink(), "the relative link is kept");
    require(contents(target) == "through" && mode(target) == 0600U,
            "the relative target holds the bytes, owner-only");
}

// A read-only target is refused, as QSaveFile refuses it.
void refusesAReadOnlyTarget(const QDir& directory) {
    const auto path = directory.filePath(QStringLiteral("locked.json"));
    require(write_private_file(path, "kept"), "write the file");
    require(::chmod(QFile::encodeName(path).constData(), 0400) == 0, "make it read-only");
    QString error;
    require(!write_private_file(path, "lost", &error) && !error.isEmpty(),
            "a read-only target is refused with a reason");
    require(contents(path) == "kept" && mode(path) == 0400U, "the read-only target is untouched");
}

// No temporary can be created in a directory that cannot be written.
void failsInAnUnwritableDirectory(const QDir& parent) {
    require(parent.mkdir(QStringLiteral("sealed")), "make the directory");
    const QDir directory(parent.filePath(QStringLiteral("sealed")));
    const auto path = directory.filePath(QStringLiteral("old.json"));
    require(write_private_file(path, "original"), "write the old file");
    const auto sealed = QFile::encodeName(directory.path());
    require(::chmod(sealed.constData(), 0500) == 0, "make the directory unwritable");
    QString error;
    const bool written = write_private_file(path, "replacement", &error);
    require(::chmod(sealed.constData(), 0700) == 0, "make the directory writable again");
    require(!written, "the write fails");
    require(error.contains(path), "the error names the file");
    require(contents(path) == "original" && mode(path) == 0600U, "the old file is intact");
    require(only(directory, QStringLiteral("old.json")), "no temporary is left");
}

} // namespace

int main() {
    try {
        QTemporaryDir root;
        require(root.isValid(), "create a temporary directory");
        const QDir directory(root.path());
        createsOwnerOnly(directory);
        replacesAtomically(directory);
        narrowsAnExistingTarget(directory);
        followsASymbolicLink(directory);
        followsARelativeSymbolicLink(directory);
        refusesAReadOnlyTarget(directory);
        // The superuser writes anywhere; the failure needs an ordinary account.
        if (::geteuid() != 0)
            failsInAnUnwritableDirectory(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
