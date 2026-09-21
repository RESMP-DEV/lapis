#ifndef LAPIS_CODEX_OBSERVER_HPP
#define LAPIS_CODEX_OBSERVER_HPP

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <lapis/session/attention.hpp>
#include <memory>

namespace lapis::codex {
// One dedicated server/TUI source. The service owns State and validates GUI
// attachment authority before decide(). All calls and signals share its thread.
class Observer final : public QObject {
    Q_OBJECT
  public:
    explicit Observer(session::attention::State& state, QObject* parent = nullptr);
    ~Observer() override;
    static QString qualifiedBinarySha256();
    void start(const QString& socket, const QString& binary_sha256);
    void reconnect(); // Explicit only; no response retransmission.
    void stop();
    [[nodiscard]] QString diagnostic() const;
    [[nodiscard]] QString threadId() const;
    // Display-only bounded details: method, command/cwd or questions/options.
    [[nodiscard]] QJsonObject details(const session::attention::RequestId& id) const;
    // Validate payload first, consume the exact State token, then send once.
    // False may mean rejected before consumption OR consumed with delivery
    // uncertain. A send failure disconnects the source and retains submission;
    // reconcile explicitly before another decision. Never automatically replay.
    // Approval choice: accept/decline/cancel. Input choice: submit; answers maps
    // question IDs to {"answers":[label or free text]}, as the source requires.
    [[nodiscard]] bool decide(quint64 epoch, const session::attention::RequestId& id,
                              quint64 revision, const QString& choice,
                              const QJsonObject& answers = {});
  signals:
    void initialized(); // Source initialized; service may start its TUI now.
    void changed();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::codex
#endif
