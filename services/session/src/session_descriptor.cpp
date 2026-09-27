#include "session_descriptor.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/unique_fd.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace lapis::session {
namespace {
constexpr qsizetype descriptor_size = 73;
constexpr qsizetype fingerprint_size = 32;

void check_descriptor(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid session descriptor");
}
[[noreturn]] void throw_system(const char* message) {
    throw std::runtime_error(std::string(message) + ": " +
                             std::error_code(errno, std::generic_category()).message());
}
QByteArray read_exact(int descriptor) {
    QByteArray result(descriptor_size, Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < descriptor_size) {
        const ssize_t count = ::read(descriptor, result.data() + offset,
                                     static_cast<std::size_t>(descriptor_size - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw_system("Cannot read session descriptor");
        }
        check_descriptor(count != 0);
        offset += count;
    }
    return result;
}
void write_exact(int descriptor, const QByteArray& bytes) {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.constData() + offset,
                                      static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw_system("Cannot write session descriptor");
        }
        check_descriptor(count != 0);
        offset += count;
    }
}
QByteArray encode_descriptor(const QByteArray& fingerprint, const wire::SessionIdentity& identity) {
    check_descriptor(fingerprint.size() == fingerprint_size && wire::valid_identity(identity));
    QByteArray result;
    result += QByteArrayLiteral("LAPIS-S1\n");
    result += identity.session_id;
    result += identity.epoch;
    result += fingerprint;
    check_descriptor(result.size() == descriptor_size);
    return result;
}
void validate_open_descriptor(int descriptor) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0)
        throw_system("Cannot inspect session descriptor");
    check_descriptor(S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
                     (info.st_mode & 07777U) == 0600U && info.st_size == descriptor_size);
}
QString descriptor_path(const QString& endpoint) {
    return posix::prepare_endpoint(endpoint) + QLatin1String(".session");
}
struct DescriptorEndpointState {
    std::mutex guard;
    quint64 newest_serial{};
};
} // namespace

struct DescriptorTicket::State {
    std::shared_ptr<DescriptorEndpointState> endpoint;
    quint64 serial{};
    QByteArray bytes;
    QString destination;
    QString temporary;
    int descriptor{-1};
    std::atomic<bool> canceled{false};
    std::atomic_flag released{};
    enum class Phase { preparing, ready, committing, closed };
    std::atomic<Phase> phase{Phase::preparing};
    std::mutex lifecycle;

    ~State() { close_locked(); }

    void close_locked() {
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
            descriptor = -1;
        }
        if (!temporary.isEmpty()) {
            static_cast<void>(::unlink(QFile::encodeName(temporary).constData()));
            temporary.clear();
        }
        phase = Phase::closed;
    }
};
namespace {
struct EndpointOwners {
    std::mutex guard;
    struct Entry {
        std::shared_ptr<DescriptorEndpointState> state;
        std::size_t owners{};
    };
    std::map<QString, Entry> endpoints;
};
EndpointOwners& endpoint_owners() {
    static EndpointOwners owners;
    return owners;
}
void validate_existing_descriptor(const QString& path) {
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0) {
        if (errno == ENOENT)
            return;
        throw_system("Cannot inspect existing session descriptor");
    }
    check_descriptor(S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
                     (info.st_mode & 07777U) == 0600U);
}
} // namespace

std::optional<wire::SessionIdentity> read_descriptor(const QString& endpoint,
                                                     const QByteArray& fingerprint) {
    check_descriptor(fingerprint.size() == fingerprint_size);
    const QString path = descriptor_path(endpoint);
    const int descriptor =
        ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT)
            return std::nullopt;
        throw_system("Cannot open session descriptor");
    }
    const posix::UniqueFd owned{descriptor};
    validate_open_descriptor(descriptor);
    const QByteArray bytes = read_exact(descriptor);
    check_descriptor(bytes.startsWith(QByteArrayLiteral("LAPIS-S1\n")));
    wire::SessionIdentity identity;
    identity.session_id = bytes.sliced(9, 16);
    identity.epoch = bytes.sliced(25, 16);
    check_descriptor(wire::valid_identity(identity) &&
                     QByteArrayView(bytes.sliced(41, fingerprint_size)) ==
                         QByteArrayView(fingerprint));
    return identity;
}

void write_descriptor(const QString& endpoint, const QByteArray& fingerprint,
                      const wire::SessionIdentity& identity) {
    static DescriptorStore store;
    store.write(endpoint, fingerprint, identity);
}

DescriptorTicket::DescriptorTicket(std::shared_ptr<State> state) : state_{std::move(state)} {}

DescriptorTicket::~DescriptorTicket() { cancel(); }

