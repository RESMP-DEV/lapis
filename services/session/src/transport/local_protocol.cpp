#include "local_protocol.hpp"
#include <QDataStream>
#include <QIODevice>
#include <QUuid>
#include <limits>
#include <stdexcept>

namespace lapis::session::wire {
namespace {
void check(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid local session message");
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
void append_quint32(QByteArray& bytes, quint32 value) {
    bytes.append(static_cast<char>(value >> 24U));
    bytes.append(static_cast<char>(value >> 16U));
    bytes.append(static_cast<char>(value >> 8U));
    bytes.append(static_cast<char>(value));
}
void append_quint64(QByteArray& bytes, quint64 value) {
    append_quint32(bytes, static_cast<quint32>(value >> 32U));
    append_quint32(bytes, static_cast<quint32>(value));
}
QByteArray raw_bytes(const unsigned char*& cursor, qsizetype size) {
    QByteArray result(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return result;
}
quint32 read_quint32(const unsigned char*& cursor) {
    const quint32 value = (quint32{cursor[0]} << 24U) | (quint32{cursor[1]} << 16U) |
                          (quint32{cursor[2]} << 8U) | quint32{cursor[3]};
    cursor += 4;
    return value;
}
quint64 read_quint64(const unsigned char*& cursor) {
    const quint64 high = read_quint32(cursor);
    return (high << 32U) | read_quint32(cursor);
}
QByteArray encode_attachment(const Attachment& attachment) {
    check(valid_identity(attachment.identity) && attachment.generation != 0);
    QByteArray result;
    result += attachment.identity.session_id;
    result += attachment.identity.epoch;
    append_quint64(result, attachment.generation);
    return result;
}
Attachment decode_attachment(const unsigned char*& cursor) {
    Attachment result;
    result.identity.session_id = raw_bytes(cursor, 16);
    result.identity.epoch = raw_bytes(cursor, 16);
    result.generation = read_quint64(cursor);
    check(valid_identity(result.identity) && result.generation != 0);
    return result;
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
    check(mode <= static_cast<quint8>(AttachMode::create));
    if (request.mode == AttachMode::discover)
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
    result.append(static_cast<char>(mode));
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
    const auto mode = *cursor++;
    check(mode <= static_cast<quint8>(AttachMode::create));
    result.mode = static_cast<AttachMode>(mode);
    result.expected.session_id = raw_bytes(cursor, 16);
    result.expected.epoch = raw_bytes(cursor, 16);
    check(cursor == reinterpret_cast<const unsigned char*>(payload.constData()) + payload.size());
    if (result.mode == AttachMode::discover)
        check(result.expected.session_id == QByteArray(16, '\0') &&
              result.expected.epoch == QByteArray(16, '\0'));
    else if (result.mode == AttachMode::reconnect)
        check(valid_identity(result.expected));
    else
        check(result.expected.session_id != QByteArray(16, '\0') &&
              result.expected.epoch == QByteArray(16, '\0'));
    if (result.mode == AttachMode::discover)
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
    return result;
}
Hello decode_hello(const QByteArray& payload) {
    check(payload.size() == 52);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    check(read_quint32(cursor) == version);
    Hello result;
    result.attachment = decode_attachment(cursor);
    result.pid = read_quint64(cursor);
    check(result.pid != 0 &&
          cursor == reinterpret_cast<const unsigned char*>(payload.constData()) + payload.size());
    return result;
}
QByteArray encode_snapshot_message(const SnapshotMessage& message) {
    check(message.sequence != 0);
    QByteArray result = encode_attachment(message.attachment);
    append_quint64(result, message.sequence);
    result += encode_snapshot(message.snapshot);
    return result;
}
SnapshotMessage decode_snapshot_message(const QByteArray& payload) {
    check(payload.size() > 48);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(payload.constData());
    SnapshotMessage result;
    result.attachment = decode_attachment(cursor);
    result.sequence = read_quint64(cursor);
    check(result.sequence != 0);
    result.snapshot = decode_snapshot(payload.sliced(48));
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
    check(kind >= Kind::hello && kind <= Kind::ready);
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
    check(kind >= static_cast<quint8>(Kind::hello) && kind <= static_cast<quint8>(Kind::ready));
    result = {static_cast<Kind>(kind), buffer.mid(consumed + 5, static_cast<qsizetype>(size) - 1)};
    consumed += static_cast<qsizetype>(size) + 4;
    if (consumed > buffer.size() / 2) {
        buffer.remove(0, consumed);
        consumed = 0;
    }
    return true;
}
QByteArray encode_snapshot(const TerminalSnapshot& s) {
    check(s.cells.size() <= max_cells && s.graphemes.size() <= max_codepoints);
    check(static_cast<std::size_t>(s.size.columns) * s.size.rows == s.cells.size());
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << quint64(s.revision) << quint16(s.size.columns) << quint16(s.size.rows)
        << quint16(s.cursor.column) << quint16(s.cursor.row) << s.cursor.in_viewport
        << s.cursor.visible << s.cursor.blinking << s.cursor.wide_tail << quint8(s.cursor.shape)
        << s.alternate_screen << s.bracketed_paste << s.application_cursor_keys
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
    in >> revision >> columns >> height >> x >> y >> s.cursor.in_viewport >> s.cursor.visible >>
        s.cursor.blinking >> s.cursor.wide_tail >> shape >> s.alternate_screen >>
        s.bracketed_paste >> s.application_cursor_keys >> s.foreground_rgb >> s.background_rgb >>
        cursor_color >> cursor_rgb >> total >> offset >> rows;
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
                 static_cast<std::size_t>(rows)};
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
    check(in.status() == QDataStream::Ok && in.atEnd());
    return s;
}
} // namespace lapis::session::wire
