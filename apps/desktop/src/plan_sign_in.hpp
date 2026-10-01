#ifndef LAPIS_DESKTOP_PLAN_SIGN_IN_HPP
#define LAPIS_DESKTOP_PLAN_SIGN_IN_HPP

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <functional>

namespace lapis::desktop {
class UpdaterProcess;

// Adds a Claude Code plan from inside lapis, signed in as whichever account
// the person chooses. plan_sign_in.py runs `claude setup-token` and passes its
// sign-in link on instead of opening it; lapis opens it in the default
// browser, copies it and keeps it shown for another try until someone signs
// in. The token Claude Code then prints goes owner-only into the accounts
// folder and the plan is recorded under the email the person names. The
// token is then copied, owner-only, only to this plan's configured destinations.
// New plans are local until additional destinations are explicitly configured.
class PlanSignIn final : public QObject {
    Q_OBJECT
    // "idle", "starting", "waiting" (the link is out), "signedIn" (waiting
    // for the email), "done" or "failed".
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString link READ link NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    // The plan that was recorded, once done.
    Q_PROPERTY(QString plan READ plan NOTIFY changed)
    // Where the plan can be used so far ("this Mac" first), and whether
    // copies to other machines are still under way.
    Q_PROPERTY(QStringList machines READ machines NOTIFY changed)
    Q_PROPERTY(bool spreading READ spreading NOTIFY changed)
  public:
    // The path of "python3" or "claude" on this Mac, or empty.
    using Program = std::function<QString(const QString&)>;
    // Copies the link, or opens it in the default browser.
    using Hand = std::function<void(const QString&)>;
    // Records the plan for `email` on `machine` ("" for this Mac) and returns
    // its name, or an empty name and a reason.
    using Prepare = std::function<bool(const QString& name, QString* reason)>;
    // The config owner chooses a name, runs prepare, then commits availability.
    // expectedName pins a destination chosen before an asynchronous copy.
    using Record = std::function<QString(const QString& email, const QString& machine,
                                         const QString& expectedName, const Prepare& prepare,
                                         QString* reason)>;
    // Explicit destinations for this email's plan; never a global SSH inventory.
    using Machines = std::function<QStringList(const QString& email)>;
    struct Hooks {
        Program program;
        Hand copy;
        Hand open;
        Record record;
        Machines machines;
    };
    struct Places {
        QString helper;   // where plan_sign_in.py is written
        QString accounts; // ~/.lapis/accounts
    };
    PlanSignIn(Hooks hooks, Places places, QObject* parent = nullptr);
    ~PlanSignIn() override;
    PlanSignIn(const PlanSignIn&) = delete;
    PlanSignIn& operator=(const PlanSignIn&) = delete;

    [[nodiscard]] const QString& state() const { return state_; }
    [[nodiscard]] const QString& link() const { return link_; }
    [[nodiscard]] const QString& message() const { return message_; }
    [[nodiscard]] const QString& plan() const { return plan_; }
    [[nodiscard]] QStringList machines() const;
    [[nodiscard]] bool spreading() const { return !copying_.isEmpty(); }

    // Starts a sign-in, ending any earlier one.
    Q_INVOKABLE void start();
    // The account being signed in; the plan is recorded once both the email
    // and the sign-in are in.
    Q_INVOKABLE void setEmail(const QString& email);
    Q_INVOKABLE void copyLink();
    Q_INVOKABLE void openLink();
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
    [[nodiscard]] const QString& pendingToken() const;
    void stopCopies();
    void stopSignIn();
    void spread(const QByteArray& token);
    struct CopyLaunch {
        QString machine;
        QString program;
        QString command;
        QByteArray token;
    };
    void copyTo(const CopyLaunch& launch);
    void report();
    Hooks hooks_;
    Places places_;
    struct Copy {
        QPointer<UpdaterProcess> process;
        QString email;
        QString plan;
        QString machine;
        QByteArray output_head;
        QByteArray output_tail;
        std::uint64_t attempt{};
    };
    QHash<QString, Copy> copying_; // by machine, for one finished sign-in
    QHash<QString, QString> copyReasons_;
    QStringList reached_;
    QStringList copy_failed_;
    QStringList copied_unregistered_;
    bool copy_limit_hit_{};
    QPointer<UpdaterProcess> process_;
    QByteArray output_;
    QByteArray helper_error_;
    qsizetype helper_bytes_{};
    QString pending_token_;
    QString state_{QStringLiteral("idle")};
    QString link_;
    QString message_;
    QString email_;
    bool emailSubmitted_{};
    QString plan_;
    std::uint64_t attempt_{};
};

// A plan name from an email: someone@example.com -> someone-example.
[[nodiscard]] QString plan_name_for(const QString& email);

} // namespace lapis::desktop
#endif
