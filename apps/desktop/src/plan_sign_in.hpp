#ifndef LAPIS_DESKTOP_PLAN_SIGN_IN_HPP
#define LAPIS_DESKTOP_PLAN_SIGN_IN_HPP

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <functional>

namespace lapis::desktop {

// Adds a Claude Code plan from inside lapis, signed in as whichever account
// the person chooses. plan_sign_in.py runs `claude setup-token` with the
// browser left closed; its sign-in link is copied and shown until someone
// signs in with it. The token Claude Code then prints goes owner-only into
// the accounts folder, never through lapis, and the plan is recorded under
// the email the person names (`Record`), usable on this Mac.
class PlanSignIn final : public QObject {
    Q_OBJECT
    // "idle", "starting", "waiting" (the link is out), "signedIn" (waiting
    // for the email), "done" or "failed".
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString link READ link NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    // The plan that was recorded, once done.
    Q_PROPERTY(QString plan READ plan NOTIFY changed)
  public:
    // The path of "python3" or "claude" on this Mac, or empty.
    using Program = std::function<QString(const QString&)>;
    using Copy = std::function<void(const QString&)>;
    // Records the plan for `email` on this Mac and returns its name, or an
    // empty name and a reason.
    using Record = std::function<QString(const QString& email, QString* reason)>;
    struct Places {
        QString helper;   // where plan_sign_in.py is written
        QString accounts; // ~/.lapis/accounts
    };
    PlanSignIn(Program program, Copy copy, Record record, Places places, QObject* parent = nullptr);
    ~PlanSignIn() override;
    PlanSignIn(const PlanSignIn&) = delete;
    PlanSignIn& operator=(const PlanSignIn&) = delete;

    [[nodiscard]] const QString& state() const { return state_; }
    [[nodiscard]] const QString& link() const { return link_; }
    [[nodiscard]] const QString& message() const { return message_; }
    [[nodiscard]] const QString& plan() const { return plan_; }

    // Starts a sign-in, ending any earlier one.
    Q_INVOKABLE void start();
    // The account being signed in; the plan is recorded once both the email
    // and the sign-in are in.
    Q_INVOKABLE void setEmail(const QString& email);
    Q_INVOKABLE void copyLink();
    // Ends a sign-in that has not finished and forgets its token.
    Q_INVOKABLE void cancel();

  signals:
    void changed();

  private:
    void read();
    void ended();
    void finish();
    void fail(const QString& why);
    void set(const QString& state, const QString& message = {});
    [[nodiscard]] QString pendingToken() const;
    Program program_;
    Copy copy_;
    Record record_;
    Places places_;
    QPointer<QProcess> process_;
    QByteArray output_;
    QString state_{QStringLiteral("idle")};
    QString link_;
    QString message_;
    QString email_;
    QString plan_;
};

// A plan name from an email: someone@example.com -> someone-example.
[[nodiscard]] QString plan_name_for(const QString& email);

} // namespace lapis::desktop
#endif