void DescriptorTicket::stage() {
    auto& state = *state_;
    if (state.canceled)
        throw std::runtime_error("Descriptor write was canceled");
    if (state.phase != State::Phase::preparing || state.descriptor >= 0 ||
        !state.temporary.isEmpty())
        throw std::runtime_error("Descriptor is already staged");

    const QFileInfo destination(state.destination);
    const QString base = destination.fileName() + QLatin1Char('.');
    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString temporary = destination.absoluteDir().filePath(
            base + QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex()) +
            QStringLiteral(".tmp"));
        const int descriptor =
            ::open(QFile::encodeName(temporary).constData(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            throw_system("Cannot create temporary session descriptor");
        }
        state.descriptor = descriptor;
        state.temporary = temporary;
        try {
            if (::fchmod(state.descriptor, 0600) != 0)
                throw_system("Cannot set session descriptor permissions");
            write_exact(state.descriptor, state.bytes);
            validate_open_descriptor(state.descriptor);
            if (::fsync(state.descriptor) != 0)
                throw_system("Cannot sync temporary session descriptor");
            if (state.canceled)
                throw std::runtime_error("Descriptor write was canceled");
            const std::lock_guard staged{state.lifecycle};
            if (state.canceled) {
                state.close_locked();
                throw std::runtime_error("Descriptor write was canceled");
            }
            state.phase = State::Phase::ready;
            return;
        } catch (...) {
            const std::lock_guard staged{state.lifecycle};
            state.close_locked();
            throw;
        }
    }
    throw std::runtime_error("Cannot allocate a temporary session descriptor name");
}

void DescriptorTicket::cancel() {
    if (!state_->endpoint || state_->released.test_and_set())
        return;
    {
        auto& owners = endpoint_owners();
        const std::lock_guard owned{owners.guard};
        const std::lock_guard endpoint{state_->endpoint->guard};
        state_->canceled = true;
        const auto found = owners.endpoints.find(state_->destination);
        if (found != owners.endpoints.end() && found->second.state == state_->endpoint) {
            if (found->second.owners > 0)
                --found->second.owners;
            if (found->second.owners == 0)
                owners.endpoints.erase(found);
        }
    }
    {
        const std::lock_guard lifecycle{state_->lifecycle};
        if (state_->phase == State::Phase::ready)
            state_->close_locked();
    }
}

QString DescriptorTicket::commit() {
    auto& state = *state_;
    QString error;
    {
        const std::lock_guard endpoint{state.endpoint->guard};
        if (state.canceled) {
            error = QStringLiteral("Descriptor write was canceled");
        } else if (state.serial != state.endpoint->newest_serial) {
            error = QStringLiteral("A newer session replaced this descriptor");
        } else if (state.phase != State::Phase::ready) {
            error = QStringLiteral("Descriptor is not staged for commit");
        } else {
            try {
                state.phase = State::Phase::committing;
                validate_existing_descriptor(state.destination);
                if (::rename(QFile::encodeName(state.temporary).constData(),
                             QFile::encodeName(state.destination).constData()) != 0)
                    throw_system("Cannot replace session descriptor");
                const std::lock_guard lifecycle{state.lifecycle};
                state.temporary.clear();
                state.close_locked();
                return {};
            } catch (const std::exception& failure) {
                error = QString::fromUtf8(failure.what());
            }
        }
    }
    const std::lock_guard lifecycle{state.lifecycle};
    if (state.phase == State::Phase::ready || state.phase == State::Phase::committing)
        state.close_locked();
    return error;
}

std::shared_ptr<DescriptorTicket> DescriptorStore::prepare(const QString& endpoint,
                                                           const QByteArray& fingerprint,
                                                           const wire::SessionIdentity& identity) {
    const QByteArray bytes = encode_descriptor(fingerprint, identity);
    const QString destination = descriptor_path(endpoint);
    auto state = std::make_shared<DescriptorTicket::State>();
    state->bytes = bytes;
    state->destination = destination;
    // Allocate ownership before registration so an allocation failure cannot
    // leave a retained endpoint entry with no ticket to release it.
    auto ticket = std::shared_ptr<DescriptorTicket>(new DescriptorTicket(state));
    auto& owners = endpoint_owners();
    const std::lock_guard owned{owners.guard};
    auto found = owners.endpoints.find(destination);
    if (found == owners.endpoints.end()) {
        auto endpoint_state = std::make_shared<DescriptorEndpointState>();
        found =
            owners.endpoints.emplace(destination, EndpointOwners::Entry{endpoint_state, 0}).first;
    }
    auto& entry = found->second;
    const std::lock_guard ordered{entry.state->guard};
    if (entry.state->newest_serial == std::numeric_limits<quint64>::max() ||
        entry.owners == std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Descriptor ticket sequence exhausted");
    state->endpoint = entry.state;
    state->serial = ++entry.state->newest_serial;
    ++entry.owners;
    return ticket;
}

QString DescriptorStore::commit(DescriptorTicket& ticket) {
    before_commit(ticket);
    try {
        return ticket.commit();
    } catch (const std::exception& failure) {
        return QString::fromUtf8(failure.what());
    }
}

void DescriptorStore::write(const QString& endpoint, const QByteArray& fingerprint,
                            const wire::SessionIdentity& identity) {
    const auto ticket = prepare(endpoint, fingerprint, identity);
    ticket->stage();
    const QString error = commit(*ticket);
    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
}

void DescriptorStore::before_commit(const DescriptorTicket&) {}
} // namespace lapis::session
