#include "accounts.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <optional>

namespace lapis::desktop {
namespace {
// A five hour window this full means a block within the session.
constexpr double kHotShortWindow = 85;
constexpr std::size_t kMostAccounts = 64;

// "local" (or nothing) is this Mac; anything else must be an ssh host name.
std::optional<QString> machine_name(const QJsonValue& value) {
    static const QRegularExpression host(QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"));
    auto text = value.toString().trimmed();
    if (text.isEmpty() || text == QLatin1String("local"))
        return QString();
    if (host.match(text).hasMatch())
        return text;
    return std::nullopt;
}
} // namespace

QString account_load_key(const QString& cli, const QString& email) {
    return cli + QLatin1Char(':') + email.trimmed().toLower();
}

QString account_home_name(const QString& machine) {
    return QStringLiteral("@") + (machine.isEmpty() ? QStringLiteral("local") : machine);
}

AccountsConfig parse_accounts(const QJsonValue& value) {
    static const QRegularExpression name(QStringLiteral(R"(^[A-Za-z0-9._-]{1,64}$)"));
    AccountsConfig config;
    const auto section = value.toObject();
    const auto at = section.value(QStringLiteral("switchAt")).toDouble(95);
    if (at >= 50 && at <= 100)
        config.switchAt = at;
    for (const auto* cli : {"claude", "codex"}) {
        for (const auto& item : section.value(QLatin1String(cli)).toArray()) {
            const auto object = item.toObject();
            Account account{
                .cli = QLatin1String(cli),
                .name = object.value(QStringLiteral("name")).toString().trimmed(),
                .email = object.value(QStringLiteral("email")).toString().trimmed().toLower(),
                .home = {},
                .hasHome = object.contains(QStringLiteral("home")),
                .machines = {}};
            if (!name.match(account.name).hasMatch() || config.accounts.size() >= kMostAccounts ||
                std::any_of(config.accounts.cbegin(), config.accounts.cend(),
                            [&account](const Account& other) {
                                return other.cli == account.cli && other.name == account.name;
                            }))
                continue;
            if (account.hasHome) {
                const auto home = machine_name(object.value(QStringLiteral("home")));
                if (!home)
                    continue;
                account.home = *home;
            }
            for (const auto& machine : object.value(QStringLiteral("machines")).toArray())
                if (const auto parsed = machine_name(machine);
                    parsed && !account.machines.contains(*parsed))
                    account.machines << *parsed;
            config.accounts.push_back(std::move(account));
        }
    }
    return config;
}

bool AccountPool::configured(const QString& cli) const {
    return std::any_of(config_.accounts.cbegin(), config_.accounts.cend(),
                       [&cli](const Account& account) { return account.cli == cli; });
}

const Account* AccountPool::find(const QString& cli, const QString& name) const {
    const auto found = std::find_if(
        config_.accounts.cbegin(), config_.accounts.cend(),
        [&](const Account& account) { return account.cli == cli && account.name == name; });
    return found == config_.accounts.cend() ? nullptr : &*found;
}

const Account* AccountPool::own(const QString& cli, const QString& machine) const {
    const auto found = std::find_if(
        config_.accounts.cbegin(), config_.accounts.cend(), [&](const Account& account) {
            return account.cli == cli && account.hasHome && account.home == machine;
        });
    return found == config_.accounts.cend() ? nullptr : &*found;
}

bool AccountPool::usable(const Account& account, const QString& machine) const {
    return (account.hasHome && account.home == machine) || account.machines.contains(machine);
}

AccountLoad AccountPool::load(const Account& account) const {
    // By its email where a report names it, and by its home machine's sign-in.
    auto load = account.email.isEmpty()
                    ? AccountLoad{}
                    : loads_.value(account_load_key(account.cli, account.email));
    if (account.hasHome) {
        const auto home =
            loads_.value(account_load_key(account.cli, account_home_name(account.home)));
        load.used = std::max(load.used, home.used);
        load.shortWindow = std::max(load.shortWindow, home.shortWindow);
    }
    return load;
}

bool AccountPool::below(const Account& account) const {
    const auto used = load(account).used;
    return used < 0 || used < config_.switchAt;
}

bool AccountPool::full(const QString& cli, const QString& machine, const QString& current) const {
    const auto* account = current.isEmpty() ? own(cli, machine) : find(cli, current);
    return account != nullptr && !below(*account);
}

QString AccountPool::choose(const QString& cli, const QString& machine,
                            const QString& current) const {
    const auto* now = current.isEmpty() ? own(cli, machine) : find(cli, current);
    if (now != nullptr && usable(*now, machine) && below(*now))
        return now->name;
    const auto* mine = own(cli, machine);
    if (mine != nullptr && below(*mine))
        return mine->name;
    // A machine whose own sign-in lapis does not know keeps it until told.
    if (current.isEmpty() && mine == nullptr)
        return {};
    const auto candidates = ranked(cli, machine);
    if (candidates.empty())
        return now != nullptr && usable(*now, machine) ? now->name
               : mine != nullptr                       ? mine->name
                                                       : QString();
    return candidates.front()->name;
}

QString AccountPool::alternative(const QString& cli, const QString& machine,
                                 const QString& current) const {
    const auto* now = current.isEmpty() ? own(cli, machine) : find(cli, current);
    for (const auto* account : ranked(cli, machine))
        if (account != now)
            return account->name;
    return now != nullptr ? now->name : current;
}

std::vector<const Account*> AccountPool::ranked(const QString& cli, const QString& machine) const {
    std::vector<const Account*> candidates;
    for (const auto& account : config_.accounts)
        if (account.cli == cli && usable(account, machine) && below(account))
            candidates.push_back(&account);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [this](const Account* left, const Account* right) {
                         const auto a = load(*left);
                         const auto b = load(*right);
                         const bool a_hot = a.shortWindow >= kHotShortWindow;
                         const bool b_hot = b.shortWindow >= kHotShortWindow;
                         if (a_hot != b_hot)
                             return !a_hot;
                         const bool a_measured = a.used >= 0;
                         const bool b_measured = b.used >= 0;
                         if (a_measured != b_measured)
                             return a_measured;
                         return a.used < b.used;
                     });
    return candidates;
}

// The two folders are distinct in role and named at every call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void link_codex_home(const QString& shared, const QString& home) {
    const QDir from(shared);
    const QDir to(home);
    for (const auto& entry : from.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                QDir::NoDotAndDotDot)) {
        const auto name = entry.fileName();
        const QFileInfo target(to.filePath(name));
        if (name == QLatin1String("auth.json") || target.exists() || target.isSymLink())
            continue;
        QFile::link(entry.absoluteFilePath(), target.filePath());
    }
}

} // namespace lapis::desktop
