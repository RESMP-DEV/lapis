#ifndef LAPIS_DESKTOP_TRANSCRIPT_HPP
#define LAPIS_DESKTOP_TRANSCRIPT_HPP
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <vector>

namespace lapis::desktop {
// A visual, read-only reading of an agent CLI's saved conversation. The
// terminal stays the source of truth and the only place input goes; this
// shows the same work as messages and tool cards. Kinds it does not know
// become `other` blocks, so a CLI feature lapis has not learned yet still
// shows up instead of vanishing.
namespace transcript {
enum class Kind { prompt, reply, thinking, tool, command, notice, divider, other };
[[nodiscard]] QString kind_name(Kind kind);

// One line of a tool's diff: '+' added, '-' removed, ' ' context.
struct DiffLine {
    QChar sign;
    QString text;
};

struct Item {
    Kind kind{Kind::other};
    QString title;  // tool: its name; command: the command; other: the record type
    QString body;   // prompt/reply/thinking text, or a tool's main argument
    QString detail; // a tool's description, or an other block's raw JSON
    QString result; // a tool's output, shortened
    std::vector<DiffLine> diff;
    int result_lines{};   // lines in the full output
    bool failed{};        // the tool reported an error
    bool answered{};      // a tool result arrived
    QString tool_id;
};

// Limits that keep one huge tool output or file write from costing the view.
inline constexpr int result_preview_lines = 14;
inline constexpr qsizetype result_preview_chars = 4000;
inline constexpr int diff_preview_lines = 40;
inline constexpr qsizetype body_chars = 24000;

// Folds Claude Code session JSONL records, in file order, into items. Tool
// results attach to the tool call that asked for them; a subagent's records
// (isSidechain) are skipped.
class ClaudeReader {
  public:
    // Index of the first item changed by this line, or -1 when none was;
    // appended items follow it.
    int add(const QByteArray& line);
    [[nodiscard]] const std::vector<Item>& items() const { return items_; }

  private:
    std::vector<Item> items_;
    QHash<QString, int> tools_; // tool_use id -> item index
};
} // namespace transcript

// The items of one conversation file for QML, following the file as the CLI
// appends to it. Loading runs off the GUI thread.
class TranscriptModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        TitleRole,
        BodyRole,
        DetailRole,
        ResultRole,
        DiffRole,
        ResultLinesRole,
        FailedRole,
        AnsweredRole,
    };
    explicit TranscriptModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] const QString& path() const { return path_; }
    void setPath(const QString& path);
    [[nodiscard]] bool loading() const { return loading_; }
    // Reads what the file gained since the last read; called by the watcher
    // and by tests.
    Q_INVOKABLE void readMore();
    // Reads the whole file on this thread; for tools that capture once.
    void loadNow();

  signals:
    void pathChanged();
    void loadingChanged();
    void countChanged();

  private:
    void apply(transcript::ClaudeReader reader, qint64 offset, QByteArray pending);
    QString path_;
    transcript::ClaudeReader reader_;
    int shown_{}; // rows the view has been told about
    qint64 offset_{};
    QByteArray pending_; // a line the CLI has not finished writing
    bool loading_{};
    quint64 generation_{};
};
} // namespace lapis::desktop
#endif
