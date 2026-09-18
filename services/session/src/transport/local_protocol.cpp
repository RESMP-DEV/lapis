#include "local_protocol.hpp"
#include <QDataStream>
#include <QIODevice>
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
} // namespace
QByteArray frame(Kind kind, const QByteArray& payload) {
    check(payload.size() < max_frame_bytes);
    QByteArray result;
    QDataStream out(&result, QIODevice::WriteOnly);
    out << static_cast<quint32>(payload.size() + 1) << static_cast<quint8>(kind);
    result += payload;
    return result;
}
bool take_frame(QByteArray& buffer, Frame& result) {
    if (buffer.size() < 4)
        return false;
    QDataStream in(buffer);
    quint32 size{};
    in >> size;
    check(size >= 1 && size <= max_frame_bytes);
    if (buffer.size() < static_cast<qsizetype>(size) + 4)
        return false;
    quint8 kind{};
    in >> kind;
    check(kind >= static_cast<quint8>(Kind::hello) && kind <= static_cast<quint8>(Kind::attach));
    result = {static_cast<Kind>(kind), buffer.mid(5, static_cast<qsizetype>(size) - 1)};
    buffer.remove(0, static_cast<qsizetype>(size) + 4);
    return true;
}
QByteArray encode_snapshot(const TerminalSnapshot& s) {
    check(s.cells.size() <= max_cells && s.graphemes.size() <= max_codepoints);
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
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
