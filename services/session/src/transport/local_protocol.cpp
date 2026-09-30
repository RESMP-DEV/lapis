#include "local_protocol.hpp"
#include "wire_bytes.hpp"
#include <QDataStream>
#include <QIODevice>
#include <QStringConverter>
#include <QUuid>
#include <limits>
#include <stdexcept>

namespace lapis::session::wire {
using namespace bytes;
namespace {
void check(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid local session message");
}
constexpr quint32 link_extension = 0x4c4e4b31U; // LNK1
constexpr quint8 attach_link_capability = 0x80U;
constexpr quint8 attach_phase_capability = 0x40U;
constexpr quint8 attach_paste_capability = 0x20U;
void validate_links(const TerminalSnapshot& snapshot) {
    check(snapshot.hyperlinks.size() <= max_hyperlink_spans);
    std::size_t end = 0;
    std::size_t bytes = 0;
    for (const auto& link : snapshot.hyperlinks) {
        check(link.first_cell >= end && link.first_cell < snapshot.cells.size() &&
              link.cell_count > 0 && link.cell_count <= snapshot.cells.size() - link.first_cell);
        check(!link.uri.empty() && link.uri.size() <= max_hyperlink_uri_bytes &&
              link.uri.size() <= max_hyperlink_bytes - bytes);
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        static_cast<void>(QString(
            decoder(QByteArrayView(link.uri.data(), static_cast<qsizetype>(link.uri.size())))));
        check(!decoder.hasError());
        bytes += link.uri.size();
        end = static_cast<std::size_t>(link.first_cell) + link.cell_count;
    }
}
void write_color(QDataStream& out, TerminalColor color) {
    out << static_cast<quint8>(color.kind) << static_cast<quint32>(color.value);
}
TerminalColor read_color(QDataStream& in) {
    quint8 kind{};
    quint32 value{};
    in >> kind >> value;
    check(kind <= static_cast<quint8>(ColorKind::rgb));
    check(kind != static_cast<quint8>(ColorKind::indexed) || value < 256);
    check(value <= 0xffffffU);
    return {static_cast<ColorKind>(kind), value};
}
quint16 style_flags(const TerminalStyle& style) {
    return static_cast<quint16>((style.bold ? 1U : 0U) | (style.italic ? 2U : 0U) |
                                (style.faint ? 4U : 0U) | (style.blink ? 8U : 0U) |
                                (style.inverse ? 16U : 0U) | (style.invisible ? 32U : 0U) |
                                (style.strikethrough ? 64U : 0U) | (style.overline ? 128U : 0U));
}
} // namespace
bool valid_identity(const SessionIdentity& identity) {
    return identity.session_id.size() == 16 && identity.epoch.size() == 16 &&
           identity.session_id != QByteArray(16, '\0') && identity.epoch != QByteArray(16, '\0');
}
QByteArray new_id() {
    const QUuid identifier = QUuid::createUuid();
    check(!identifier.isNull());
    QByteArray result = identifier.toRfc4122();
    check(result.size() == 16 && result != QByteArray(16, '\0'));
    return result;
}
QByteArray encode_attach(const AttachRequest& request) {
    check(request.fingerprint.size() == 32);
    auto expected = request.expected;
    if (expected.session_id.isEmpty())
        expected.session_id = QByteArray(16, '\0');
    if (expected.epoch.isEmpty())
        expected.epoch = QByteArray(16, '\0');
    const auto mode = static_cast<quint8>(request.mode);
    check(mode <= static_cast<quint8>(AttachMode::join));
    if (request.mode == AttachMode::discover || request.mode == AttachMode::join)
        check(expected.session_id == QByteArray(16, '\0') &&
              expected.epoch == QByteArray(16, '\0'));
    else if (request.mode == AttachMode::reconnect)
        check(valid_identity(expected));
    else
        check(expected.session_id.size() == 16 && expected.session_id != QByteArray(16, '\0') &&
              expected.epoch == QByteArray(16, '\0'));
    QByteArray result;
    append_quint32(result, version);
    result += request.fingerprint;
    result.append(static_cast<char>(mode | (request.hyperlinks ? attach_link_capability : 0U) |
                                    (request.attention_phase ? attach_phase_capability : 0U) |
                                    (request.paste_transactions ? attach_paste_capability : 0U)));
    result += expected.session_id;
    result += expected.epoch;
    return result;
}
AttachRequest decode_attach(const QByteArray& payload) {
    check(payload.size() == 69);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    check(read_quint32(cursor) == version);
    AttachRequest result;
    result.fingerprint = raw_bytes(cursor, 32);
    const auto flags = *cursor++;
    result.hyperlinks = (flags & attach_link_capability) != 0;
    result.attention_phase = (flags & attach_phase_capability) != 0;
    result.paste_transactions = (flags & attach_paste_capability) != 0;
    const auto mode = flags & 0x1fU;
    check(mode <= static_cast<quint8>(AttachMode::join));
    result.mode = static_cast<AttachMode>(mode);
    result.expected.session_id = raw_bytes(cursor, 16);
    result.expected.epoch = raw_bytes(cursor, 16);
    check(cursor == reinterpret_cast<const unsigned char*>(payload.constData()) + payload.size());
    const bool anonymous = result.mode == AttachMode::discover || result.mode == AttachMode::join;
    if (anonymous)
        check(result.expected.session_id == QByteArray(16, '\0') &&
              result.expected.epoch == QByteArray(16, '\0'));
    else if (result.mode == AttachMode::reconnect)
        check(valid_identity(result.expected));
    else
        check(result.expected.session_id != QByteArray(16, '\0') &&
              result.expected.epoch == QByteArray(16, '\0'));
    if (anonymous)
        result.expected.session_id.clear();
    if (result.mode != AttachMode::reconnect)
        result.expected.epoch.clear();
    return result;
}
QByteArray encode_hello(const Hello& hello) {
    check(hello.pid != 0);
    QByteArray result;
    append_quint32(result, version);
    result += encode_attachment(hello.attachment);
    append_quint64(result, hello.pid);
    if (hello.paste_transactions)
        append_quint32(result, 1);
    return result;
}
Hello decode_hello(const QByteArray& payload) {
    check(payload.size() == 52 || payload.size() == 56);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    check(read_quint32(cursor) == version);
    Hello result;
    result.attachment = decode_attachment(cursor);
    result.pid = read_quint64(cursor);
    if (payload.size() == 56) {
        check(read_quint32(cursor) == 1);
        result.paste_transactions = true;
    }
    check(result.pid != 0 &&
          cursor == reinterpret_cast<const unsigned char*>(payload.constData()) + payload.size());
    return result;
}
QByteArray encode_paste_request(const PasteRequest& request) {
    check(request.request_id != 0 && request.text.size() <= max_paste_bytes);
    auto bytes = encode_attachment(request.attachment);
    append_quint64(bytes, request.request_id);
    bytes.append(request.submit ? char{1} : char{0});
    bytes += request.text;
    return bytes;
}
PasteRequest decode_paste_request(const QByteArray& payload) {
    check(payload.size() >= 49 && payload.size() <= 49 + max_paste_bytes);
    const auto* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    PasteRequest request;
    request.attachment = decode_attachment(cursor);
    request.request_id = read_quint64(cursor);
    const auto submit = *cursor;
    check(request.request_id != 0 && submit <= 1);
    request.submit = submit != 0;
    request.text = payload.sliced(49);
    return request;
}
QByteArray encode_paste_result(const PasteResult& result) {
    const auto message = result.message.toUtf8();
    check(result.request_id != 0 && message.size() <= 4096);
    auto bytes = encode_attachment(result.attachment);
    append_quint64(bytes, result.request_id);
    bytes.append(result.queued ? char{1} : char{0});
    bytes += message;
    return bytes;
}
PasteResult decode_paste_result(const QByteArray& payload) {
    check(payload.size() >= 49 && payload.size() <= 49 + 4096);
    const auto* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    PasteResult result;
    result.attachment = decode_attachment(cursor);
    result.request_id = read_quint64(cursor);
    const auto queued = *cursor;
    check(result.request_id != 0 && queued <= 1);
    result.queued = queued != 0;
    const auto message = payload.sliced(49);
    result.message = QString::fromUtf8(message);
    check(result.message.toUtf8() == message);
    return result;
}
QByteArray encode_snapshot_message(const SnapshotMessage& message, bool hyperlinks) {
    check(message.sequence != 0);
    QByteArray result = encode_attachment(message.attachment);
    append_quint64(result, message.sequence);
    append_quint64(result, message.timing.pty_read_ns);
    append_quint64(result, message.timing.parse_end_ns);
    append_quint64(result, message.timing.publish_ns);
    result += encode_snapshot(message.snapshot, hyperlinks);
    return result;
}
SnapshotMessage decode_snapshot_message(const QByteArray& payload) {
    check(payload.size() > snapshot_header_bytes);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    SnapshotMessage result;
    result.attachment = decode_attachment(cursor);
    result.sequence = read_quint64(cursor);
    check(result.sequence != 0);
    result.timing.pty_read_ns = read_quint64(cursor);
    result.timing.parse_end_ns = read_quint64(cursor);
    result.timing.publish_ns = read_quint64(cursor);
    result.snapshot = decode_snapshot(payload.sliced(snapshot_header_bytes));
    return result;
}
SnapshotEnvelope decode_snapshot_envelope(const QByteArray& payload) {
    // The screen's revision (8 bytes), then its columns and rows.
    check(payload.size() > snapshot_header_bytes + 12);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    SnapshotEnvelope result;
    result.attachment = decode_attachment(cursor);
    result.sequence = read_quint64(cursor);
    check(result.sequence != 0);
    result.timing.pty_read_ns = read_quint64(cursor);
    result.timing.parse_end_ns = read_quint64(cursor);
    result.timing.publish_ns = read_quint64(cursor);
    const auto* size = cursor + 8;
    result.size = {static_cast<std::uint16_t>((unsigned{size[0]} << 8U) | size[1]),
                   static_cast<std::uint16_t>((unsigned{size[2]} << 8U) | size[3])};
    check(result.size.columns > 0 && result.size.rows > 0 &&
          quint32(result.size.columns) * result.size.rows <= max_cells);
    result.encoded = payload.sliced(snapshot_header_bytes);
    return result;
}
QByteArray encode_history_request(const HistoryRequest& request) {
    check(request.request_id != 0 && request.direction <= HistoryDirection::at);
    check(request.direction != HistoryDirection::newer || request.reference != 0);
    QByteArray bytes;
    append_quint64(bytes, request.request_id);
    append_quint64(bytes, request.reference);
    bytes.append(static_cast<char>(request.direction));
    return bytes;
}
HistoryRequest decode_history_request(const QByteArray& payload) {
    check(payload.size() == 17);
    const auto* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    HistoryRequest result;
    result.request_id = read_quint64(cursor);
    result.reference = read_quint64(cursor);
    result.direction = static_cast<HistoryDirection>(*cursor);
    static_cast<void>(encode_history_request(result));
    return result;
}
QByteArray encode_history_reply(const HistoryReply& reply, bool hyperlinks) {
    check(reply.request_id != 0 && (reply.snapshot.has_value() == (reply.page_id != 0)));
    const auto message = reply.message.toUtf8();
    check(message.size() <= 4096);
    auto bytes = encode_attachment(reply.attachment);
    append_quint64(bytes, reply.request_id);
    append_quint64(bytes, reply.page_id);
    append_quint32(bytes, static_cast<quint32>(message.size()));
    bytes += message;
    if (reply.snapshot)
        bytes += encode_snapshot(*reply.snapshot, hyperlinks);
    check(bytes.size() + 1 <= max_frame_bytes);
    return bytes;
}
HistoryReply decode_history_reply(const QByteArray& payload) {
    check(payload.size() >= 60 && payload.size() + 1 <= max_frame_bytes);
    const auto* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    HistoryReply result;
    result.attachment = decode_attachment(cursor);
    result.request_id = read_quint64(cursor);
    result.page_id = read_quint64(cursor);
    const auto length = read_quint32(cursor);
    check(result.request_id != 0 && length <= 4096 && payload.size() >= 60 + length);
    const auto message = raw_bytes(cursor, length);
    result.message = QString::fromUtf8(message);
    check(result.message.toUtf8() == message);
    const auto tail = payload.sliced(60 + length);
    if (result.page_id != 0)
        result.snapshot = decode_snapshot(tail);
    else
        check(tail.isEmpty());
    return result;
}
QByteArray encode_control(const ControlMessage& message) {
    check(message.payload.size() <= max_codepoints);
    QByteArray result = encode_attachment(message.attachment);
    result += message.payload;
    return result;
}
ControlMessage decode_control(const QByteArray& payload) {
    check(payload.size() >= 40 && payload.size() <= 40 + max_codepoints);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    ControlMessage result;
    result.attachment = decode_attachment(cursor);
    result.payload = payload.sliced(40);
    return result;
}
QByteArray encode_ready(const Ready& ready) {
    check(ready.sequence != 0);
    QByteArray result = encode_attachment(ready.attachment);
    append_quint64(result, ready.sequence);
    return result;
}
Ready decode_ready(const QByteArray& payload) {
    check(payload.size() == 48);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    Ready result;
    result.attachment = decode_attachment(cursor);
    result.sequence = read_quint64(cursor);
    check(result.sequence != 0 &&
          cursor == reinterpret_cast<const unsigned char*>(payload.constData()) + payload.size());
    return result;
}
QByteArray encode_status(const Status& status) {
    const auto code = static_cast<quint8>(status.code);
    check(code >= static_cast<quint8>(StatusCode::rejected) &&
          code <= static_cast<quint8>(StatusCode::overloaded));
    const QByteArray message = status.message.toUtf8();
    check(message.size() <= 4096);
    QByteArray result;
    result.append(static_cast<char>(code));
    result += message;
    return result;
}
Status decode_status(const QByteArray& payload) {
    check(payload.size() >= 1 && payload.size() <= 4097);
    const auto code = static_cast<quint8>(payload.front());
    check(code >= static_cast<quint8>(StatusCode::rejected) &&
          code <= static_cast<quint8>(StatusCode::overloaded));
    const QByteArray message = payload.sliced(1);
    QString text = QString::fromUtf8(message);
    check(text.toUtf8() == message);
    return {static_cast<StatusCode>(code), std::move(text)};
}
QByteArray frame(Kind kind, const QByteArray& payload) {
    check(kind >= Kind::hello && kind <= Kind::paste_result);
    check(payload.size() + 1 <= max_frame_bytes);
    QByteArray result;
    QDataStream out(&result, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << static_cast<quint32>(payload.size() + 1) << static_cast<quint8>(kind);
    result += payload;
    return result;
}
bool take_frame(QByteArray& buffer, Frame& result) {
    qsizetype consumed{};
    const bool taken = take_frame(buffer, consumed, result);
    if (taken)
        buffer.remove(0, consumed);
    return taken;
}
bool take_frame(QByteArray& buffer, qsizetype& consumed, Frame& result) {
    if (consumed < 0 || consumed > buffer.size())
        throw std::runtime_error("Invalid local session read offset");
    const auto available = buffer.size() - consumed;
    if (available < 4)
        return false;
    const auto* header = reinterpret_cast<const unsigned char*>(buffer.constData()) + consumed;
    const quint32 size = (quint32{header[0]} << 24U) | (quint32{header[1]} << 16U) |
                         (quint32{header[2]} << 8U) | quint32{header[3]};
    check(size >= 1 && size <= max_frame_bytes);
    if (available < static_cast<qsizetype>(size) + 4)
        return false;
    const auto kind = static_cast<quint8>(header[4]);
    check(kind >= static_cast<quint8>(Kind::hello) &&
          kind <= static_cast<quint8>(Kind::paste_result));
    result = {static_cast<Kind>(kind), buffer.mid(consumed + 5, static_cast<qsizetype>(size) - 1)};
    consumed += static_cast<qsizetype>(size) + 4;
    if (consumed > buffer.size() / 2) {
        buffer.remove(0, consumed);
        consumed = 0;
    }
    return true;
}
// The alternate-screen byte: 1 on the alternate screen, and bit 2 beside it
// when this service takes wheel input. Readers before wheel input read any
// nonzero byte as true, so the bit is only ever set with the first.
namespace {
quint8 alternate_byte(const TerminalSnapshot& s) { return s.alternate_screen ? 3U : 0U; }
} // namespace

QByteArray encode_wheel(const Wheel& wheel) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << wheel.steps << wheel.column << wheel.row;
    check(out.status() == QDataStream::Ok);
    return bytes;
}
Wheel decode_wheel(const QByteArray& payload) {
    check(payload.size() == 6);
    QDataStream in(payload);
    in.setVersion(QDataStream::Qt_6_0);
    Wheel wheel;
    in >> wheel.steps >> wheel.column >> wheel.row;
    check(in.status() == QDataStream::Ok && wheel.steps != 0);
    return wheel;
}

QByteArray encode_snapshot(const TerminalSnapshot& s, bool hyperlinks) {
    check(s.cells.size() <= max_cells && s.graphemes.size() <= max_codepoints);
    check(static_cast<std::size_t>(s.size.columns) * s.size.rows == s.cells.size());
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << quint64(s.revision) << quint16(s.size.columns) << quint16(s.size.rows)
        << quint16(s.cursor.column) << quint16(s.cursor.row) << s.cursor.in_viewport
        << s.cursor.visible << s.cursor.blinking << s.cursor.wide_tail << quint8(s.cursor.shape)
        << alternate_byte(s) << s.bracketed_paste << s.application_cursor_keys
        << quint32(s.foreground_rgb) << quint32(s.background_rgb) << s.cursor_rgb.has_value()
        << quint32(s.cursor_rgb.value_or(0)) << quint64(s.history.total_rows)
        << quint64(s.history.viewport_offset) << quint64(s.history.viewport_rows);
    for (auto rgb : s.palette)
        out << quint32(rgb);
    out << quint32(s.graphemes.size());
    for (auto point : s.graphemes)
        out << quint32(point);
    out << quint32(s.cells.size());
    for (const auto& cell : s.cells) {
        out << quint32(cell.text_offset) << quint32(cell.text_length) << quint8(cell.kind);
        write_color(out, cell.style.foreground);
        write_color(out, cell.style.background);
        write_color(out, cell.style.underline_color);
        out << quint8(cell.style.underline) << style_flags(cell.style);
    }
    if (hyperlinks && !s.hyperlinks.empty()) {
        validate_links(s);
        out << link_extension << quint32(s.hyperlinks.size());
        for (const auto& link : s.hyperlinks) {
            out << quint32(link.first_cell) << quint32(link.cell_count) << quint32(link.uri.size());
            check(out.writeRawData(link.uri.data(), static_cast<qint64>(link.uri.size())) ==
                  static_cast<qint64>(link.uri.size()));
        }
    }
    check(out.status() == QDataStream::Ok);
    return bytes;
}
TerminalSnapshot decode_snapshot(const QByteArray& bytes) {
    QDataStream in(bytes);
    in.setVersion(QDataStream::Qt_6_0);
    TerminalSnapshot s;
    quint64 revision{}, total{}, offset{}, rows{};
    quint16 columns{}, height{}, x{}, y{};
    quint8 shape{};
    bool cursor_color{};
    quint32 cursor_rgb{};
    quint8 alternate{};
    in >> revision >> columns >> height >> x >> y >> s.cursor.in_viewport >> s.cursor.visible >>
        s.cursor.blinking >> s.cursor.wide_tail >> shape >> alternate >> s.bracketed_paste >>
        s.application_cursor_keys >> s.foreground_rgb >> s.background_rgb >> cursor_color >>
        cursor_rgb >> total >> offset >> rows;
    check(alternate <= 3U && alternate != 2U); // the wheel bit only appears with the screen bit
    s.alternate_screen = alternate != 0U;
    s.accepts_wheel = (alternate & 2U) != 0U;
    check(columns > 0 && height > 0 && quint32(columns) * height <= max_cells);
    check(!s.cursor.in_viewport || (x < columns && y < height));
    check(offset <= total && rows <= total - offset);
    check(shape <= static_cast<quint8>(CursorShape::hollow_block));
    s.revision = revision;
    s.size = {columns, height};
    s.cursor.column = x;
    s.cursor.row = y;
    s.cursor.shape = static_cast<CursorShape>(shape);
    if (cursor_color)
        s.cursor_rgb = cursor_rgb;
    s.history = {static_cast<std::size_t>(total), static_cast<std::size_t>(offset),
                 static_cast<std::size_t>(rows), !s.alternate_screen};
    for (auto& rgb : s.palette)
        in >> rgb;
    quint32 count{};
    in >> count;
    check(count <= max_codepoints);
    s.graphemes.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        quint32 point{};
        in >> point;
        check(point <= 0x10ffffU && (point < 0xd800U || point > 0xdfffU));
        s.graphemes.push_back(static_cast<char32_t>(point));
    }
    in >> count;
    check(count == quint32(columns) * height);
    s.cells.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        TerminalCell cell;
        quint8 kind{}, underline{};
        quint16 flags{};
        in >> cell.text_offset >> cell.text_length >> kind;
        check(kind <= static_cast<quint8>(CellKind::wrap_spacer));
        check(cell.text_offset <= s.graphemes.size() &&
              cell.text_length <= s.graphemes.size() - cell.text_offset);
        cell.kind = static_cast<CellKind>(kind);
        cell.style.foreground = read_color(in);
        cell.style.background = read_color(in);
        cell.style.underline_color = read_color(in);
        in >> underline >> flags;
        check(underline <= static_cast<quint8>(Underline::dashed) && flags <= 255);
        cell.style.underline = static_cast<Underline>(underline);
        cell.style.bold = (flags & 1U) != 0;
        cell.style.italic = (flags & 2U) != 0;
        cell.style.faint = (flags & 4U) != 0;
        cell.style.blink = (flags & 8U) != 0;
        cell.style.inverse = (flags & 16U) != 0;
        cell.style.invisible = (flags & 32U) != 0;
        cell.style.strikethrough = (flags & 64U) != 0;
        cell.style.overline = (flags & 128U) != 0;
        s.cells.push_back(cell);
    }
    if (!in.atEnd()) {
        quint32 tag{}, links{};
        in >> tag >> links;
        check(in.status() == QDataStream::Ok && tag == link_extension && links > 0 &&
              links <= max_hyperlink_spans);
        std::size_t bytes_read = 0;
        for (quint32 i = 0; i < links; ++i) {
            TerminalHyperlink link;
            quint32 length{};
            in >> link.first_cell >> link.cell_count >> length;
            check(in.status() == QDataStream::Ok && length > 0 &&
                  length <= max_hyperlink_uri_bytes && length <= max_hyperlink_bytes - bytes_read &&
                  in.device()->bytesAvailable() >= length);
            link.uri.resize(length);
            check(in.readRawData(link.uri.data(), length) == length);
            bytes_read += length;
            s.hyperlinks.push_back(std::move(link));
        }
        validate_links(s);
    }
    check(in.status() == QDataStream::Ok && in.atEnd());
    return s;
}
} // namespace lapis::session::wire
