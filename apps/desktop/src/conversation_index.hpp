#ifndef LAPIS_DESKTOP_CONVERSATION_INDEX_HPP
#define LAPIS_DESKTOP_CONVERSATION_INDEX_HPP
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lapis::desktop {
// A conversation someone had with an agent CLI on this Mac, as that CLI saved
// it: enough to list it and to resume it in its folder.
struct Conversation {
    QString harness; // "claude" or "codex"
    QString id;      // what the CLI's resume option takes
    QString directory;
    QString title; // the CLI's own title, else the first thing typed
    qint64 modified{};
};

namespace conversations {
// A Claude Code session file. Only sessions someone opened in a terminal
// (entrypoint "cli") count; -p and SDK runs are automation.
[[nodiscard]] std::optional<Conversation> read_claude(const QString& path);
// A Codex rollout. Only interactive main threads count: not `codex exec`,
// not subagents. `names` are Codex's thread names by id.
[[nodiscard]] std::optional<Conversation> read_codex(const QString& path,
                                                     const QHash<QString, QString>& names);
// Codex's thread names (session_index.jsonl), the latest name for each id.
[[nodiscard]] QHash<QString, QString> codex_thread_names(const QString& path);
// How active each folder has been: every conversation there adds
// 0.5^(age / 14 days), so recent and frequent work both count, and each
// folder in `open` (the agents running now) adds 1.
[[nodiscard]] QHash<QString, double> folder_heat(const std::vector<Conversation>& all,
                                                 qint64 now_ms, const QStringList& open = {});
// A folder's children for a picker: the `hot` most active (counting work in
// folders under each) first, then the rest by name with names that start
// with an underscore last.
[[nodiscard]] QStringList order_children(const QHash<QString, double>& heat, const QString& parent,
                                         const QStringList& names, int hot = 10);
// "now", "5 min", "3 h", "yesterday", "4 d", or a date.
[[nodiscard]] QString age_text(qint64 then_ms, qint64 now_ms);
} // namespace conversations

// The conversations of this Mac's agent CLIs, read in the background and
// cached by file size and time, so a later scan reads only what changed.
class ConversationIndex final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
  public:
    ConversationIndex(QString claude_home, QString codex_home, QString cache_path,
                      QObject* parent = nullptr);
    ~ConversationIndex() override;
    [[nodiscard]] bool ready() const { return ready_; }
    // Rescans in the background; `changed` follows.
    Q_INVOKABLE void refresh();
    // Newest first. Every word of `query` must appear in the title, the
    // folder or the CLI. {harness, id, directory, place, title, when}.
    Q_INVOKABLE [[nodiscard]] QVariantList recent(const QString& query, int limit = 40) const;
    // order_children() for `parent`, with this index's activity.
    Q_INVOKABLE [[nodiscard]] QStringList orderFolders(const QString& parent,
                                                       const QStringList& names) const;
    // The folders of the agents running now, which also count as activity.
    void setOpenFolders(std::function<QStringList()> open) { open_folders_ = std::move(open); }
    // Tests and the scan's result.
    void setConversations(std::vector<Conversation> all);
    [[nodiscard]] const std::vector<Conversation>& all() const { return all_; }
  signals:
    void changed();

  private:
    QString claude_home_;
    QString codex_home_;
    QString cache_path_;
    std::vector<Conversation> all_; // newest first
    std::function<QStringList()> open_folders_;
    std::shared_ptr<std::atomic_bool> scanning_{std::make_shared<std::atomic_bool>(false)};
    std::shared_ptr<std::atomic_bool> alive_{std::make_shared<std::atomic_bool>(true)};
    bool ready_{};
};
} // namespace lapis::desktop
#endif
