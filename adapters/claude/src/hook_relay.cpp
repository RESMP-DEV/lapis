#include "hook_relay.hpp"
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <array>
#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace lapis::claude {
namespace {
constexpr qsizetype input_limit = qsizetype{1024} * 1024;
constexpr qsizetype frame_limit = qsizetype{16} * 1024;
constexpr int deadline_ms = 1000;
bool read_input(QByteArray& input, QDeadlineTimer& deadline) {
    std::array<char, 16384> chunk{};
    while (!deadline.hasExpired()) {
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(deadline.remainingTime()));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (static_cast<unsigned short>(descriptor.revents) &
                           static_cast<unsigned short>(POLLNVAL)) != 0)
            return false;
        const auto count = ::read(STDIN_FILENO, chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return false;
        if (count == 0)
            return true;
        if (input.size() + count > input_limit)
            return false;
        input.append(chunk.data(), static_cast<qsizetype>(count));
    }
    return false;
}
} // namespace
int run_hook_relay(const QString& socket, const QString& nonce) noexcept {
    // Hook failure must never become a Claude permission decision or prompt text.
    try {
        if (!socket.startsWith(QLatin1Char('/')) || socket.size() > 100 || nonce.size() != 36 ||
            socket.contains(QChar::Null))
            return 0;
        QDeadlineTimer deadline(deadline_ms);
        QByteArray input;
        if (!read_input(input, deadline))
            return 0;
        QJsonParseError error{};
        const auto source = QJsonDocument::fromJson(input, &error);
        if (error.error != QJsonParseError::NoError || !source.isObject())
            return 0;
        const QStringList fields{"hook_event_name", "session_id",        "prompt_id", "tool_name",
                                 "tool_use_id",     "notification_type", "source",    "reason"};
        QJsonObject event;
        const auto object = source.object();
        for (const auto& key : fields)
            if (object.contains(key))
                event.insert(key, object.value(key));
        const auto data = QJsonDocument(QJsonObject{{"nonce", nonce}, {"event", event}})
                              .toJson(QJsonDocument::Compact) +
                          '\n';
        if (data.size() > frame_limit || deadline.hasExpired())
            return 0;
        QLocalSocket connection;
        connection.connectToServer(socket);
        if (!connection.waitForConnected(static_cast<int>(deadline.remainingTime())))
            return 0;
        if (connection.write(data) != data.size())
            return 0;
        while (connection.bytesToWrite() && !deadline.hasExpired())
            if (!connection.waitForBytesWritten(static_cast<int>(deadline.remainingTime())))
                return 0;
        if (!deadline.hasExpired() && connection.bytesAvailable() == 0)
            static_cast<void>(
                connection.waitForReadyRead(static_cast<int>(deadline.remainingTime())));
        // The ACK is transport evidence only. Nothing is returned to Claude.
        static_cast<void>(connection.read(3));
    } catch (...) {
        // Outer hook process boundary: fail open without approving or denying.
        return 0;
    }
    return 0;
}
} // namespace lapis::claude
