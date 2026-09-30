#include "hook_relay.hpp"
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <algorithm>
#include <array>
#include <cerrno>
#include <optional>
#include <poll.h>
#include <unistd.h>

namespace lapis::claude {
namespace {
constexpr qsizetype input_limit = qsizetype{1024} * 1024;
std::optional<int> active_tasks(const QJsonArray& tasks) {
    int count{};
    for (const auto& task : tasks) {
        const auto status = task.toObject().value(QStringLiteral("status")).toString();
        if (status == QLatin1String("running") || status == QLatin1String("pending"))
            ++count;
        else if (status != QLatin1String("completed") && status != QLatin1String("failed") &&
                 status != QLatin1String("killed") && status != QLatin1String("cancelled"))
            return std::nullopt;
    }
    return count;
}
QString background_work(const QJsonObject& object) {
    if (object.value(QStringLiteral("hook_event_name")) != QLatin1String("Stop"))
        return {};
    if (!object.contains(QStringLiteral("background_tasks")) &&
        !object.contains(QStringLiteral("session_crons")))
        return QStringLiteral("legacy");
    const auto tasks = object.value(QStringLiteral("background_tasks"));
    const auto wakeups = object.value(QStringLiteral("session_crons"));
    if (!tasks.isArray() || !wakeups.isArray())
        return QStringLiteral("unknown");
    const auto active = active_tasks(tasks.toArray());
    const auto crons = wakeups.toArray();
    const bool valid_crons = std::all_of(crons.begin(), crons.end(),
                                         [](const QJsonValue& cron) { return cron.isObject(); });
    return active && valid_crons ? QString::number(*active + crons.size())
                                 : QStringLiteral("unknown");
}
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
        QDeadlineTimer deadline(relay_deadline_ms);
        QByteArray input;
        if (!read_input(input, deadline))
            return 0;
        QJsonParseError error{};
        const auto source = QJsonDocument::fromJson(input, &error);
        if (error.error != QJsonParseError::NoError || !source.isObject())
            return 0;
        QJsonObject event;
        const auto object = source.object();
        for (const auto& key : relay_identity_fields)
            if (object.contains(key))
                event.insert(key, object.value(key));
        const auto in_flight = background_work(object);
        if (!in_flight.isNull())
            event.insert(relay_derived_fields.front().toString(), in_flight);
        QJsonObject frame{{"nonce", nonce}, {"event", event}};
        const auto data = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
        if (data.size() > relay_frame_limit || deadline.hasExpired())
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
