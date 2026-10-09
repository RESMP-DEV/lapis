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

struct SequenceTerminator {
    qsizetype end{-1};
    qsizetype size{};
    qsizetype false_escape{-1};
};

SequenceTerminator find_terminator(QByteArrayView data, qsizetype body) {
    const auto bell = data.indexOf('\x07', body);
    const auto escape = data.indexOf('\x1b', body);
    if (bell >= 0 && (escape < 0 || bell < escape))
        return {bell, 1, -1};
    if (escape < 0 || escape + 1 == data.size())
        return {};
    if (data.at(escape + 1) != '\\')
        return {-1, 0, escape};
    return {escape, 2, -1};
}
} // namespace

TerminalHookChannel::DiscardResult TerminalHookChannel::advance_discard(QByteArrayView data,
                                                                        qsizetype& discard_from) {
    if (discarded_ + data.size() >= max_discard) {
        discarding_ = false;
        discarded_ = 0;
        return DiscardResult::recover;
    }
    for (;;) {
        const auto bell = data.indexOf('\x07', discard_from);
        const auto escape = data.indexOf('\x1b', discard_from);
        if (bell >= 0 && (escape < 0 || bell < escape)) {
            discard_from = bell + 1;
            discarding_ = false;
            discarded_ = 0;
            return DiscardResult::ready;
        }
        if (escape < 0 || escape + 1 == data.size()) {
            discarded_ += data.size();
            return DiscardResult::partial;
        }
        if (data.at(escape + 1) == '\\') {
            discard_from = escape + 2;
            discarding_ = false;
            discarded_ = 0;
            return DiscardResult::ready;
        }
        discard_from = escape + 1;
    }
}

bool TerminalHookChannel::begin_oversized_discard(qsizetype candidate_size) {
    if (discarded_ + candidate_size < max_discard) {
        discarding_ = true;
        discarded_ += candidate_size;
        return false;
    }
    discarding_ = false;
    discarded_ = 0;
    return true;
}

QByteArray TerminalHookChannel::filter(QByteArrayView output,
                                       std::vector<TerminalHookEvent>& events) {
    QByteArray joined;
    QByteArrayView data = output;
    qsizetype discard_from = 0;
    if (discarding_) {
        // An already oversized candidate cannot be shown or interpreted. Drop
        // it through its terminator, then filter the remainder normally. A
        // byte budget bounds quarantine when its terminator never arrives; a
        // bare backslash is not a terminator.
        const auto discarded = advance_discard(data, discard_from);
        if (discarded == DiscardResult::recover)
            return QByteArrayLiteral("[lapis: dropped an oversized hook]\r\n");
        if (discarded == DiscardResult::partial)
            return {};
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
        const auto terminator = find_terminator(data, body);
        if (terminator.false_escape >= 0) {
            // Not one of ours after all: leave it for the terminal.
            kept.append(data.sliced(start, terminator.false_escape - start));
            from = terminator.false_escape;
            continue;
        }
        if (terminator.end < 0) {
            const auto body_size = data.size() - body;
            if (body_size > max_sequence) {
                const auto candidate = max_sequence;
                if (begin_oversized_discard(candidate)) {
                    kept.append(QByteArrayLiteral("[lapis: dropped an oversized hook]\r\n"));
                    return kept;
                }
                carry_.clear();
                return kept;
            }
            carry_ = data.sliced(start).toByteArray();
            return kept;
        }
        if (terminator.end - start <= max_sequence)
            accept(data.sliced(body, terminator.end - body), events);
        from = terminator.end + terminator.size;
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
    if (skip_completion_) {
        // Return already started the next prompt. This finish is the one that
        // was in flight; applying it would leave that new turn marked finished.
        skip_completion_ = false;
        prompt_active_ = false;
        return false;
    }
    if (!state_.ready()) {
        if (sequence_ != 0)
            return false; // lost synchronization stays visible
        if (state_.connected()) {
            // Retry the same epoch after a rejected initial observation; the
            // source epoch has not changed and must strictly increase.
            if (state_.begin_observation({state_.epoch(), 1}, now) != attention::Outcome::applied)
                return false;
        } else {
            state_.connect(state_.epoch() + 1, {true, false, false});
            if (state_.begin_observation({state_.epoch(), 1}, now) != attention::Outcome::applied)
                return false;
        }
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
    prompt_active_ = false;
    return true;
}

bool NotifyTurns::submitted() {
    if (!state_.ready() || state_.activity() != attention::Activity::turn_completed ||
        sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        if (state_.ready() && state_.activity() != attention::Activity::turn_completed) {
            if (prompt_active_)
                skip_completion_ = true;
        }
        return false;
    }
    const auto result =
        state_.activity({state_.epoch(), sequence_ + 1}, attention::Activity::unknown);
    if (result != attention::Outcome::applied)
        return false;
    sequence_ = sequence_ + 1;
    prompt_active_ = true;
    return true;
}

QString NotifyTurns::diagnostic() const {
    return QStringLiteral("Codex notify turns: synchronized=%1 epoch=%2 sequence=%3; "
                          "between turns status is estimated from output")
        .arg(state_.ready() ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(state_.epoch())
        .arg(sequence_);
}
} // namespace lapis::session
