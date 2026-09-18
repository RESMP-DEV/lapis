#ifndef LAPIS_SESSION_LOCAL_PROTOCOL_HPP
#define LAPIS_SESSION_LOCAL_PROTOCOL_HPP
#include <QByteArray>
#include <QtGlobal>
#include <lapis/session/terminal.hpp>

namespace lapis::session::wire {
constexpr quint32 version = 2;
constexpr qsizetype max_frame_bytes = qsizetype{8} * 1024 * 1024;
constexpr quint32 max_cells = 32768;
constexpr quint32 max_codepoints = 65536;
enum class Kind : quint8 { hello = 1, snapshot, text, paste, key, resize, status, attach };
struct Frame {
    Kind kind{Kind::hello};
    QByteArray payload;
};
[[nodiscard]] QByteArray frame(Kind kind, const QByteArray& payload);
// Incomplete input returns false; malformed or oversized frames throw.
[[nodiscard]] bool take_frame(QByteArray& buffer, Frame& result);
// consumed tracks the parsed prefix and is compacted lazily.
[[nodiscard]] bool take_frame(QByteArray& buffer, qsizetype& consumed, Frame& result);
[[nodiscard]] QByteArray encode_snapshot(const TerminalSnapshot& snapshot);
[[nodiscard]] TerminalSnapshot decode_snapshot(const QByteArray& bytes);
} // namespace lapis::session::wire
#endif
