#ifndef LAPIS_CODEX_OBSERVER_HPP
#define LAPIS_CODEX_OBSERVER_HPP

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <lapis/session/attention.hpp>
#include <lapis/session/observation_phase.hpp>
#include <memory>

namespace lapis::codex {
enum class DecisionOutcome : std::uint8_t {
    rejected = 0,
    uncertain = 1,
    delivered = 2,
};

// One dedicated server/TUI source. The service owns State and validates GUI
// attachment authority before decide(). All calls and signals share its thread.
class Observer final : public QObject {
    Q_OBJECT
  public:
    explicit Observer(session::attention::State& state, QObject* parent = nullptr);
    ~Observer() override;
    static QString qualifiedBinarySha256();
    // Every executable digest whose request behavior has been qualified live.
    static QStringList qualifiedBinarySha256s();
    void start(const QString& socket, const QString& binary_sha256);
    void reconnect(); // Explicit only; no response retransmission.
    void stop();
    [[nodiscard]] QString diagnostic() const;
    [[nodiscard]] session::attention::ObservationPhase observationPhase() const;
    [[nodiscard]] QString threadId() const;
    // Advisory only. Inventory is aggregated so source paths and hook payloads
    // remain private; runs expose identity and classification, not hook output.
    [[nodiscard]] QJsonObject hookInventory() const;
    [[nodiscard]] QJsonArray hookRuns() const;
    // Display-only bounded details: method, command/cwd or questions/options.
    [[nodiscard]] QJsonObject details(const session::attention::RequestId& id) const;
    // Validate payload and complete encoded envelope, consume the exact State
    // token, then send once. Rejected means the token was not consumed.
    // Uncertain means it was consumed but socket delivery failed; reconcile
    // explicitly before another decision. Delivered means queued locally, not
    // source resolution; the source still owns the final outcome. Never replay.
    // Approval choice: accept/decline/cancel. Input choice: submit; answers maps
    // question IDs to {"answers":[label or free text]}, as the source requires.
    [[nodiscard]] DecisionOutcome decide(quint64 epoch, const session::attention::RequestId& id,
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
