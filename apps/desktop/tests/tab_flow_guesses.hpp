#ifndef LAPIS_DESKTOP_TESTS_TAB_FLOW_GUESSES_HPP
#define LAPIS_DESKTOP_TESTS_TAB_FLOW_GUESSES_HPP

// The visual-tab-flow check's stand-in predictor (factory_ui_test.cpp). It
// lives in a header so moc writes its own translation unit, which the
// project's static analysis leaves to the compiler as generated code.
#include <QHash>
#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace tab_flow {
// A stand-in for NextPrompt with its QML surface and its bookkeeping: an
// offer goes once it is used (Tab typed it), a "sent" use needs that earlier
// use of the same offer, and an impression counts once per offer. It never
// runs the helper or calls a model; it records what the window reports.
class Guesses final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(double confidentThreshold READ confidentThreshold CONSTANT)
    Q_PROPERTY(int failureStreak READ failureStreak CONSTANT)
    Q_PROPERTY(QString lastFailure READ lastFailure CONSTANT)
  public:
    struct Offer {
        QString text;
        QString key;
        bool seen{};
        QString author;
    };
    void offer(const QString& id, const QString& text, const QString& key,
               const QString& author = {}) {
        offers_.insert(id, {text, key, false, author});
        ++revision_;
        emit changed();
    }
    [[nodiscard]] const QStringList& events() const { return events_; }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] static bool enabled() { return true; }
    [[nodiscard]] static double confidentThreshold() { return 0.45; }
    [[nodiscard]] static int failureStreak() { return 0; }
    [[nodiscard]] static QString lastFailure() { return {}; }
    Q_INVOKABLE [[nodiscard]] static bool guesses(const QString& cli) {
        return cli == QLatin1String("claude") || cli == QLatin1String("codex") ||
               cli == QLatin1String("omp");
    }
    Q_INVOKABLE [[nodiscard]] QString suggestion(const QString& id) const {
        return offers_.value(id).text;
    }
    Q_INVOKABLE [[nodiscard]] QString author(const QString& id) const {
        return offers_.value(id).author;
    }
    Q_INVOKABLE [[nodiscard]] static double confidence(const QString&) { return 0.5; }
    Q_INVOKABLE [[nodiscard]] QString offerKey(const QString& id) const {
        return offers_.value(id).key;
    }
    Q_INVOKABLE [[nodiscard]] QVariantMap readyAgents() const {
        QVariantMap ready;
        for (auto offer = offers_.cbegin(); offer != offers_.cend(); ++offer)
            ready.insert(offer.key(), offer->seen);
        return ready;
    }
    Q_INVOKABLE void seenOffer(const QVariantMap& identity) {
        const auto id = identity.value(QStringLiteral("session")).toString();
        const auto key = identity.value(QStringLiteral("offer")).toString();
        auto offer = offers_.find(id);
        if (offer == offers_.end() || key.isEmpty() || offer->key != key || offer->seen)
            return;
        offer->seen = true;
        events_.append(QStringLiteral("seen ") + key);
    }
    Q_INVOKABLE void used(const QString& id, bool sent, int, const QString& key = {}) {
        if (sent) {
            if (!filled_.contains(id) || (!key.isEmpty() && filled_.value(id) != key))
                return;
            events_.append(QStringLiteral("sent ") + filled_.value(id));
            return;
        }
        const auto offer = offers_.value(id);
        if (offer.text.isEmpty() || (!key.isEmpty() && offer.key != key))
            return;
        events_.append(QStringLiteral("filled ") + offer.key);
        filled_.insert(id, offer.key);
        offers_.remove(id);
        ++revision_;
        emit changed();
    }
  signals:
    void changed();

  private:
    QHash<QString, Offer> offers_;
    QHash<QString, QString> filled_;
    QStringList events_;
    int revision_{};
};
} // namespace tab_flow

#endif
