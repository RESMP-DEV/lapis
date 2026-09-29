#ifndef LAPIS_DESKTOP_ACCOUNTS_HPP
#define LAPIS_DESKTOP_ACCOUNTS_HPP

#include <QHash>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <vector>

namespace lapis::desktop {

// A plan lapis can give a Claude Code or Codex session. On its home machine
// it is that machine's own sign-in; on the other machines it names, lapis
// keeps a credential for it: a Claude Code setup token in
// ~/.lapis/accounts/claude/NAME.token, or a Codex home
// ~/.lapis/accounts/codex/NAME holding its own auth.json.
struct Account {
    QString cli;          // "claude" or "codex"
    QString name;         // [A-Za-z0-9._-], at most 64
    QString email;        // lowercased; how usage reports name it
    QString home;         // its sign-in's machine: "" for this Mac, else an ssh host
    bool hasHome{};       // false: no machine signs in as it
    QStringList machines; // where lapis keeps its credential ("" for this Mac)
};

// {"accounts": {"switchAt": 95, "claude": [{"name", "email", "home",
// "machines"}], "codex": [...]}}, where "home" and "machines" say "local" for
// this Mac.
struct AccountsConfig {
    std::vector<Account> accounts;
    double switchAt{95};
};
[[nodiscard]] AccountsConfig parse_accounts(const QJsonValue& value);
// Links every entry of the shared Codex home into an account's home, but its
// auth.json, so sessions and settings stay shared across plans.
[[nodiscard]] QString link_codex_home(const QString& shared, const QString& home);

// How full an account is, in percent used: its tightest window, and its
// short (five hour) window; negative when no report has said.
struct AccountLoad {
    double used{-1};
    double shortWindow{-1};
};
// A load's key: the CLI and the account's email, or account_home_name for
// a machine's own sign-in, whoever it is.
[[nodiscard]] QString account_load_key(const QString& cli, const QString& email);
[[nodiscard]] QString account_home_name(const QString& machine);

// Chooses a plan for each session, as OMP ranks its accounts: a session keeps
// its plan while that is below the switch point; a new one takes its
// machine's own sign-in while that is; otherwise the plan with the most room
// that the machine can use, ahead of any whose five hour window is nearly
// spent, measured before unmeasured.
class AccountPool {
  public:
    void setConfig(AccountsConfig config) { config_ = std::move(config); }
    void setLoads(QHash<QString, AccountLoad> loads) { loads_ = std::move(loads); }
    [[nodiscard]] const AccountsConfig& config() const { return config_; }
    [[nodiscard]] bool configured(const QString& cli) const;
    [[nodiscard]] const Account* find(const QString& cli, const QString& name) const;
    // `machine`'s own sign-in, when an account names it as home.
    [[nodiscard]] const Account* own(const QString& cli, const QString& machine) const;
    [[nodiscard]] bool usable(const Account& account, const QString& machine) const;
    // The plan for a session of `cli` on `machine` now using `current`
    // (empty: the machine's own sign-in). Empty when nothing is configured.
    [[nodiscard]] QString choose(const QString& cli, const QString& machine,
                                 const QString& current) const;
    // The plan with the most room other than `current`, for a switch the
    // user asks for; `current` itself when no other has room.
    [[nodiscard]] QString alternative(const QString& cli, const QString& machine,
                                      const QString& current) const;
    // Whether the session's plan is at or past the switch point.
    [[nodiscard]] bool full(const QString& cli, const QString& machine,
                            const QString& current) const;
    [[nodiscard]] AccountLoad load(const Account& account) const;

  private:
    [[nodiscard]] bool below(const Account& account) const;
    // The machine's usable plans below the switch point, most room first.
    [[nodiscard]] std::vector<const Account*> ranked(const QString& cli,
                                                     const QString& machine) const;
    AccountsConfig config_;
    QHash<QString, AccountLoad> loads_;
};

} // namespace lapis::desktop
#endif
