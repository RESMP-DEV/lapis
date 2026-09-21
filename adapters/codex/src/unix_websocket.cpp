#include "unix_websocket.hpp"

#include <QCryptographicHash>
#include <QLocalSocket>
#include <QMap>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace lapis::codex {
namespace {
constexpr qsizetype header_limit = qsizetype{16} * 1024;
constexpr qsizetype queue_limit = 2 * UnixWebSocket::maximum_message_bytes;
constexpr auto websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
bool valid_utf8(const QByteArray& bytes) { return QString::fromUtf8(bytes).toUtf8() == bytes; }
bool token(const QByteArray& value, QByteArrayView wanted) {
    const auto values = value.toLower().split(',');
    return std::any_of(values.begin(), values.end(),
                       [&](const auto& part) { return part.trimmed() == wanted; });
}
} // namespace

class UnixWebSocket::Impl final : public QObject {
  public:
    explicit Impl(UnixWebSocket& owner) : owner_(owner) {
        deadline_.setSingleShot(true);
        deadline_.setInterval(5000);
        connect(&deadline_, &QTimer::timeout, this,
                [this] { fail("WebSocket connect timed out"); });
        close_deadline_.setSingleShot(true);
        close_deadline_.setInterval(2000);
        connect(&close_deadline_, &QTimer::timeout, this,
                [this] { fail("WebSocket close drain timed out"); });
    }
    void close() {
        ++generation_;
        deadline_.stop();
        if (socket_) {
            socket_->disconnect(this);
            socket_->abort();
            socket_->deleteLater();
            socket_ = nullptr;
            closing_ = false;
            close_deadline_.stop();
        }
        ready_ = false;
        fragmented_ = false;
        input_.clear();
        message_.clear();
    }
    void open(const QString& path) {
        close();
        socket_ = new QLocalSocket(this);
        socket_->setReadBufferSize(UnixWebSocket::maximum_message_bytes + header_limit + 14);
        const auto generation = generation_;
        connect(socket_, &QLocalSocket::bytesWritten, this, [this, generation](qint64) {
            if (generation == generation_)
                finish_close();
        });
        connect(socket_, &QLocalSocket::connected, this, [this, generation] {
            if (generation != generation_)
                return;
            QByteArray nonce(16, '\0');
            for (qsizetype offset = 0; offset < nonce.size(); offset += 4) {
                auto value = QRandomGenerator::system()->generate();
                for (qsizetype byte = 0; byte < 4; ++byte) {
                    nonce[offset + byte] = static_cast<char>(value & 0xffU);
                    value >>= 8U;
                }
            }
            const auto key = nonce.toBase64();
            accept_ =
                QCryptographicHash::hash(key + websocket_guid, QCryptographicHash::Sha1).toBase64();
            const auto request =
                QByteArrayLiteral("GET / HTTP/1.1\r\nHost: localhost\r\n"
                                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ") +
                key + "\r\n\r\n";
            if (socket_->write(request) != request.size())
                fail("WebSocket handshake write failed");
        });
        connect(socket_, &QLocalSocket::readyRead, this, [this, generation] {
            if (generation == generation_)
                receive();
        });
        connect(socket_, &QLocalSocket::disconnected, this, [this, generation] {
            if (generation == generation_)
                fail(closing_ ? "WebSocket peer closed" : "WebSocket disconnected");
        });
        connect(socket_, &QLocalSocket::errorOccurred, this,
                [this, generation](QLocalSocket::LocalSocketError) {
                    if (generation == generation_ && !closing_)
                        fail("WebSocket connection failed");
                });
        deadline_.start();
        socket_->connectToServer(path);
    }
    bool send(const QByteArray& text) {
        if (!ready_ || closing_ || text.size() > UnixWebSocket::maximum_message_bytes ||
            !valid_utf8(text))
            return false;
        return send_frame(1, text);
    }

  private:
    void fail(const char* reason) {
        close();
        emit owner_.failed(QString::fromLatin1(reason));
    }
    void finish_close() {
        if (!closing_ || !socket_ || socket_->bytesToWrite() != 0)
            return;
        fail("WebSocket peer closed");
    }
    bool send_frame(quint8 opcode, const QByteArray& payload) {
        if (!socket_ || socket_->state() != QLocalSocket::ConnectedState ||
            socket_->bytesToWrite() + payload.size() + 14 > queue_limit)
            return false;
        QByteArray frame;
        frame.append(static_cast<char>(0x80U | opcode));
        const auto size = static_cast<quint64>(payload.size());
        if (size < 126) {
            frame.append(static_cast<char>(0x80U | size));
        } else {
            const int count = size <= 65535 ? 2 : 8;
            frame.append(static_cast<char>(count == 2 ? 0xfe : 0xff));
            for (int index = count - 1; index >= 0; --index)
                frame.append(static_cast<char>(size >> (static_cast<unsigned int>(index) * 8U)));
        }
        QByteArray mask(4, '\0');
        const auto mask_value = QRandomGenerator::system()->generate();
        for (int index = 0; index < mask.size(); ++index)
            mask[index] =
                static_cast<char>((mask_value >> (static_cast<unsigned int>(index) * 8U)) & 0xffU);
        frame += mask;
        for (qsizetype index = 0; index < payload.size(); ++index)
            frame.append(static_cast<char>(static_cast<unsigned char>(payload[index]) ^
                                           static_cast<unsigned char>(mask[index % 4])));
        return socket_->write(frame) == frame.size();
    }
    bool handshake() {
        const auto end = input_.indexOf("\r\n\r\n");
        if (end < 0) {
            if (input_.size() > header_limit)
                fail("WebSocket handshake exceeded limit");
            return false;
        }
        if (end > header_limit) {
            fail("WebSocket handshake exceeded limit");
            return false;
        }
        const auto lines = input_.first(end).split('\n');
        const auto status = lines.first().trimmed().split(' ');
        if (status.size() < 2 || status[0] != "HTTP/1.1" || status[1] != "101") {
            fail("WebSocket upgrade rejected");
            return false;
        }
        QMap<QByteArray, QByteArray> headers;
        for (qsizetype index = 1; index < lines.size(); ++index) {
            const auto line = lines[index].trimmed();
            const auto colon = line.indexOf(':');
            if (colon <= 0 || headers.contains(line.first(colon).toLower())) {
                fail("Invalid WebSocket headers");
                return false;
            }
            headers.insert(line.first(colon).toLower(), line.mid(colon + 1).trimmed());
        }
        if (headers.value("sec-websocket-accept") != accept_ ||
            !token(headers.value("connection"), "upgrade") ||
            headers.value("upgrade").toLower() != "websocket" ||
            headers.contains("sec-websocket-extensions") ||
            headers.contains("sec-websocket-protocol")) {
            fail("Invalid WebSocket upgrade response");
            return false;
        }
        input_.remove(0, end + 4);
        deadline_.stop();
        ready_ = true;
        return true;
    }
    struct Header {
        quint8 opcode{};
        bool fin{};
        quint64 length{};
        qsizetype offset{};
    };
    std::optional<Header> read_header() {
        if (input_.size() < 2)
            return std::nullopt;
        const auto first = static_cast<quint8>(input_[0]);
        const auto second = static_cast<quint8>(input_[1]);
        Header header{static_cast<quint8>(first & 0x0fU), (first & 0x80U) != 0, second & 0x7fU, 2};
        const auto opcode = header.opcode;
        if ((first & 0x70U) != 0 || (second & 0x80U) != 0 ||
            (opcode != 0 && opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10))
            throw std::runtime_error("Invalid WebSocket frame");
        if (header.length >= 126) {
            const auto marker = header.length;
            const qsizetype count = marker == 126 ? 2 : 8;
            if (input_.size() < 2 + count)
                return std::nullopt;
            header.length = 0;
            for (qsizetype index = 0; index < count; ++index)
                header.length =
                    (header.length << 8U) | static_cast<quint8>(input_[header.offset++]);
            if ((marker == 126 && header.length < 126) ||
                (marker == 127 && (header.length <= 65535 || (header.length >> 63U) != 0)))
                throw std::runtime_error("Noncanonical WebSocket frame length");
        }
        return header;
    }
    void check_length(const Header& header) const {
        const bool control = header.opcode >= 8;
        if (header.length > static_cast<quint64>(UnixWebSocket::maximum_message_bytes) ||
            (control && (!header.fin || header.length > 125)) ||
            (!control && (header.length + static_cast<quint64>(message_.size()) >
                          static_cast<quint64>(UnixWebSocket::maximum_message_bytes))))
            throw std::runtime_error("WebSocket frame exceeded limit");
    }
    void deliver(const Header& header, QByteArray payload) {
        if (header.opcode == 8) {
            close_from_peer(payload);
            return;
        }
        if (header.opcode == 9) {
            if (!send_frame(10, payload))
                throw std::runtime_error("WebSocket pong queue unavailable");
            return;
        }
        if (header.opcode == 10)
            return;
        if ((header.opcode == 0) != fragmented_)
            throw std::runtime_error("Invalid WebSocket continuation");
        message_ += payload;
        fragmented_ = !header.fin;
        if (header.fin) {
            payload = std::move(message_);
            message_.clear();
            if (!valid_utf8(payload))
                throw std::runtime_error("Invalid WebSocket UTF-8");
            emit owner_.message(payload);
        }
    }
    void close_from_peer(const QByteArray& payload) {
        if (payload.size() == 1)
            throw std::runtime_error("Invalid WebSocket close frame");
        if (payload.size() >= 2) {
            const auto code = (static_cast<quint32>(static_cast<quint8>(payload[0])) << 8U) |
                              static_cast<quint8>(payload[1]);
            const bool valid_code =
                (code >= 1000 && code <= 1011 && code != 1004 && code != 1005 && code != 1006) ||
                (code >= 3000 && code <= 4999);
            if (!valid_code || (payload.size() > 2 && !valid_utf8(payload.mid(2))))
                throw std::runtime_error("Invalid WebSocket close frame");
        }
        closing_ = true;
        ready_ = false;
        if (!send_frame(8, payload))
            throw std::runtime_error("WebSocket close response queue unavailable");
        close_deadline_.start();
        finish_close();
    }
    void receive() {
        QPointer<Impl> alive(this);
        const auto generation = generation_;
        try {
            if (closing_)
                return;
            input_ += socket_->readAll();
            if (input_.size() > UnixWebSocket::maximum_message_bytes + header_limit + 14)
                throw std::runtime_error("WebSocket input exceeded limit");
            if (!ready_) {
                if (!handshake())
                    return;
                emit owner_.opened();
                if (!alive || generation != generation_ || closing_)
                    return;
            }
            // Bound each event-loop turn even when the server sends tiny frames.
            for (int budget = 0; budget < 128; ++budget) {
                const auto header = read_header();
                if (!header)
                    return;
                check_length(*header);
                if (input_.size() - header->offset < static_cast<qsizetype>(header->length))
                    return;
                auto payload = input_.mid(header->offset, static_cast<qsizetype>(header->length));
                input_.remove(0, header->offset + static_cast<qsizetype>(header->length));
                deliver(*header, std::move(payload));
                if (!alive || generation != generation_ || closing_)
                    return;
            }
            QTimer::singleShot(0, this, [this, generation] {
                if (generation == generation_)
                    receive();
            });
        } catch (const std::exception& error) {
            if (alive)
                fail(error.what());
        }
    }
    UnixWebSocket& owner_;
    QLocalSocket* socket_{}; // QObject-owned, retired with deleteLater on close.
    QTimer deadline_;
    QTimer close_deadline_;
    QByteArray accept_;
    QByteArray input_;
    QByteArray message_;
    quint64 generation_{};
    bool ready_{};
    bool fragmented_{};
    bool closing_{};
};

UnixWebSocket::UnixWebSocket(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this)) {}
UnixWebSocket::~UnixWebSocket() = default;
void UnixWebSocket::open(const QString& path) { impl_->open(path); }
void UnixWebSocket::close() { impl_->close(); }
bool UnixWebSocket::send(const QByteArray& text) { return impl_->send(text); }
} // namespace lapis::codex
