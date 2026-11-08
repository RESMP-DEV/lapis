#ifndef LAPIS_DESKTOP_TRANSCRIPT_HPP
#define LAPIS_DESKTOP_TRANSCRIPT_HPP
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

class QQuickTextDocument;
class QTextDocument;

namespace lapis::desktop {
// A visual, read-only reading of an agent CLI's saved conversation. The
// terminal stays the source of truth, and what the view sends goes through it; this
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
    qint64 at_ms{};       // when it was written (ms since the epoch), or 0
    bool starts_turn{};   // a prompt, or a background task waking the agent
    qint64 duration_ms{}; // a turn item: how long the turn took
};

// The model, effort and context use the agent last reported, read the same
// way for every CLI so the bar shows the same things for each.
struct Status {
    QString model;          // as the API names it ("claude-opus-5-5")
    QString effort;         // "low" ... "max", or empty when not reported
    qint64 context_tokens{}; // what the last request sent and got back
    bool operator==(const Status&) const = default;
};
// The model's context window in tokens, from Claude Code's own table: 1M for
// the current Opus, Fable, Sonnet 4.6+ and Haiku 4.5 (and any "[1m]" name),
// else 200k; never less than what was already used.
[[nodiscard]] qint64 context_window(const QString& model, qint64 used);

// The thread as shown: each finished turn folds to its prompt, one "Worked
// for" row standing for its work (thinking, steps, notes between), and its
// final reply. A turn still running shows everything as it happens.
struct Turn {
    int start{};          // its first item (the prompt)
    int last{};           // its last item
    int final_reply{-1};  // the reply that ends it, or -1
    int steps{};          // items folded under its "Worked for" row
    qint64 duration_ms{}; // from the CLI, else from the items' times
    bool closed{};        // finished: by its duration record or the next turn
    // What the folded work did, as the Codex app sums it up: "ran 4
    // commands, edited 2 files, read 3 files".
    QString summary;
};
// The summary for the work items in [first, last], skipping `except`.
[[nodiscard]] QString work_summary(const std::vector<Item>& items, int first, int last, int except);
struct Row {
    int item{-1}; // the item shown, or -1 for a turn's "Worked for" row
    int turn{-1}; // the turn it belongs to, or -1
    bool in_work{}; // folded work, shown because its turn is open
};
struct Layout {
    std::vector<Turn> turns;
    std::vector<Row> rows;
};
// `expanded` holds the first items of the turns whose work is shown.
[[nodiscard]] Layout lay_out(const std::vector<Item>& items, const QSet<int>& expanded);

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

// Text fit to paste anywhere: Qt's paragraph and line separators become
// newlines, no-break spaces plain spaces, invisible characters go, and
// trailing spaces and blank lines are trimmed.
[[nodiscard]] QString clean_copy(QString text);

// Part of a laid-out reply as text to paste: paragraphs separated by a blank
// line, list items as "- " or "1. " lines, cleaned as above.
[[nodiscard]] QString selection_text(const QTextDocument& document, int start, int end);

// The folder agents' chart styles live in (~/.lapis/visual); lapis points
// local agents' MATPLOTLIBRC there once a style is written.
[[nodiscard]] QString chart_style_folder();

// Paths in a message to local media the view can show, in order, at most 8:
// Markdown image and link targets, then bare absolute or ~ paths with a media
// extension. Only files that exist count.
struct Media {
    QString path;
    QString kind; // image, animation, svg, audio, video, html, pdf, file
};
[[nodiscard]] std::vector<Media> find_media(const QString& text);

// Folds Claude Code session JSONL records, in file order, into items. Tool
// results attach to the tool call that asked for them; a subagent's records
// (isSidechain) are skipped.
class ClaudeReader {
  public:
    // Index of the first item changed by this line, or -1 when none was;
    // appended items follow it.
    int add(const QByteArray& line);
    [[nodiscard]] const std::vector<Item>& items() const { return items_; }
    [[nodiscard]] const Status& status() const { return status_; }

