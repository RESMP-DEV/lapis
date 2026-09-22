#ifndef LAPIS_CLAUDE_OBSERVER_HPP
#define LAPIS_CLAUDE_OBSERVER_HPP
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <lapis/session/attention.hpp>
#include <memory>

namespace lapis::claude {
// Service-thread owner of a private, observation-only hook channel.
class Observer final : public QObject {
    Q_OBJECT
  public:
    explicit Observer(session::attention::State& state, QObject* parent = nullptr);
    ~Observer() override;
    [[nodiscard]] const QString& diagnostic() const;
    [[nodiscard]] QJsonObject details(const session::attention::RequestId& id) const;
    [[nodiscard]] QStringList launchArguments(const QStringList& original,
                                              const QString& serviceExecutable);
    void stop();
  signals:
    // State changes immediately; notifications are coalesced on the event loop.
    // Receivers may stop or destroy the observer without reentering transport work.
    void changed();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::claude
#endif
