#include "transport/local_protocol.hpp"
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>

namespace {
void require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("Protocol expectation failed at line " +
                                 std::to_string(where.line()));
}
template <typename Operation>
void rejects(Operation operation, std::source_location where = std::source_location::current()) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, where);
}
void identity_messages() {
    using namespace lapis::session;
    const QByteArray session = wire::new_id();
    const QByteArray epoch = wire::new_id();
    const QByteArray fingerprint(32, 'F');
    require(session.size() == 16 && epoch.size() == 16 && wire::new_id() != session);
    const wire::SessionIdentity identity{session, epoch};
    const wire::Attachment attachment{identity, quint64{0x0102030405060708}};

    const QByteArray attach = wire::encode_attach(
        {.mode = wire::AttachMode::reconnect, .fingerprint = fingerprint, .expected = identity});
    require(attach.size() == 69 && attach.left(4) == QByteArrayLiteral("\0\0\0\6"));
    const auto decoded_attach = wire::decode_attach(attach);
    require(decoded_attach.mode == wire::AttachMode::reconnect &&
            decoded_attach.fingerprint == fingerprint && decoded_attach.expected == identity);
    for (auto mode : {wire::AttachMode::discover, wire::AttachMode::create}) {
        const wire::SessionIdentity expected = mode == wire::AttachMode::discover
                                                   ? wire::SessionIdentity{}
                                                   : wire::SessionIdentity{session, {}};
        const QByteArray encoded =
            wire::encode_attach({.mode = mode, .fingerprint = fingerprint, .expected = expected});
        const auto decoded = wire::decode_attach(encoded);
        require(decoded.mode == mode && decoded.fingerprint == fingerprint &&
                decoded.expected == expected);
    }
    rejects([&] {
        static_cast<void>(wire::encode_attach({.mode = wire::AttachMode::reconnect,
                                               .fingerprint = fingerprint,
                                               .expected = wire::SessionIdentity{}}));
    });
    rejects([&] {
        static_cast<void>(wire::encode_attach(
            {.mode = wire::AttachMode::create, .fingerprint = fingerprint, .expected = identity}));
    });
    rejects([&] { static_cast<void>(wire::decode_attach(attach + 'x')); });
    rejects([&] { static_cast<void>(wire::decode_attach(attach.chopped(1))); });
    auto bad_version = attach;
    bad_version[3] = char{5};
    rejects([&] { static_cast<void>(wire::decode_attach(bad_version)); });

    const wire::Hello hello{attachment, quint64{0x0a0b0c0d0e0f1011}};
    const QByteArray encoded_hello = wire::encode_hello(hello);
    require(encoded_hello.size() == 52 &&
            encoded_hello.mid(36, 8) == QByteArrayLiteral("\1\2\3\4\5\6\7\10") &&
            encoded_hello.right(8) == QByteArrayLiteral("\12\13\14\15\16\17\20\21"));
    const auto decoded_hello = wire::decode_hello(encoded_hello);
    require(decoded_hello.attachment == attachment && decoded_hello.pid == hello.pid);
    rejects([&] { static_cast<void>(wire::encode_hello({attachment, 0})); });
    auto bad_pid = encoded_hello;
    bad_pid.truncate(51);
    rejects([&] { static_cast<void>(wire::decode_hello(bad_pid)); });

    const wire::Ready ready{attachment, quint64{0x1122334455667788}};
    const QByteArray encoded_ready = wire::encode_ready(ready);
    require(encoded_ready.size() == 48 &&
            encoded_ready.mid(32, 8) == QByteArrayLiteral("\1\2\3\4\5\6\7\10") &&
            encoded_ready.right(8) == QByteArrayLiteral("\21\"3DUfw\210"));
    const auto decoded_ready = wire::decode_ready(encoded_ready);
    require(decoded_ready.attachment == attachment && decoded_ready.sequence == ready.sequence);
    rejects([&] { static_cast<void>(wire::encode_ready({attachment, 0})); });
    rejects([&] { static_cast<void>(wire::decode_ready(encoded_ready + 'x')); });
}
void history_messages() {
    using namespace lapis::session;
    const wire::Attachment attachment{{wire::new_id(), wire::new_id()}, 12};
    const wire::HistoryRequest request{17, 23, wire::HistoryDirection::newer};
    const auto bytes = wire::encode_history_request(request);
    const auto decoded = wire::decode_history_request(bytes);
    require(decoded.request_id == 17 && decoded.reference == 23 &&
            decoded.direction == wire::HistoryDirection::newer);
    rejects([&] { static_cast<void>(wire::decode_history_request(bytes.chopped(1))); });
    rejects([&] { static_cast<void>(wire::decode_history_request(bytes + 'x')); });
    auto malformed = bytes;
    malformed[16] = 2;
    rejects([&] { static_cast<void>(wire::decode_history_request(malformed)); });
    rejects([&] { static_cast<void>(wire::encode_history_request({0, 0})); });
    rejects([&] {
        static_cast<void>(wire::encode_history_request({1, 0, wire::HistoryDirection::newer}));
    });
    Terminal terminal({20, 4});
    terminal.feed("old \x1b[31m界é\x1b[0m");
    const wire::HistoryReply reply{attachment, 17, 99, QStringLiteral("Archived page"),
                                   terminal.snapshot()};
    const auto encoded = wire::encode_history_reply(reply);
    const auto restored = wire::decode_history_reply(encoded);
    require(restored.attachment == attachment && restored.request_id == 17 &&
            restored.page_id == 99);
    require(restored.snapshot &&
            wire::encode_snapshot(*restored.snapshot) == wire::encode_snapshot(*reply.snapshot));
    for (qsizetype size = 0; size < encoded.size(); ++size)
        rejects([&] { static_cast<void>(wire::decode_history_reply(encoded.first(size))); });
    rejects([&] { static_cast<void>(wire::decode_history_reply(encoded + 'x')); });
    const auto empty =
        wire::encode_history_reply({attachment, 18, 0, QStringLiteral("No older history"), {}});
    require(!wire::decode_history_reply(empty).snapshot);
    rejects([&] { static_cast<void>(wire::decode_history_reply(empty + 'x')); });
    rejects([&] { static_cast<void>(wire::encode_history_reply({attachment, 18, 1, {}, {}})); });
    auto oversized = encoded;
    oversized[56] = static_cast<char>(0xff);
    rejects([&] { static_cast<void>(wire::decode_history_reply(oversized)); });
}
void envelope_messages() {
    using namespace lapis::session;
    const wire::Attachment attachment{{wire::new_id(), wire::new_id()}, 1};
    Terminal terminal({20, 4});
    const auto decoded = terminal.snapshot();
    const auto encoded = wire::encode_snapshot(decoded);
    const wire::SnapshotMessage snapshot_message{attachment, quint64{7}, decoded, {100, 110, 120}};
    const QByteArray encoded_snapshot_message = wire::encode_snapshot_message(snapshot_message);
    const auto decoded_snapshot_message = wire::decode_snapshot_message(encoded_snapshot_message);
    require(decoded_snapshot_message.timing.pty_read_ns == 100 &&
            decoded_snapshot_message.timing.parse_end_ns == 110 &&
            decoded_snapshot_message.timing.publish_ns == 120);
    require(encoded_snapshot_message.size() == wire::snapshot_header_bytes + encoded.size());
    require(decoded_snapshot_message.attachment == attachment &&
            decoded_snapshot_message.sequence == 7 &&
            wire::encode_snapshot(decoded_snapshot_message.snapshot) == encoded);
    rejects([&] { static_cast<void>(wire::encode_snapshot_message({attachment, 0, decoded})); });
    rejects(
        [&] { static_cast<void>(wire::decode_snapshot_message(encoded_snapshot_message + 'x')); });

    const QByteArray payload = QByteArrayLiteral("input");
    for (const auto kind :
         {wire::Kind::text, wire::Kind::paste, wire::Kind::key, wire::Kind::resize}) {
        const wire::ControlMessage message{attachment, payload};
        require(wire::frame(kind, wire::encode_control(message))[4] == static_cast<char>(kind));
        const QByteArray encoded_control = wire::encode_control(message);
        const auto decoded_control = wire::decode_control(encoded_control);
        require(encoded_control.size() == 45 && decoded_control.attachment == attachment &&
                decoded_control.payload == payload);
    }
    const wire::ControlMessage oversized{attachment, QByteArray(wire::max_codepoints + 1, 'x')};
    rejects([&] { static_cast<void>(wire::encode_control(oversized)); });
    rejects([&] { static_cast<void>(wire::decode_control(QByteArray(41, '\0'))); });

    for (const auto& [status, encoded_status] :
         std::initializer_list<std::pair<wire::Status, QByteArray>>{
             {{wire::StatusCode::ended, QStringLiteral("ended")}, QByteArrayLiteral("\2ended")},
             {{wire::StatusCode::overloaded, {}}, QByteArrayLiteral("\4")}}) {
        const auto decoded_status = wire::decode_status(encoded_status);
        require(wire::encode_status(status) == encoded_status &&
                decoded_status.code == status.code && decoded_status.message == status.message);
    }
    rejects([&] { static_cast<void>(wire::decode_status(QByteArrayLiteral("\1\xff"))); });
    rejects([&] { static_cast<void>(wire::decode_status(QByteArray(4098, 'x'))); });
    require(wire::frame(wire::Kind::ready, wire::encode_ready({attachment, 1})).size() == 53);
}
} // namespace
int main() {
    using namespace lapis::session;
    try {
        identity_messages();
        envelope_messages();
        history_messages();
        Terminal terminal({20, 4});
        terminal.feed("A界é\x1b[1;31mZ\x1b[0m\x1b[?2004h");
        const auto expected = terminal.snapshot();
        const auto encoded = wire::encode_snapshot(expected);
        const auto decoded = wire::decode_snapshot(encoded);
        require(wire::encode_snapshot(decoded) == encoded);
        require(decoded.size == expected.size && decoded.bracketed_paste);
        require(decoded.graphemes == expected.graphemes);
        require(decoded.cells.at(4).style == expected.cells.at(4).style);

        const auto packet = wire::frame(wire::Kind::snapshot, encoded);
        QByteArray buffer;
        wire::Frame frame;
        for (qsizetype i = 0; i < packet.size() - 1; ++i) {
            buffer += packet[i];
            require(!wire::take_frame(buffer, frame));
        }
        buffer += packet.back();
        buffer += wire::frame(wire::Kind::text, "next");
        qsizetype consumed{};
        require(wire::take_frame(buffer, frame) && frame.kind == wire::Kind::snapshot &&
                frame.payload == encoded);
        require(consumed == 0);
        require(wire::take_frame(buffer, frame) && frame.kind == wire::Kind::text &&
                frame.payload == "next" && buffer.isEmpty());
        buffer = wire::frame(wire::Kind::text, "a") + wire::frame(wire::Kind::text, "b");
        require(wire::take_frame(buffer, consumed, frame) && frame.payload == "a" &&
                consumed == 6 && buffer.size() == 12);
        require(wire::take_frame(buffer, consumed, frame) && frame.payload == "b" &&
                consumed == 0 && buffer.isEmpty());
        rejects([&] { static_cast<void>(wire::decode_snapshot(encoded.chopped(1))); });
        rejects([&] { static_cast<void>(wire::decode_snapshot(encoded + 'x')); });
        QByteArray batch;
        for (int index = 0; index < 1000; ++index)
            batch += wire::frame(wire::Kind::text, QByteArray::number(index));
        const auto tail = wire::frame(wire::Kind::text, QByteArrayLiteral("tail"));
        batch += tail.first(3);
        consumed = 0;
        for (int index = 0; index < 1000; ++index) {
            require(wire::take_frame(batch, consumed, frame));
            require(frame.kind == wire::Kind::text && frame.payload == QByteArray::number(index));
        }
        require(!wire::take_frame(batch, consumed, frame));
        batch += tail.sliced(3);
        require(wire::take_frame(batch, consumed, frame));
        require(frame.payload == "tail");
        require(!wire::take_frame(batch, consumed, frame));

        auto invalid = expected;
        invalid.cells.front().text_offset = 100000;
        rejects([&] { static_cast<void>(wire::decode_snapshot(wire::encode_snapshot(invalid))); });
        invalid = expected;
        invalid.graphemes.front() = char32_t{0xd800};
        rejects([&] { static_cast<void>(wire::decode_snapshot(wire::encode_snapshot(invalid))); });
        invalid = expected;
        invalid.cells.pop_back();
        rejects([&] { static_cast<void>(wire::encode_snapshot(invalid)); });
        const QByteArray framed = wire::frame(wire::Kind::text, QByteArrayLiteral("x"));
        require(framed.size() == 6);
        for (const auto* hex : {"00000000", "00800001", "0000000100"}) {
            auto malformed = QByteArray::fromHex(hex);
            rejects([&] { static_cast<void>(wire::take_frame(malformed, frame)); });
        }
        std::cout << "Snapshot, fragmented/coalesced frames and malformed input passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
