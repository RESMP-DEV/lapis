#include "agent_state.hpp"
#include "next_prompt.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr qsizetype kReasonLimit = 256;
constexpr auto kOwnerOnly = QFile::ReadOwner | QFile::WriteOwner;

void write_private(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(kOwnerOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        qWarning().noquote() << "Agent state not published:" << file.errorString();
        return;
    }
    QFile::setPermissions(path, kOwnerOnly);
}
} // namespace

AgentStatePublisher::AgentStatePublisher(Workspace& workspace, NextPrompt* next, QString path,
                                         int interval_ms, QObject* parent)
    : QObject(parent), workspace_(workspace), next_(next), path_(std::move(path)),
      interval_ms_(std::max(0, interval_ms)) {
    writer_.setMaxThreadCount(1);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &AgentStatePublisher::publish);
    connect(&workspace_, &Workspace::sessionsChanged, this, [this] {
        watchSessions();
        changed();
    });
    connect(&workspace_, &Workspace::categoriesChanged, this, &AgentStatePublisher::changed);
    connect(&workspace_, &Workspace::turnFinished, this, [this](SessionPreview* item) {
        if (item != nullptr)
            turn_at_ms_.insert(item->sessionId(), QDateTime::currentMSecsSinceEpoch());
        changed();
    });
    if (next_) {
        connect(next_, &NextPrompt::changed, this, &AgentStatePublisher::changed);
        connect(next_, &NextPrompt::seenChanged, this, &AgentStatePublisher::changed);
    }
    watchSessions();
    changed();
}

AgentStatePublisher::~AgentStatePublisher() { writer_.waitForDone(); }

void AgentStatePublisher::watchSessions() {
    for (const auto& value : workspace_.sessions()) {
        auto* item = value.value<SessionPreview*>();
        if (item == nullptr)
            continue;
        connect(item, &SessionPreview::statusChanged, this, &AgentStatePublisher::changed,
                Qt::UniqueConnection);
        connect(item, &SessionPreview::attentionChanged, this, &AgentStatePublisher::changed,
                Qt::UniqueConnection);
        connect(item, &SessionPreview::unseenChanged, this, &AgentStatePublisher::changed,
                Qt::UniqueConnection);
    }
}

void AgentStatePublisher::changed() {
    if (timer_.isActive())
        return; // the pending deadline publishes the newest state
    const qint64 since = since_publish_.isValid() ? since_publish_.elapsed() : interval_ms_;
    timer_.start(static_cast<int>(std::max<qint64>(0, interval_ms_ - since)));
}

QJsonObject AgentStatePublisher::state() const {
    QJsonArray agents;
    for (const auto& value : workspace_.sessions()) {
        const auto* item = value.value<SessionPreview*>();
        if (item == nullptr)
            continue;
        const auto& id = item->sessionId();
        QJsonObject agent{{QStringLiteral("id"), id},
                          {QStringLiteral("status"), item->statusKind()},
                          {QStringLiteral("unseen"), item->unseen()},
                          {QStringLiteral("requests"), item->attentionCount()},
                          {QStringLiteral("neededAtMs"), item->neededAtMs()},
                          {QStringLiteral("turnAtMs"), turn_at_ms_.value(id)}};
        if (item->attentionPending())
            agent.insert(QStringLiteral("request"), item->attentionReason().left(kReasonLimit));
        if (next_)
            if (const auto offer = next_->offerState(id); !offer.isEmpty())
                agent.insert(QStringLiteral("offer"), offer);
        agents.append(agent);
    }
    return {{QStringLiteral("version"), version},
            {QStringLiteral("pid"), QCoreApplication::applicationPid()},
            {QStringLiteral("agents"), agents}};
}

QByteArray AgentStatePublisher::document() const {
    return QJsonDocument(state()).toJson(QJsonDocument::Compact);
}

void AgentStatePublisher::publish() {
    since_publish_.start();
    auto object = state();
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes == last_)
        return;
    last_ = std::move(bytes);
    // The time is added after the comparison so an unchanged state is not
    // rewritten just because the clock moved.
    object.insert(QStringLiteral("publishedAtMs"), QDateTime::currentMSecsSinceEpoch());
    ++writes_;
    writer_.start([path = path_, out = QJsonDocument(object).toJson(QJsonDocument::Compact)] {
        write_private(path, out);
    });
}

void AgentStatePublisher::waitForWrites() { writer_.waitForDone(); }
} // namespace lapis::desktop
