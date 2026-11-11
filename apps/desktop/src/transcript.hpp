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
#include <memory>
#include <optional>
#include <vector>

class QJsonObject;
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
    // A step's description, an other block's raw JSON, or the paths of the
    // images a prompt attached, one per line.
    QString detail;
    QString result; // a tool's or a command's output, shortened
    std::vector<DiffLine> diff;
    int added{}; // diff lines added and removed, counted in full
    int removed{};
    int result_lines{}; // lines in the full output
    bool failed{};      // the tool or the API reported an error
    bool answered{};    // a tool result arrived
    bool after_step{};  // follows another step: drawn tight against it
    QString tool_id;
    // The saved record this item came from, so a fork can cut there.
    QString record;
    qint64 at_ms{};     // when it was written (ms since the epoch), or 0
    bool starts_turn{}; // a prompt, or a background task waking the agent
    // A prompt sent while the agent worked, waiting for it to take it in.
    bool queued{};
    // Withdrawn from the queue, or sent on as a turn of its own: not shown.
    bool dropped{};
    bool quiet{}; // starts a turn without a row of its own (a background task's wake-up)
    // Ends any turn still open and stays outside turns (a /compact the
    // person ran between turns), so it is never folded into a turn's work.
    bool between_turns{};
    qint64 duration_ms{}; // a turn item: how long the turn took
};

// The model, effort and context use the agent last reported, read the same
// way for every CLI so the bar shows the same things for each.
struct Status {
    QString model;           // as the API names it ("claude-opus-5-5")
    QString effort;          // "low" ... "max", or empty when not reported
    qint64 context_tokens{}; // what the last request sent and got back
    // The window the CLI itself reported (Codex does), or 0 to look it up.
    qint64 window_tokens{};
    // When a /compact the person ran started (ms since the epoch) while its
    // compaction is still under way, else 0.
    qint64 compacting_since{};
    bool operator==(const Status&) const = default;
};
// The model's context window in tokens, from Claude Code's own table: 1M for
// the current Opus, Fable, Sonnet 4.6+ and Haiku 4.5 (and any "[1m]" name),
// else 200k; never less than what was already used.
[[nodiscard]] qint64 context_window(const QString& model, qint64 used);
// The window for a status: what the CLI reported, else the table above.
[[nodiscard]] qint64 context_window(const Status& status);

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
    int item{-1};   // the item shown, or -1 for a turn's "Worked for" row
    int turn{-1};   // the turn it belongs to, or -1
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

// The path written around `position` in `text` (absolute, ~, or relative to
// `folder`), resolved, with any ":line[:column]" after it kept, when that
// file or folder exists; else empty.
[[nodiscard]] QString path_at(const QString& text, int position, const QString& folder);

// The live tail: what a running turn shows on Claude Code's screen that the
// thread does not show yet. Claude Code draws, above its input box (the last
// box-rule pair from the bottom), the conversation so far: echoed prompts
// ("❯ text" at column 0), each message or step as a block led by "●" (or
// "⏺") at column 0 with its output under it, and status rows led by a
// spinner glyph ("✻ Thinking…"). The tail is only the part in progress: the
// rows after the last echoed prompt, from the last block on, so earlier
// replies, tables and steps the thread already renders never repeat in it.
// A last block that is a reply the thread already shows (its first row
// starts like one of `shown`, compared by letters and digits) is dropped,
// keeping only the status rows after it. While Claude Code compacts the
// conversation (see compacting_shown) the tail is empty. Blank rows at
// either end are dropped.
[[nodiscard]] QStringList live_lines(const QStringList& screen, const QStringList& shown = {});
// Whether the lowest status row above Claude Code's input box says it is
// compacting the conversation ("✻ Compacting conversation… (1m 3s)"), as a
// /compact or an automatic compaction does.
[[nodiscard]] bool compacting_shown(const QStringList& screen);

