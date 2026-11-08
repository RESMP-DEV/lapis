#ifndef LAPIS_DESKTOP_TRANSCRIPT_HPP
#define LAPIS_DESKTOP_TRANSCRIPT_HPP
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

class QQuickTextDocument;

namespace lapis::desktop {
// A visual, read-only reading of an agent CLI's saved conversation. The
// terminal stays the source of truth and the only place input goes; this
// shows the same work as messages and one-line steps. Kinds it does not know
// become `other` blocks, so a CLI feature lapis has not learned yet still
// shows up instead of vanishing.
namespace transcript {
enum class Kind { prompt, reply, thinking, tool, command, notice, divider, turn, other };
[[nodiscard]] QString kind_name(Kind kind);

// One line of a tool's diff: '+' added, '-' removed, ' ' context.
struct DiffLine {
    QChar sign;
    QString text;
};

struct Item {
    Kind kind{Kind::other};
    // A step's past-tense verb ("Ran", "Edited"), a divider's or notice's
    // text, a command's name, an other block's record type.
    QString title;
    // A step's target (the command, the file), or a message's text.
    QString body;
    // A step's description, or an other block's raw JSON.
    QString detail;
    QString result; // a tool's output, shortened
    std::vector<DiffLine> diff;
    int added{};   // diff lines added and removed, counted in full
    int removed{};
    int result_lines{}; // lines in the full output
    bool failed{};      // the tool or the API reported an error
    bool answered{};    // a tool result arrived
    bool after_step{};  // follows another step: drawn tight against it
    QString tool_id;
    // The saved record this item came from, so a fork can cut there.
    QString record;
};

// Limits that keep one huge tool output or file write from costing the view.
inline constexpr int result_preview_lines = 14;
inline constexpr qsizetype result_preview_chars = 4000;
inline constexpr int diff_preview_lines = 40;
inline constexpr qsizetype body_chars = 24000;

// "12s", "3m 12s", "1h 4m".
[[nodiscard]] QString duration_text(qint64 ms);

// A reply split at its fenced code blocks: prose stays markdown, and each
// block is its own piece so it can be copied whole.
struct Piece {
    bool code{};
    QString language;
    QString text;
};
[[nodiscard]] std::vector<Piece> pieces(const QString& markdown);

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
    void push(Item item);
    std::vector<Item> items_;
    QString record_; // uuid of the record being read
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
        AddedRole,
        RemovedRole,
        ResultLinesRole,
        FailedRole,
        AnsweredRole,
        AfterStepRole,
        RecordRole,
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
    // A reply's prose and code pieces: [{code, language, text}].
    Q_INVOKABLE [[nodiscard]] QVariantList pieces(const QString& markdown) const;
    // Lays markdown into a TextEdit's document with the view's fonts and
    // sizes, so headings, lists and inline code keep one scale and the text
    // stays selectable. `style` holds sans, mono, size, codeSize, codeColor
    // and linkColor.
    Q_INVOKABLE void style(QQuickTextDocument* target, const QString& markdown,
                           const QVariantMap& style) const;
    // Font families lapis loads from the system for reading: serif, mono.
    Q_INVOKABLE [[nodiscard]] QVariantMap fonts() const;
    // Copies text to the clipboard.
    Q_INVOKABLE void copy(const QString& text) const;

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
