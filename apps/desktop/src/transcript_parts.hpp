#ifndef LAPIS_DESKTOP_TRANSCRIPT_PARTS_HPP
#define LAPIS_DESKTOP_TRANSCRIPT_PARTS_HPP
// What the CLIs' transcript readers share: the same text limits and the same
// way of making items, so every CLI's steps read alike. Internal to the
// readers (transcript*.cpp).
#include "transcript.hpp"

#include <QJsonValue>

namespace lapis::desktop::transcript::detail {
// `text` cut to `limit` characters, with an ellipsis when cut.
[[nodiscard]] QString clip(QString text, qsizetype limit);
// A path under the home folder written from ~.
[[nodiscard]] QString homeless(QString path);
// `text` without terminal color and cursor sequences.
[[nodiscard]] QString without_escapes(QString text);
// A JSON value written compactly, cut to `limit`.
[[nodiscard]] QString compact_json(const QJsonValue& value, qsizetype limit);
[[nodiscard]] Item make(Kind kind, QString title, QString body = {}, QString detail = {});
// Adds `text`'s lines to the diff preview and counts all of them.
void add_lines(Item& item, QChar sign, const QString& text);
// A text's first line, with an ellipsis when more follow.
[[nodiscard]] QString first_line(const QString& text);
// Sets a step's output: escapes removed, trimmed, its lines counted and the
// preview bounded as for every CLI.
void set_result(Item& item, const QString& output);
// A thought as the view shows it: trimmed, runs of blank lines one paragraph
// break, and agy's stock opening ("I'm thinking through how to approach
// this.") dropped, so a thought of only that is empty.
[[nodiscard]] QString thought_text(QString text);
// Marks a prompt whose body is a lapis-peers message (peer_message) as
// another agent's: its body becomes the message and `from` its sender.
void mark_peer(Item& prompt);
// Thinking and tool steps are drawn tight against one another.
[[nodiscard]] bool is_step(Kind kind);
// ms since the epoch for an ISO 8601 time ("2026-11-10T20:21:33Z", with or
// without milliseconds or an offset), or 0.
[[nodiscard]] qint64 time_ms(const QString& stamp);
} // namespace lapis::desktop::transcript::detail
#endif