// Whether Claude Code's screen shows a dialog (a picker, settings, a search)
// in place of its input box: a full-width rule (a row of at least 20 U+2500
// from column 0) with no input box, which is the lowest two such rules 1 to
// 12 rows apart, the first row between them empty or led by ❯, >, ! or #.
// A screen with no full rule (startup, a shell) shows none.
[[nodiscard]] bool dialog_showing(const QStringList& screen);

// The CLIs whose saved conversations lapis reads, each with its own reader.
enum class Format { claude, codex, antigravity };
[[nodiscard]] QString format_name(Format format);
// A format by name ("claude", "codex", "antigravity"; the CLIs' ids "agy"
// too), or nullopt.
[[nodiscard]] std::optional<Format> format_named(const QString& name);
// The format of a conversation file by its name, as each CLI names them:
// Codex's rollout-*.jsonl, Antigravity's transcript_full.jsonl (or
// transcript.jsonl), and Claude Code's <session>.jsonl otherwise.
[[nodiscard]] Format format_of(const QString& path);

// One CLI's conversation file read into the thread's items: each line, in
// file order, folds into the same item kinds whatever the CLI, so one view
// shows them all. A reader ignores lines it cannot parse or does not know.
class Reader {
  public:
    Reader() = default;
    virtual ~Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;
    // Index of the first item changed by this line, or -1 when none was;
    // appended items follow it.
    virtual int add(const QByteArray& line) = 0;
    [[nodiscard]] virtual Format format() const = 0;
    [[nodiscard]] const std::vector<Item>& items() const { return items_; }
    [[nodiscard]] const Status& status() const { return status_; }

  protected:
    std::vector<Item> items_;
    Status status_;
};
[[nodiscard]] std::unique_ptr<Reader> make_reader(Format format);

// Folds Claude Code session JSONL records, in file order, into items. Tool
// results attach to the tool call that asked for them; a subagent's records
// (isSidechain) are skipped.
class ClaudeReader final : public Reader {
  public:
    int add(const QByteArray& line) override;
    [[nodiscard]] Format format() const override { return Format::claude; }

  private:
    void push(Item item);
    // A slash or bash-mode command's output (<local-command-stdout>,
    // <bash-stdout> and their stderr), attached to the command it answers
    // when nothing else came between. Returns the item changed, or -1.
    int command_output(const QString& raw);
    // A slash command's record (<command-name>, <command-args>).
    void command(const QString& raw);
    qint64 at_ms_{};            // time of the record being read
    QString record_;            // uuid of the record being read
    QHash<QString, int> tools_; // tool_use id -> item index
    // Claude Code's input queue in order: {text, item}, item -1 for entries
    // not shown (background task notices). A dequeue names no text and
    // takes the oldest.
    std::vector<std::pair<QString, int>> queued_;
    // The /compact item Claude Code writes as the person's text when the
    // compaction starts, until its command record (written once it ends)
    // and output come in; -1 when none is waiting.
    int compact_item_{-1};
};

// Folds a Codex rollout (sessions/YYYY/MM/DD/rollout-*.jsonl, as Codex
// 0.157 writes it) into items. The conversation comes from its finished
// items (event_msg item_completed), each written whole with its output, so a
// step shows once it is done. task_started, task_complete and turn_aborted
// bound and time the turns; turn_context, thread_settings_applied and
// token_count report the model, effort and context. The model-facing
// response_item copies of the same work, and the records Codex keeps for
// itself, are skipped.
class CodexReader final : public Reader {
  public:
    int add(const QByteArray& line) override;
    [[nodiscard]] Format format() const override { return Format::codex; }

  private:
    void push(Item item);
    // One finished item (a payload.item) as zero or more items.
    void item(const QJsonObject& item, qint64 started_ms, qint64 completed_ms);
    qint64 at_ms_{};
    QString record_;
    QString turn_;         // the running turn's id, or empty between turns
    bool turn_prompted_{}; // the running turn has its prompt
};

