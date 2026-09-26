#include "conversation_index.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::Conversation;
using lapis::desktop::ConversationIndex;
namespace conversations = lapis::desktop::conversations;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

constexpr const char* kFirst = "0a1b2c3d-0000-4000-8000-000000000001";
constexpr const char* kSecond = "0a1b2c3d-0000-4000-8000-000000000002";
constexpr const char* kThird = "0a1b2c3d-0000-4000-8000-000000000003";

QByteArray line(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "fixture written");
}

QJsonObject user(const QJsonValue& content, bool meta = false) {
    QJsonObject entry{{"type", "user"},
                      {"message", QJsonObject{{"role", "user"}, {"content", content}}}};
    if (meta)
        entry.insert("isMeta", true);
    return entry;
}

QByteArray claude_session(const QString& entrypoint, const QString& cwd) {
    return line({{"type", "queue-operation"}}) +
           line({{"type", "system"}, {"entrypoint", entrypoint}, {"cwd", cwd}});
}

void claude_sessions_are_read_only_when_someone_opened_them() {
    QTemporaryDir root;
    const auto path = [&](const QString& id) { return root.filePath(id + ".jsonl"); };
    // The first typed message names it; wrappers and meta records do not.
    write(path(kFirst),
          claude_session("cli", "/work/lapis/") +
              line(user("<command-name>/clear</command-name>")) +
              line(user("Caveat: local command output")) + line(user("hidden", true)) +
              line(user(
                  QJsonArray{QJsonObject{{"type", "text"}, {"text", "  fix the\n resize  "}}})));
    const auto first = conversations::read_claude(path(kFirst));
    require(first && first->harness == "claude" && first->id == kFirst &&
                first->directory == "/work/lapis" && first->title == "fix the resize",
            "a terminal session is named by what was typed first");
    // Claude Code's own title wins, including one written at the end.
    QByteArray long_session = claude_session("cli", "/work/x") + line(user("typed"));
    for (int i = 0; i < 400; ++i)
        long_session += line({{"type", "attachment"}, {"n", i}});
    long_session += line({{"type", "ai-title"}, {"aiTitle", "Resize bug"}, {"sessionId", kSecond}});
    write(path(kSecond), long_session);
    const auto second = conversations::read_claude(path(kSecond));
    require(second && second->title == "Resize bug", "the newest ai-title names the session");
    write(path(kThird), claude_session("sdk-cli", "/work/x") + line(user("automation")));
    require(!conversations::read_claude(path(kThird)), "SDK and -p runs are not conversations");
    write(root.filePath("notes.jsonl"), claude_session("cli", "/work/x"));
    require(!conversations::read_claude(root.filePath("notes.jsonl")),
            "only session files named by their id");
}

QByteArray rollout(const QJsonObject& meta, const QString& typed) {
    const QJsonObject item{
        {"type", "message"},
        {"role", "user"},
        {"content", QJsonArray{QJsonObject{{"type", "input_text"}, {"text", typed}}}}};
    const QJsonObject context{
        {"type", "message"},
        {"role", "user"},
        {"content",
         QJsonArray{QJsonObject{{"type", "input_text"}, {"text", "<environment_context>x"}}}}};
    return line({{"type", "session_meta"}, {"payload", meta}}) +
           line({{"type", "response_item"}, {"payload", context}}) +
           line({{"type", "response_item"}, {"payload", item}});
}

void codex_rollouts_are_interactive_main_threads() {
    QTemporaryDir root;
    const auto path = root.filePath("rollout-a.jsonl");
    write(path, rollout({{"id", kFirst},
                         {"cwd", "/work/api"},
                         {"source", "cli"},
                         {"thread_source", "user"},
                         {"parent_thread_id", QJsonValue()}},
                        "add retries"));
    const auto found = conversations::read_codex(path, {});
    require(found && found->harness == "codex" && found->id == kFirst &&
                found->directory == "/work/api" && found->title == "add retries",
            "an interactive rollout, named by its first typed message");
    const auto named = conversations::read_codex(path, {{kFirst, "Retry work"}});
    require(named && named->title == "Retry work", "Codex's thread name wins");
    write(path, rollout({{"id", kFirst}, {"cwd", "/w"}, {"source", "exec"}}, "x"));
    require(!conversations::read_codex(path, {}), "codex exec is automation");
    write(path,
          rollout(
              {{"id", kFirst}, {"cwd", "/w"}, {"source", QJsonObject{{"subagent", QJsonObject{}}}}},
              "x"));
    require(!conversations::read_codex(path, {}), "subagents are not conversations");
    const auto index = root.filePath("session_index.jsonl");
    write(index, line({{"id", kFirst}, {"thread_name", "old"}}) +
                     line({{"id", kFirst}, {"thread_name", "new name"}}) + "not json\n");
    require(conversations::codex_thread_names(index).value(kFirst) == "new name",
            "the latest thread name wins");
}

