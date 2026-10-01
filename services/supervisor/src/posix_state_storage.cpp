#include "lapis/supervisor/state.hpp"

#include "platform/posix/unique_fd.hpp"

#include <QDir>
#include <QFile>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace lapis::supervisor {
namespace {

[[noreturn]] void failed(const char* message) { throw std::runtime_error(message); }

[[noreturn]] void failed_system(const char* message) {
    throw std::runtime_error(std::string{message} + ": " + std::generic_category().message(errno));
}

bool valid_directory(const struct stat& info) {
    return S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 07777U) == 0700U;
}

bool valid_file(const struct stat& info) {
    return S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
           (info.st_mode & 07777U) == 0600U && info.st_nlink == 1;
}

struct stat lstat_path(const std::string& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0)
        failed_system("Cannot inspect supervisor state path");
    return info;
}

std::string path_of(const std::string& directory, const char* name) {
    return directory + "/" + name;
}

void write_all(int descriptor, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            failed("Could not write supervisor state");
        done += static_cast<std::size_t>(count);
    }
}

void sync_directory(const std::string& directory) {
    const lapis::session::posix::UniqueFd descriptor{
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (!descriptor || ::fsync(descriptor.get()) != 0)
        failed("Could not sync supervisor state directory");
}

} // namespace

struct PosixStateStorage::State {
    std::string directory;
    lapis::session::posix::UniqueFd lock;
};

PosixStateStorage::PosixStateStorage(std::string directory) {
    if (directory.empty() || directory.front() != '/')
        failed("Supervisor state directory must be absolute");
    const auto path = QString::fromStdString(directory);
    if (!QDir().mkpath(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        failed("Could not create supervisor state directory");
    if (!valid_directory(lstat_path(directory)))
        failed("Supervisor state directory must be owned by you with mode 0700");
    state_ = std::make_unique<State>();
    state_->directory = std::move(directory);
}

PosixStateStorage::~PosixStateStorage() = default;

void PosixStateStorage::acquire() {
    const auto path = path_of(state_->directory, ".supervisor.lock");
    lapis::session::posix::UniqueFd descriptor{
        ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!descriptor)
        failed_system("Could not open supervisor registry lock");
    struct stat info{};
    if (::fchmod(descriptor.get(), 0600) != 0 || ::fstat(descriptor.get(), &info) != 0 ||
        !valid_file(info) || ::flock(descriptor.get(), LOCK_EX | LOCK_NB) != 0)
        failed("Supervisor registry is already open or its lock is not private");
    state_->lock = std::move(descriptor);
}

void PosixStateStorage::release() { state_->lock.reset(); }

std::optional<std::string> PosixStateStorage::load() const {
    const auto path = path_of(state_->directory, "supervisor.json");
    struct stat before{};
    if (::lstat(path.c_str(), &before) != 0) {
        if (errno == ENOENT)
            return std::nullopt;
        failed_system("Cannot inspect supervisor state");
    }
    if (!valid_file(before))
        failed("Supervisor state must be a singly linked owner-only regular file");
    const lapis::session::posix::UniqueFd descriptor{
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor)
        failed_system("Could not open supervisor state");
    struct stat after{};
    if (::fstat(descriptor.get(), &after) != 0 || !valid_file(after) || after.st_size <= 0 ||
        static_cast<unsigned long long>(after.st_size) > max_state_bytes)
        failed("Supervisor state is outside its persistence bound");
    std::string bytes(static_cast<std::size_t>(after.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::read(descriptor.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            failed("Could not read the complete supervisor state");
        done += static_cast<std::size_t>(count);
    }
    return bytes;
}

void PosixStateStorage::store(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > max_state_bytes || !state_->lock)
        failed("Cannot store supervisor state");
    const auto pending = path_of(state_->directory, ".supervisor.json.pending");
    struct stat existing{};
    if (::lstat(pending.c_str(), &existing) == 0) {
        if (::unlink(pending.c_str()) != 0)
            failed_system("Could not recover pending supervisor state");
    } else if (errno != ENOENT) {
        failed_system("Cannot inspect pending supervisor state");
    }
    {
        lapis::session::posix::UniqueFd descriptor{
            ::open(pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (!descriptor)
            failed_system("Could not create pending supervisor state");
        struct stat info{};
        if (::fchmod(descriptor.get(), 0600) != 0 || ::fstat(descriptor.get(), &info) != 0 ||
            !valid_file(info))
            failed("Pending supervisor state is not a private regular file");
        write_all(descriptor.get(), bytes);
        if (::fsync(descriptor.get()) != 0)
            failed_system("Could not sync pending supervisor state");
    }
    const auto target = path_of(state_->directory, "supervisor.json");
    struct stat target_info{};
    if (::lstat(target.c_str(), &target_info) == 0 && !valid_file(target_info))
        failed("Refusing to replace unsafe supervisor state");
    if (::rename(pending.c_str(), target.c_str()) != 0) {
        static_cast<void>(::unlink(pending.c_str()));
        failed_system("Could not replace supervisor state");
    }
    sync_directory(state_->directory);
}

} // namespace lapis::supervisor