// Folds an Antigravity CLI transcript (brain/<conversation>/.system_generated
// /logs/transcript_full.jsonl, as agy 1.5 appends it) into items. Each line is
// a step: the person's input (USER_INPUT), a model response (thinking, text
// and tool calls), each call's result in call order (GENERIC), and the CLI's
// own messages. A turn ends at a response with text and no tool calls; the
// file records no durations, so turns and steps are timed from their times.
// The model and effort come from the settings change agy notes in a prompt.
class AntigravityReader final : public Reader {
  public:
    int add(const QByteArray& line) override;
    [[nodiscard]] Format format() const override { return Format::antigravity; }

  private:
    void push(Item item);
    // A model step after the turn ended (a timer or background task woke
    // the agent) starts a turn of its own, with no row for its start.
    void wake(const QString& why);
    qint64 at_ms_{};
    QString record_;
    std::vector<int> waiting_; // tool items awaiting their result, in call order
    bool turn_open_{};
    qint64 turn_since_ms_{};
    QSet<qint64> steps_; // step indexes read, so a step written twice counts once
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
    // When a /compact the person ran started (ms since the epoch) while it
    // is still compacting, else 0.
    Q_PROPERTY(double compactingSince READ compactingSince NOTIFY statusChanged)
    // Which CLI's file this is ("claude", "codex", "antigravity"), from its
    // name unless a tool chose one (setFormat).
    Q_PROPERTY(QString format READ formatName NOTIFY pathChanged)
    // Claude Code conversations can be forked from a message (Edit); the
    // others cannot yet.
    Q_PROPERTY(bool forkable READ forkable NOTIFY pathChanged)
    // Whether liveLines and dialogShowing know this CLI's terminal layout
    // (Claude Code's input box); for the others the view shows no live tail
    // and never peeks at a dialog.
    Q_PROPERTY(bool screenKnown READ screenKnown NOTIFY pathChanged)
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
        QueuedRole,   // a prompt sent mid-turn, not yet taken in
        AtRole,       // when it was written (ms since the epoch), or 0
        DurationRole, // how long a step, thought or turn took, in ms, or 0
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
    // (code blocks), inlineCodeColor and inlineCodeTextColor (code inside a
    // sentence), linkColor, borderColor and quoteColor.
    Q_INVOKABLE void style(QQuickTextDocument* target, const QString& markdown,
                           const QVariantMap& style) const;
    // Underlines the links to `link` in a styled document, and no others;
    // an empty link clears them. Called as the pointer moves over links.
    Q_INVOKABLE void hoverLink(QQuickTextDocument* target, const QString& link) const;
    // Colors code in a TextEdit's document by its language, with the Tokyo
    // Night palette (`light` for its Day variant). Replaces any earlier one.
    Q_INVOKABLE void highlight(QQuickTextDocument* target, const QString& language,
                               bool light) const;
    // The slash commands for an agent of `harness` ("claude", "codex",
    // "agy") working in `folder`, as {name, description, source} (Codex
    // and agy add `terminal`); empty for another CLI.
    Q_INVOKABLE [[nodiscard]] QVariantList slashCommands(const QString& harness,
                                                         const QString& folder) const;
    // Local files a message names that the view can show inline: Markdown
    // images and links, and absolute or ~ paths to images, audio, video,
    // HTML and PDF that exist. Each {path, url, kind, name, size}; kind is
    // image, animation (GIF), svg, audio, video, html or pdf.
    Q_INVOKABLE [[nodiscard]] QVariantList media(const QString& text) const;
    // Writes the chart style agents' plots follow (a matplotlibrc and a
    // palette.json under ~/.lapis/visual) from the view's colors: background,
    // text, muted, grid and light. Unchanged files are left alone.
    Q_INVOKABLE void writeChartStyle(const QVariantMap& colors) const;
    // The file or folder path written at `position` in a laid-out message
    // ("/abs/file.cpp", "~/x", "src/a.cpp:42" relative to `folder`), with
    // any ":line[:column]" kept, when it names something on this Mac; else
    // empty.
    Q_INVOKABLE [[nodiscard]] QString pathAt(QQuickTextDocument* source, int position,
                                             const QString& folder) const;
    // The editors on this Mac files can open in: [{id, label}], VS Code,
    // Cursor, Zed and TextEdit as installed, then the system's default app.
    Q_INVOKABLE [[nodiscard]] QVariantList editors() const;
    // Opens a path from pathAt (or a file link) in `editor` (an id from
    // editors(), or auto for the first installed), at its line where the
    // editor takes one; folders open in the Finder.
    Q_INVOKABLE bool openPath(const QString& path, const QString& editor) const;
    // The part of a running turn the terminal screen shows and the thread
    // does not yet (see transcript::live_lines): never an earlier reply,
    // and nothing while the conversation compacts.
    Q_INVOKABLE [[nodiscard]] QStringList liveLines(const QStringList& screen) const;
    // Whether the screen shows Claude Code compacting the conversation.
    Q_INVOKABLE [[nodiscard]] bool compactingShown(const QStringList& screen) const;
    // Whether the terminal shows a dialog in place of the input box (see
    // transcript::dialog_showing). Both are Claude Code's layout: for other
    // CLIs (screenKnown false) liveLines is empty and dialogShowing false.
    Q_INVOKABLE [[nodiscard]] bool dialogShowing(const QStringList& screen) const;
    // When the last turn's prompt was written (ms since the epoch) while that
    // turn is still open, else 0: what "Working for" counts from.
    Q_INVOKABLE [[nodiscard]] double runningSince() const;
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