void activity_orders_folders() {
    const qint64 now = qint64{1'800'000'000'000};
    const qint64 minute = qint64{60'000};
    const qint64 hour = 60 * minute;
    const qint64 day = 24 * hour;
    const std::vector<Conversation> all{
        {"claude", kFirst, "/home/dev/lapis/apps", "", now},
        {"codex", kSecond, "/home/dev/lapis", "", now - 14 * day},
        {"codex", kThird, "/home/dev/old", "", now - 140 * day},
    };
    const auto heat = conversations::folder_heat(all, now, {"/home/dev/zeta/"});
    require(std::abs(heat.value("/home/dev/lapis") - 0.5) < 1e-9 &&
                std::abs(heat.value("/home/dev/lapis/apps") - 1.0) < 1e-9 &&
                heat.value("/home/dev/zeta") == 1.0,
            "a conversation counts half as much every two weeks; open agents count 1");
    const QStringList names{"_scratch", "beta", "lapis", "Alpha", "old", "zeta", "_attic"};
    require(conversations::order_children(heat, "/home/dev/", names) ==
                QStringList({"lapis", "zeta", "old", "Alpha", "beta", "_attic", "_scratch"}),
            "active folders first (work below a folder counts), then by name, underscores last");
    require(conversations::order_children(heat, "/home/dev", names, 1) ==
                QStringList({"lapis", "Alpha", "beta", "old", "zeta", "_attic", "_scratch"}),
            "past the hot few, active folders sort by name too");
    require(conversations::order_children({}, "/", {"b", "_a", "A"}) ==
                QStringList({"A", "b", "_a"}),
            "without activity the order is by name");
    require(conversations::age_text(now - 30'000, now) == "now" &&
                conversations::age_text(now - 5 * minute, now) == "5 min" &&
                conversations::age_text(now - 3 * hour, now) == "3 h" &&
                conversations::age_text(now - 30 * hour, now) == "yesterday" &&
                conversations::age_text(now - 4 * day, now) == "4 d",
            "ages read as short words");
}

bool wait_for(ConversationIndex& index) {
    QEventLoop loop;
    QTimer::singleShot(10'000, &loop, &QEventLoop::quit);
    QObject::connect(&index, &ConversationIndex::changed, &loop, &QEventLoop::quit);
    index.refresh();
    loop.exec();
    return index.ready();
}

void the_index_scans_in_the_background_and_caches() {
    QTemporaryDir root;
    const auto claude = root.filePath("claude");
    const auto codex = root.filePath("codex");
    const auto cache = root.filePath("conversations.json");
    write(claude + "/projects/-work-lapis/" + kFirst + ".jsonl",
          claude_session("cli", "/work/lapis") + line(user("first task")));
    write(codex + "/sessions/2026/09/25/rollout-x.jsonl",
          rollout({{"id", kSecond}, {"cwd", "/work/api"}, {"source", "cli"}}, "api task"));
    write(codex + "/session_index.jsonl", line({{"id", kSecond}, {"thread_name", "API"}}));
    ConversationIndex index(claude, codex, cache);
    require(wait_for(index) && index.all().size() == 2, "both CLIs' conversations are found");
    const auto rows = index.recent("", 10);
    require(rows.size() == 2, "recent lists every conversation");
    // "lapis" contains "api": a query word matches anywhere in a field.
    require(index.recent("API", 10).size() == 2, "words match inside names");
    const auto api = index.recent("Codex", 10);
    require(api.size() == 1 && api.front().toMap().value("id").toString() == kSecond &&
                api.front().toMap().value("title").toString() == "API",
            "every word of the query must match the title, folder or CLI");
    require(QFile::exists(cache), "the scan is cached");
    // A renamed thread is picked up without its rollout changing.
    write(codex + "/session_index.jsonl", line({{"id", kSecond}, {"thread_name", "Renamed"}}));
    require(wait_for(index) && index.recent("renamed", 10).size() == 1,
            "names apply over the cache");
    require(index.orderFolders("/work", {"api", "lapis", "_x", "b"}).mid(2) ==
                QStringList({"b", "_x"}),
            "the index orders a folder's children by its activity");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        claude_sessions_are_read_only_when_someone_opened_them();
        codex_rollouts_are_interactive_main_threads();
        activity_orders_folders();
        the_index_scans_in_the_background_and_caches();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "conversation index tests passed\n";
    return 0;
}
