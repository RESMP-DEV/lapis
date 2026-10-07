#include "terminal_hooks.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <limits>

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
    qsizetype discard_from = 0;
    if (discarding_) {
        // An already oversized candidate cannot be shown or interpreted. Drop
        // it through its terminator, then filter the remainder normally.
        for (;;) {
            if (discard_from == 0 && !data.isEmpty() && data.front() == '\\') {
                discard_from = 1;
                discarding_ = false;
                break;
            }
            const auto bell = data.indexOf('\x07', discard_from);
            const auto escape = data.indexOf('\x1b', discard_from);
            if (bell >= 0 && (escape < 0 || bell < escape)) {
                discard_from = bell + 1;
                discarding_ = false;
                break;
            }
            if (escape < 0)
                return {};
            if (escape + 1 == data.size())
                return {};
            if (data.at(escape + 1) == '\\') {
                discard_from = escape + 2;
                discarding_ = false;
                break;
            }
            discard_from = escape + 1;
        }
    }
    if (!carry_.isEmpty()) {
        joined = carry_ + output.toByteArray();
        data = joined;
        carry_.clear();
    }
    QByteArray kept;
    kept.reserve(data.size());
    qsizetype from = discard_from;
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
                kept.chop(data.size() - start);
                discarding_ = true;
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
    const auto decoded = QByteArray::fromBase64Encoding(value.toByteArray(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
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
        state_.connect(state_.epoch() + 1, {true, false, false});
        // The adapter owns its position only once the service accepts it. A
        // rejected initial observation leaves this at zero for the next event.
        if (state_.begin_observation({state_.epoch(), 1}, now) != attention::Outcome::applied)
            return false;
        sequence_ = 1;
    }
    if (!state_.ready() || state_.activity() == attention::Activity::turn_completed ||
        sequence_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    const auto result =
        state_.activity({state_.epoch(), sequence_ + 1}, attention::Activity::turn_completed);
    if (result != attention::Outcome::applied)
        return false;
    sequence_ = sequence_ + 1;
    return true;
}

bool NotifyTurns::submitted() {
    if (!state_.ready() || state_.activity() != attention::Activity::turn_completed ||
        sequence_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    const auto result =
        state_.activity({state_.epoch(), sequence_ + 1}, attention::Activity::unknown);
    if (result != attention::Outcome::applied)
        return false;
    sequence_ = sequence_ + 1;
    return true;
}

QString NotifyTurns::diagnostic() {
    return QStringLiteral(
        "Codex reports finished turns; between them status is estimated from output");
}
} // namespace lapis::session