    [[nodiscard]] const QString& model() const { return reader_->status().model; }
    [[nodiscard]] const QString& effort() const { return reader_->status().effort; }
    [[nodiscard]] qint64 contextTokens() const { return reader_->status().context_tokens; }
    [[nodiscard]] double compactingSince() const {
        return static_cast<double>(reader_->status().compacting_since);
    }
    [[nodiscard]] qint64 contextWindow() const {
        return transcript::context_window(reader_->status());
    }
    [[nodiscard]] transcript::Format format() const { return reader_->format(); }
    // The items read so far and the turns laid out, for tools and tests.
    [[nodiscard]] const std::vector<transcript::Item>& items() const { return reader_->items(); }
    [[nodiscard]] int turnCount() const { return static_cast<int>(layout_.turns.size()); }
    [[nodiscard]] QString formatName() const { return transcript::format_name(format()); }
    [[nodiscard]] bool forkable() const { return format() == transcript::Format::claude; }
    [[nodiscard]] bool screenKnown() const { return format() == transcript::Format::claude; }
    // Reads every later path as `format` whatever its name (for tools that
    // show a file saved elsewhere); nullopt goes back to the name. Applies
    // from the next setPath.
    void setFormat(std::optional<transcript::Format> format) { forced_format_ = format; }

  signals:
    void pathChanged();
    void loadingChanged();
    void countChanged();
    void statusChanged();

  private:
    [[nodiscard]] transcript::Format format_for(const QString& path) const;
    void apply(std::unique_ptr<transcript::Reader> reader, qint64 offset, QByteArray pending);
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
        std::unique_ptr<transcript::Reader> reader;
        qint64 offset{};
        QByteArray pending;
    };
    static constexpr std::size_t kept_conversations = 6;
    std::vector<Parsed> kept_;
    QString path_;
    std::optional<transcript::Format> forced_format_;
    // Never null: an empty Claude Code reader until a path is set.
    std::unique_ptr<transcript::Reader> reader_;
    transcript::Status shown_status_;
    qint64 offset_{};
    QByteArray pending_; // a line the CLI has not finished writing
    bool loading_{};
    quint64 generation_{};
};
} // namespace lapis::desktop
#endif
