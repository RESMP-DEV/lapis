#include "accounts.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::account_home_name;
using lapis::desktop::account_load_key;
using lapis::desktop::AccountLoad;
using lapis::desktop::AccountPool;
using lapis::desktop::parse_accounts;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

lapis::desktop::AccountsConfig sample() {
    const auto root = QJsonDocument::fromJson(R"({"accounts": {"switchAt": 90,
        "claude": [
            {"name": "mac", "email": "A@Example.com", "home": "local", "machines": ["devbox", "local"]},
            {"name": "box", "email": "b@example.com", "home": "devbox", "machines": ["local"]},
            {"name": "spare", "email": "c@example.com", "machines": ["local", "-oProxyCommand=x"]},
            {"name": "bad name", "home": "local"},
            {"name": "mac", "home": "devbox"}],
        "codex": [{"name": "one", "email": "d@example.com", "home": "local"}]}})")
                          .object();
    return parse_accounts(root.value(QStringLiteral("accounts")));
}

void parsesTheConfig() {
    const auto config = sample();
    require(config.switchAt == 90, "the switch point is read");
    require(config.accounts.size() == 4, "bad names and duplicates are left out");
    const auto& mac = config.accounts.at(0);
    require(mac.email == QStringLiteral("a@example.com") && mac.hasHome && mac.home.isEmpty() &&
                mac.machines == QStringList({QStringLiteral("devbox"), QString()}),
            "local is this Mac, and emails compare lowercased");
    const auto& spare = config.accounts.at(2);
    require(!spare.hasHome && spare.machines == QStringList{QString()},
            "a plan no machine signs in as, and never an option for a host");
    require(parse_accounts(QJsonValue()).accounts.empty(), "no section, no plans");
}

void choosesAsOmpRanksAccounts() {
    AccountPool pool;
    pool.setConfig(sample());
    const auto claude = QStringLiteral("claude");
    const QString local;
    const auto devbox = QStringLiteral("devbox");
    require(pool.choose(claude, local, {}) == QStringLiteral("mac") &&
                pool.choose(claude, devbox, {}) == QStringLiteral("box"),
            "a new session takes its machine's own sign-in");
    require(pool.choose(claude, QStringLiteral("elsewhere"), {}).isEmpty(),
            "a machine whose sign-in is unknown keeps it");
    require(pool.choose(QStringLiteral("codex"), local, {}) == QStringLiteral("one"),
            "each CLI has its own plans");

    pool.setLoads({{account_load_key(claude, QStringLiteral("a@example.com")), {95, 20}}});
    require(pool.full(claude, local, {}), "past the switch point is full");
    require(pool.choose(claude, local, {}) == QStringLiteral("box"),
            "a full sign-in hands over to a plan the machine can use, in the config's order");
    require(pool.choose(claude, local, QStringLiteral("spare")) == QStringLiteral("spare"),
            "a session keeps a plan with room");
    pool.setLoads({{account_load_key(claude, QStringLiteral("a@example.com")), {95, 20}},
                   {account_load_key(claude, QStringLiteral("c@example.com")), {40, 90}},
                   {account_load_key(claude, account_home_name(devbox)), {60, 10}}});
    require(pool.load(*pool.find(claude, QStringLiteral("box"))).used == 60,
            "a plan's load is also its home machine's sign-in's");
    require(pool.choose(claude, local, {}) == QStringLiteral("box"),
            "a nearly spent five hour window ranks behind a cool one");
    require(pool.alternative(claude, local, QStringLiteral("box")) == QStringLiteral("spare"),
            "a switch asked for takes the next plan with room");
    pool.setLoads({{account_load_key(claude, QStringLiteral("a@example.com")), {99, 99}},
                   {account_load_key(claude, QStringLiteral("c@example.com")), {97, 50}},
                   {account_load_key(claude, account_home_name(devbox)), {98, 50}}});
    require(pool.choose(claude, local, {}) == QStringLiteral("mac") &&
                pool.alternative(claude, local, QStringLiteral("mac")) == QStringLiteral("mac"),
            "with every plan full a session stays where it is");
}

void linksACodexHome() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary folder");
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("shared/sessions")) && root.mkpath(QStringLiteral("home")),
            "folders");
    for (const auto* name : {"shared/config.toml", "shared/auth.json", "shared/.hidden",
                             "home/auth.json", "home/config.toml"}) {
        QFile file(root.filePath(QLatin1String(name)));
        require(file.open(QIODevice::WriteOnly), "write a file");
        file.write(name);
    }
    require(lapis::desktop::link_codex_home(root.filePath(QStringLiteral("shared")),
                                            root.filePath(QStringLiteral("home")))
                .isEmpty(),
            "the account home is prepared successfully");
    require(QFileInfo(root.filePath(QStringLiteral("home/sessions"))).isSymLink() &&
                QFileInfo(root.filePath(QStringLiteral("home/.hidden"))).isSymLink(),
            "shared entries are linked in");
    require(!QFileInfo(root.filePath(QStringLiteral("home/auth.json"))).isSymLink() &&
                !QFileInfo(root.filePath(QStringLiteral("home/config.toml"))).isSymLink(),
            "the plan's own login and anything already there stay");
    require(
        !lapis::desktop::link_codex_home(root.filePath("missing"), root.filePath("home")).isEmpty(),
        "a missing shared home is an activation failure");
    require(
        !lapis::desktop::link_codex_home(root.filePath("shared"), root.filePath("home/auth.json"))
             .isEmpty(),
        "a non-directory account home is an activation failure");
    require(root.mkpath("blocked"), "a read-only account home");
    const auto blocked = root.filePath("blocked");
    const auto permissions = QFileInfo(blocked).permissions();
    require(QFile::setPermissions(blocked, QFile::ReadOwner | QFile::ExeOwner),
            "make home unwritable");
    const auto diagnostic = lapis::desktop::link_codex_home(root.filePath("shared"), blocked);
    require(QFile::setPermissions(blocked, permissions), "restore home permissions");
    require(diagnostic.contains("Could not link"),
            "a failed link cannot report successful preparation");
}
} // namespace

int main() {
    try {
        parsesTheConfig();
        choosesAsOmpRanksAccounts();
        linksACodexHome();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "account tests passed\n";
    return 0;
}
