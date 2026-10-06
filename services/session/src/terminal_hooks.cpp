#include "terminal_hooks.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>

namespace lapis::session {
namespace {
constexpr QByteArrayView marker{"\x1b]7717;lapis-"};
constexpr qsizetype nonce_size = 32;
bool valid_nonce(QByteArrayView value) {
    return value.size() == nonce_size && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
// The longest end of `data` that could begin the marker.
qsizetype partial_marker(QByteArrayView data) {
    for (auto keep = std::min(data.size(), marker.size() - 1); keep > 0; --keep)
        if (data.last(keep) == marker.first(keep))
            return keep;
    return 0;
}
} // namespace

QByteArray TerminalHookChannel::filter(QByteArrayView output,
                                       std::vector<TerminalHookEvent>& events) {
    QByteArray joined;
    QByteArrayView data = output;
    if (!carry_.isEmpty()) {
        joined = carry_ + output.toByteArray();
        data = joined;
        carry_.clear();
    }
    QByteArray kept;
    kept.reserve(data.size());
    qsizetype from = 0;
    for (;;) {
        const auto start = data.indexOf(marker, from);
        if (start < 0) {
            const auto keep = partial_marker(data.sliced(from));
            kept.append(data.sliced(from, data.size() - from - keep));
            carry_ = data.last(keep).toByteArray();
            return kept;
        }
        kept.append(data.sliced(from, start - from));
        const auto body = start + marker.size();
        const auto bell = data.indexOf('\x07', body);
        const auto escape = data.indexOf('\x1b', body);
        qsizetype end = -1;
        qsizetype terminator = 0;
        if (bell >= 0 && (escape < 0 || bell < escape)) {
            end = bell;
            terminator = 1;
        } else if (escape >= 0 && escape + 1 < data.size()) {
            if (data.at(escape + 1) != '\\') {
                // Not one of ours after all: leave it for the terminal.
                kept.append(data.sliced(start, escape - start));
                from = escape;
                continue;
            }
            end = escape;
            terminator = 2;
        }
        if (end < 0) {
            if (data.size() - start > max_sequence) {
                kept.append(data.sliced(start));
                return kept;
            }
            carry_ = data.sliced(start).toByteArray();
            return kept;
        }
        if (end - start <= max_sequence)
            accept(data.sliced(body, end - body), events);
        from = end + terminator;
    }
}

void TerminalHookChannel::accept(QByteArrayView body, std::vector<TerminalHookEvent>& events) {
    const auto first = body.indexOf(';');
    const auto second = first < 0 ? -1 : body.indexOf(';', first + 1);
    if (second < 0 || body.indexOf(';', second + 1) >= 0)
        return;
    const auto kind = body.first(first);
    const auto subject = body.sliced(first + 1, second - first - 1);
    const auto value = body.sliced(second + 1);
    if (kind == QByteArrayView("init")) {
        if (!nonce_.isEmpty() || !valid_nonce(value) ||
            (subject != QByteArrayView("claude") && subject != QByteArrayView("codex")))
            return;
        nonce_ = value.toByteArray();
        cli_ = QString::fromLatin1(subject);
        return;
    }
    if (kind != QByteArrayView("event") || nonce_.isEmpty() || subject != nonce_)
        return;
    const auto decoded = QByteArray::fromBase64Encoding(
        value.toByteArray(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(*decoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;
    events.push_back({cli_, document.object()});
}

bool NotifyTurns::completed(const QJsonObject& source, attention::Tick now) {
    if (source.value(QStringLiteral("type")) != QLatin1String("agent-turn-complete"))
        return false;
    if (!state_.ready()) {
        if (sequence_ != 0)
            return false; // lost synchronization stays visible
        state_.connect(1, {true, false, false});
        sequence_ = 1;
        if (state_.begin_observation({1, sequence_}, now) != attention::Outcome::applied)
            return false;
    }
    if (state_.activity() == attention::Activity::turn_completed)
        return false;
    return state_.activity({1, ++sequence_}, attention::Activity::turn_completed) ==
           attention::Outcome::applied;
}

bool NotifyTurns::submitted() {
    if (!state_.ready() || state_.activity() == attention::Activity::unknown)
        return false;
    return state_.activity({1, ++sequence_}, attention::Activity::unknown) ==
           attention::Outcome::applied;
}

QString NotifyTurns::diagnostic() {
    return QStringLiteral(
        "Codex reports finished turns; between them status is estimated from output");
}
} // namespace lapis::session
