#ifndef LAPIS_SESSION_LOCAL_PROTOCOL_HPP
#define LAPIS_SESSION_LOCAL_PROTOCOL_HPP
#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <lapis/session/terminal.hpp>

namespace lapis::session::wire {
constexpr quint32 version = 6;
constexpr qsizetype max_frame_bytes = qsizetype{8} * 1024 * 1024;
constexpr quint32 max_cells = 32768;
constexpr quint32 max_codepoints = 65536;
enum class Kind : quint8 {
    hello = 1,
    snapshot,
    text,
    paste,
    key,
    resize,
    status,
    attach,
    ready,
    history_request,
    history_page,
    attention_snapshot,
    attention_decision,
    attention_retry,
    // Client request to end the agent's process; the service answers with the
    // ordinary `ended` status once the process has exited.
    terminate
};
// Identities introduced in v3 and retained in v4/v6: two nonzero 16-byte UUIDs and a BE u64
// generation.
struct SessionIdentity {
    QByteArray session_id;
    QByteArray epoch;
    bool operator==(const SessionIdentity&) const = default;
};
struct Attachment {
    SessionIdentity identity;
    quint64 generation{};
    bool operator==(const Attachment&) const = default;
};
// Join (added within v6) attaches another view beside the current client
// instead of replacing it; services built before it reject the request.
enum class AttachMode : quint8 { discover = 0, reconnect = 1, create = 2, join = 3 };
struct AttachRequest {
    AttachMode mode{AttachMode::discover};
    QByteArray fingerprint;
    SessionIdentity expected;
};
struct Hello {
    Attachment attachment;
    quint64 pid{};
};
struct SnapshotTiming {
    quint64 pty_read_ns{};
    quint64 parse_end_ns{};
    quint64 publish_ns{};
};
constexpr qsizetype snapshot_header_bytes = 72;
struct SnapshotMessage {
    Attachment attachment;
    quint64 sequence{};
    TerminalSnapshot snapshot;
    SnapshotTiming timing{};
};
enum class HistoryDirection : quint8 { older = 0, newer = 1 };
struct HistoryRequest {
    quint64 request_id{};
    quint64 reference{}; // Zero means newest for older; newer requires a page ID.
    HistoryDirection direction{HistoryDirection::older};
};
struct HistoryReply {
    Attachment attachment;
    quint64 request_id{};
    quint64 page_id{}; // Zero only when no page is returned.
    QString message;
    std::optional<TerminalSnapshot> snapshot;
};
// Requests use the normal attachment-bound control envelope. Replies carry
// their own attachment and request ID, independent of live snapshot sequence.
[[nodiscard]] QByteArray encode_history_request(const HistoryRequest& request);
[[nodiscard]] HistoryRequest decode_history_request(const QByteArray& payload);
[[nodiscard]] QByteArray encode_history_reply(const HistoryReply& reply);
[[nodiscard]] HistoryReply decode_history_reply(const QByteArray& payload);
struct ControlMessage {
    Attachment attachment;
    QByteArray payload;
};
struct Ready {
    Attachment attachment;
    quint64 sequence{};
};
enum class StatusCode : quint8 { rejected = 1, ended = 2, replaced = 3, overloaded = 4 };
struct Status {
    StatusCode code{StatusCode::rejected};
    QString message;
};
[[nodiscard]] bool valid_identity(const SessionIdentity& identity);
[[nodiscard]] QByteArray new_id();
// Attach: BE u32 version, fingerprint[32], mode u8, expected session[16], epoch[16].
// Discover and join require both zero; reconnect both nonzero; create session
// nonzero/epoch zero.
[[nodiscard]] QByteArray encode_attach(const AttachRequest& request);
[[nodiscard]] AttachRequest decode_attach(const QByteArray& payload);
// Hello: BE u32 version, attachment[40], BE u64 child PID.
[[nodiscard]] QByteArray encode_hello(const Hello& hello);
[[nodiscard]] Hello decode_hello(const QByteArray& payload);
// Snapshot: attachment[40], BE u64 sequence, three monotonic BE u64 timestamps,
// existing snapshot encoding. Zero timing means no observed PTY output yet.
[[nodiscard]] QByteArray encode_snapshot_message(const SnapshotMessage& message);
[[nodiscard]] SnapshotMessage decode_snapshot_message(const QByteArray& payload);
// Text/paste/key/resize: attachment[40], existing payload (at most 64 KiB).
[[nodiscard]] QByteArray encode_control(const ControlMessage& message);
[[nodiscard]] ControlMessage decode_control(const QByteArray& payload);
// Ready acknowledgement: attachment[40], BE u64 first applied snapshot sequence.
[[nodiscard]] QByteArray encode_ready(const Ready& ready);
[[nodiscard]] Ready decode_ready(const QByteArray& payload);
// Status: code u8, bounded UTF-8 message (at most 4096 bytes).
[[nodiscard]] QByteArray encode_status(const Status& status);
[[nodiscard]] Status decode_status(const QByteArray& payload);
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
