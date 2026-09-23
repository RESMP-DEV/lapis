#ifndef LAPIS_DESKTOP_WORKSPACE_SUPERVISOR_HPP
#define LAPIS_DESKTOP_WORKSPACE_SUPERVISOR_HPP

#include "workspace.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace lapis::desktop {
// GUI-thread policy only. SessionPreview remains the response/connection authority.
class WorkspaceSupervisor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList attentionQueue READ attentionQueue NOTIFY changed)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY changed)
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
  public:
    explicit WorkspaceSupervisor(Workspace& workspace, std::function<qint64()> clock = {},
                                 QObject* parent = nullptr);
    ~WorkspaceSupervisor() override;
    Q_INVOKABLE bool review(const QString& sessionId, const QString& token);
    Q_INVOKABLE bool snooze(const QString& sessionId, const QString& token,
                            int milliseconds = 30000);
    Q_INVOKABLE void tick();
    void noteInteraction();
    void setWindowActive(bool active);
    [[nodiscard]] QVariantList attentionQueue() const;
    [[nodiscard]] int pendingCount() const { return static_cast<int>(requests_.size()); }
    [[nodiscard]] bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    [[nodiscard]] bool paused() const { return paused_; }
    void setPaused(bool paused);
    [[nodiscard]] bool pinned() const;
    void setPinned(bool pinned);
    [[nodiscard]] QString status() const;
  signals:
    void changed();
    void reviewRequested(lapis::desktop::SessionPreview* session, const QString& token);

  private:
    using Key = std::pair<QString, QString>;
    struct Request {
        QVariantMap row;
        qint64 first_seen{};
        qint64 snoozed_until{};
    };
    struct Source {
        QPointer<SessionPreview> session;
        qint64 last_visit{};
        std::vector<QMetaObject::Connection> connections;
    };
    [[nodiscard]] qint64 now() const;
    void synchronize();
    void observeSource(const QString& id, SessionPreview* session, qint64 timestamp);
    void publish();
    [[nodiscard]] QString candidate(qint64 timestamp) const;
    [[nodiscard]] std::optional<std::pair<bool, qint64>> requestPriority(const QString& id,
                                                                         qint64 timestamp) const;
    Workspace& workspace_;
    std::function<qint64()> clock_;
    QTimer timer_;
    std::map<QString, Source> sources_;
    std::map<Key, Request> requests_;
    QVariantMap published_state_;
    mutable qint64 last_clock_{};
    qint64 last_input_{};
    qint64 last_manual_{};
    qint64 last_switch_{};
    QString pinned_id_;
    bool enabled_{};
    bool paused_{};
    bool window_active_{};
    bool automatic_focus_{};
    bool synchronizing_{};
};
} // namespace lapis::desktop
#endif