  private:
    void push(Item item);
    std::vector<Item> items_;
    Status status_;
    qint64 at_ms_{}; // time of the record being read
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
    // What the agent last reported, the same for every CLI: its model, its
    // effort, and its context use against the model's window.
    Q_PROPERTY(QString model READ model NOTIFY statusChanged)
    Q_PROPERTY(QString effort READ effort NOTIFY statusChanged)
    Q_PROPERTY(qint64 contextTokens READ contextTokens NOTIFY statusChanged)
    Q_PROPERTY(qint64 contextWindow READ contextWindow NOTIFY statusChanged)
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
        ExpandedRole, // a "Worked for" row whose work is shown
        InWorkRole,   // an item shown inside an opened "Worked for"
        StepsRole,    // a "Worked for" row: how many items it folds
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
    // Underlines the links to `link` in a styled document, and no others;
    // an empty link clears them. Called as the pointer moves over links.
    Q_INVOKABLE void hoverLink(QQuickTextDocument* target, const QString& link) const;
    // Colors code in a TextEdit's document by its language, with the Tokyo
    // Night palette (`light` for its Day variant). Replaces any earlier one.
    Q_INVOKABLE void highlight(QQuickTextDocument* target, const QString& language, bool light) const;
    // The slash commands for a Claude Code agent working in `folder`, as
    // {name, description, source}.
    Q_INVOKABLE [[nodiscard]] QVariantList slashCommands(const QString& folder) const;
    // Local files a message names that the view can show inline: Markdown
    // images and links, and absolute or ~ paths to images, audio, video,
    // HTML and PDF that exist. Each {path, url, kind, name, size}; kind is
    // image, animation (GIF), svg, audio, video, html or pdf.
    Q_INVOKABLE [[nodiscard]] QVariantList media(const QString& text) const;
    // Writes the chart style agents' plots follow (a matplotlibrc and a
    // palette.json under ~/.lapis/visual) from the view's colors: background,
    // text, muted, grid and light. Unchanged files are left alone.
    Q_INVOKABLE void writeChartStyle(const QVariantMap& colors) const;
    // Font families lapis loads from the system for reading: serif, mono.
    Q_INVOKABLE [[nodiscard]] QVariantMap fonts() const;
    // Copies text to the clipboard as plain text only (no styled HTML to
    // carry the theme's colors elsewhere), cleaned of the separators and
    // invisible characters Qt's text selection produces.
    Q_INVOKABLE void copy(const QString& text) const;
    // Copies part of a styled reply: paragraphs separated by a blank line,
    // list items as "- " or "1. " lines, everything else plain.
    Q_INVOKABLE void copySelection(QQuickTextDocument* source, int start, int end) const;
    // Opens or folds the work of the turn a "Worked for" row stands for.
    Q_INVOKABLE void toggleWork(int row);
    // What a paste attaches: files copied in the Finder as their paths, or
    // an image (a screenshot) saved as a PNG; empty when the clipboard holds
    // neither, so the paste stays text.
    Q_INVOKABLE [[nodiscard]] QStringList pastedFiles() const;
    // A file the CLI should see as an image rather than read as text.
    Q_INVOKABLE [[nodiscard]] bool isImage(const QString& path) const;
    // A local file path for a dropped or picked file URL, or empty.
    Q_INVOKABLE [[nodiscard]] QString localPath(const QUrl& url) const;

    [[nodiscard]] const QString& model() const { return reader_.status().model; }
    [[nodiscard]] const QString& effort() const { return reader_.status().effort; }
    [[nodiscard]] qint64 contextTokens() const { return reader_.status().context_tokens; }
    [[nodiscard]] qint64 contextWindow() const {
        return transcript::context_window(reader_.status().model, reader_.status().context_tokens);
    }

  signals:
    void pathChanged();
    void loadingChanged();
    void countChanged();
    void statusChanged();

  private:
    void apply(transcript::ClaudeReader reader, qint64 offset, QByteArray pending);
    // Lays the thread out again and tells the view only what moved: rows
    // that stayed are kept, `changed` (an item index) and later are redrawn.
    void relayout(int changed);
    // Emits statusChanged when the model, effort or context use moved.
    void noteStatus();
    QSet<int> expanded_; // first items of turns whose work is shown
    transcript::Layout layout_;
    // Conversations read recently stay parsed, so switching back to an agent
    // shows its thread at once and reads only what it wrote since.
    struct Parsed {
        QString path;
        transcript::ClaudeReader reader;
        qint64 offset{};
        QByteArray pending;
    };
    static constexpr std::size_t kept_conversations = 6;
    std::vector<Parsed> kept_;
    QString path_;
    transcript::ClaudeReader reader_;
    transcript::Status shown_status_;
    qint64 offset_{};
    QByteArray pending_; // a line the CLI has not finished writing
    bool loading_{};
    quint64 generation_{};
};
} // namespace lapis::desktop
#endif
