#include "live_connection.hpp"
#include "workspace.hpp"

#include <QDebug>
#include <QJsonObject>
#include <algorithm>
#include <exception>

namespace lapis::desktop {
namespace {
QString token(const session::wire::AttentionSnapshot& snapshot,
              const session::attention::Pending& pending) {
    const auto& attachment = snapshot.attachment;
    const auto* number = std::get_if<std::int64_t>(&pending.request.id);
    const auto request_id =
        number
            ? QStringLiteral("n") + QString::number(*number)
            : QStringLiteral("s") +
                  QString::fromLatin1(
                      QByteArray::fromStdString(std::get<std::string>(pending.request.id)).toHex());
    return QString::fromLatin1(attachment.identity.session_id.toHex()) + ':' +
           QString::fromLatin1(attachment.identity.epoch.toHex()) + ':' +
           QString::number(attachment.generation) + ':' + QString::number(pending.source_epoch) +
           ':' + QString::number(pending.revision) + ':' + request_id;
}
} // namespace
// A connected agent without an observer reads from the output estimate.
QString SessionPreview::unobservedStatusKind() const {
    if (status_source_ == StatusSource::output && (output_active_ || output_quiet_))
        return output_active_ ? QStringLiteral("working") : QStringLiteral("idle");
    return QStringLiteral("unknown");
}
QString SessionPreview::statusKind() const {
    if (live()) {
        if (connection_state_ == QStringLiteral("ended"))
            return QStringLiteral("ended");
        if (connection_state_ == QStringLiteral("connecting") ||
            connection_state_ == QStringLiteral("synchronizing"))
            return QStringLiteral("connecting");
        if (!input_ready_)
            return QStringLiteral("disconnected");
        if (!attention_)
            return unobservedStatusKind();
        if (!attention_->ready || !attention_->connected)
            return QStringLiteral("unknown");
    }
    if (attention_ && (!attention_->ready || !attention_->connected))
        return QStringLiteral("unknown");
    if (attentionPending())
        return QStringLiteral("waiting");
    if (!attention_)
        return QStringLiteral("unknown");
    switch (attention_->activity) {
    case session::attention::Activity::working:
        return QStringLiteral("working");
    case session::attention::Activity::idle:
        return QStringLiteral("idle");
    case session::attention::Activity::turn_completed:
        return QStringLiteral("finished");
    case session::attention::Activity::unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}
QString SessionPreview::statusLabel() const {
    const auto kind = statusKind();
    if (closing_ && kind != QStringLiteral("ended"))
        return QStringLiteral("Ending agent");
    if (!attention_ && status_source_ == StatusSource::output &&
        (kind == QStringLiteral("working") || kind == QStringLiteral("idle")))
        return kind == QStringLiteral("working") ? QStringLiteral("Output active")
                                                 : QStringLiteral("Quiet");
    if (kind == QStringLiteral("working"))
        return QStringLiteral("Working");
    if (kind == QStringLiteral("waiting"))
        return QStringLiteral("Needs your response");
    if (kind == QStringLiteral("finished"))
        return QStringLiteral("Turn finished");
    if (kind == QStringLiteral("idle"))
        return QStringLiteral("Ready");
    if (kind == QStringLiteral("connecting"))
        return QStringLiteral("Opening agent");
    if (kind == QStringLiteral("disconnected"))
        return QStringLiteral("Unavailable");
    if (kind == QStringLiteral("ended"))
        return QStringLiteral("Ended");
    if (harnessId() != QStringLiteral("codex") && input_ready_)
        return QStringLiteral("Connected");
    // A new Codex session has no persistent thread until its first prompt; the
    // observer is connected and has nothing to report yet.
    if (attention_ && attention_->connected && !attention_->ready &&
        attention_->diagnostic == QLatin1String("Waiting for a persistent Codex thread"))
        return QStringLiteral("Awaiting first prompt");
    if (attention_ && attention_->connected && !attention_->ready)
        return QStringLiteral("Status pending");
    return QStringLiteral("Status unavailable");
}
bool SessionPreview::hasAttentionSource() const { return attention_ && attention_->available; }
bool SessionPreview::attentionReady() const {
    return input_ready_ && attention_ && attention_->ready && attention_->connected;
}
QString SessionPreview::attentionDiagnostic() const {
    return attention_ ? attention_->diagnostic : QString{};
}
int SessionPreview::attentionCount() const {
    return attention_ ? static_cast<int>(attention_->requests.size())
                      : static_cast<int>(requests_.size());
}
QString SessionPreview::attentionReason() const {
    if (!attention_)
        return requests_.isEmpty() ? QString{} : requests_.first();
    if (attention_->requests.empty())
        return {};
    return QString::fromStdString(attention_->requests.front().pending.request.reason);
}
QVariantList SessionPreview::attentionRequests() const {
    QVariantList values;
    if (!attention_)
        return values;
    for (const auto& item : attention_->requests) {
        const auto& pending = item.pending;
        const auto key = token(*attention_, pending);
        const bool responding = pending.submitted || submitted_attention_.contains(key);
        const bool eligible = attentionReady() && !responding &&
                              pending.status == session::attention::RequestStatus::pending;
        QStringList choices;
        for (const auto& choice : pending.request.choices)
            choices.append(QString::fromStdString(choice));
        values.append(QVariantMap{{"token", key},
                                  {"reason", QString::fromStdString(pending.request.reason)},
                                  {"summary", QString::fromStdString(pending.request.summary)},
                                  {"choices", choices},
                                  {"details", item.details.toVariantMap()},
                                  {"responding", responding},
                                  {"attentionEligible", eligible},
                                  {"enabled", eligible && !choices.isEmpty()}});
    }
    return values;
}
void SessionPreview::applyAttention(session::wire::AttentionSnapshot snapshot) {
    bool arrived = false;
    QSet<QString> current;
    for (const auto& item : snapshot.requests) {
        current.insert(token(snapshot, item.pending));
        const auto& id = item.pending.request.id;
        if (!attention_ || std::none_of(attention_->requests.begin(), attention_->requests.end(),
                                        [&](const auto& old) {
                                            return old.pending.request.id == id &&
                                                   old.pending.source_epoch ==
                                                       item.pending.source_epoch &&
                                                   old.pending.revision == item.pending.revision;
                                        }))
            arrived = true;
    }
    submitted_attention_.intersect(current);
    attention_ = std::move(snapshot);
    if (arrived)
        ++attention_serial_;
    emit attentionChanged();
    if (arrived)
        emit attentionArrived();
}
void SessionPreview::invalidateAttention() {
    if (!attention_)
        return;
    attention_->ready = false;
    attention_->connected = false;
    attention_->diagnostic = QStringLiteral("Connection lost; reconnect before responding");
    for (auto& item : attention_->requests)
        item.pending.status = session::attention::RequestStatus::stale;
    emit attentionChanged();
}
void SessionPreview::retryAttention(const session::wire::AttentionDecision& decision) {
    if (!attention_ || attention_->source_epoch != decision.source_epoch)
        return;
    for (const auto& item : attention_->requests) {
        const auto& pending = item.pending;
        if (pending.request.id == decision.request_id && pending.revision == decision.revision &&
            !pending.submitted && pending.status == session::attention::RequestStatus::pending) {
            submitted_attention_.remove(token(*attention_, pending));
            emit attentionChanged();
            return;
        }
    }
}
bool SessionPreview::respondAttention(const QString& key, const QVariantMap& response) {
    const auto choice = response.value(QStringLiteral("choice")).toString();
    const auto answers = response.value(QStringLiteral("answers")).toMap();
    if (!live_ || !attention_ || !attentionReady() || submitted_attention_.contains(key))
        return false;
    const auto item =
        std::find_if(attention_->requests.begin(), attention_->requests.end(),
                     [&](const auto& value) { return token(*attention_, value.pending) == key; });
    if (item == attention_->requests.end() || item->pending.submitted ||
        item->pending.status != session::attention::RequestStatus::pending ||
        std::find(item->pending.request.choices.begin(), item->pending.request.choices.end(),
                  choice.toStdString()) == item->pending.request.choices.end())
        return false;
    const session::wire::AttentionDecision decision{
        attention_->source_epoch, item->pending.request.id, item->pending.revision, choice,
        QJsonObject::fromVariantMap(answers)};
    try {
        if (!live_->send(session::wire::Kind::attention_decision,
                         session::wire::encode_attention_decision(decision)))
            return false;
    } catch (const std::exception& error) {
        qWarning() << "Attention response rejected before queueing:" << error.what();
        return false; // Encoding rejects invalid/bounded input before any bytes are queued.
    }
    submitted_attention_.insert(key);
    emit attentionChanged();
    return true;
}
} // namespace lapis::desktop
