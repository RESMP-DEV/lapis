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
class QThreadPool;
class QTimer;

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
    // A prompt the CLI read inside a running turn (it steers that turn),
    // placed where it was read; `waited` counts the steps the agent took
    // between its sending and its reading, or -1 when the CLI does not say.
    bool steered{};
    int waited{-1};
    bool quiet{}; // starts a turn without a row of its own (a background task's wake-up)
    // Ends any turn still open and stays outside turns (a /compact the
    // person ran between turns), so it is never folded into a turn's work.
    bool between_turns{};
    qint64 duration_ms{}; // a turn item: how long the turn took
    // The reader's task (Reader::tasks) this step started, or -1.
    int task{-1};
};

// Work a step left running past its own call: a subagent, a background shell
// command, a monitor. The reader keeps one per launch; later output checks,
// events and the end record attach to it rather than adding rows.
struct Task {
    enum class Type { subagent, background, monitor };
    Type type{Type::subagent};
    // The CLI's id for it: Claude Code's agentId or task id, Codex's thread
    // or process id, agy's conversation or task id. Empty until recorded.
    QString id;
    int item{-1};        // the step that started it
    QString description; // what it is for, or its command
    QString agent_type;  // a subagent's type or role
    QString model;       // a subagent's model, when named
    qint64 started_ms{};
    qint64 seen_ms{}; // its newest sign of life in the conversation (a check, an event)
    qint64 ended_ms{};
    // Empty while no end is recorded; else completed, failed, killed or
    // stopped as the CLI says.
    QString status;
    qint64 expires_ms{};  // a monitor's own expiry, or 0
    int steps{-1};        // tool calls it made, when the CLI says
    qint64 tokens{};      // tokens it used, when the CLI says
    qint64 duration_ms{}; // as the CLI timed it, else 0
    int exit_code{};
    bool exited{};      // exit_code was recorded
    int checks{};       // output checks and events attached to it
    QString last_check; // the newest check's output or event, shortened
    QString report;     // a subagent's final report, shortened
    // Where its output (a background command) or its own conversation (a
    // subagent, when the CLI names it) is written, or empty.
    QString file;
};
// A task with no end recorded counts as running until its last sign of life
// (`alive_ms`: its start, a check or event, or its own file's last write) is
// this old, so one whose end was never written (the CLI quit) does not run
// forever. A monitor also stops at its own expiry.
inline constexpr qint64 task_silence_ms = 60 * 60 * 1000;
[[nodiscard]] bool task_running(const Task& task, qint64 alive_ms, qint64 now_ms);

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

// The thread as shown: each turn folds to its prompt, one "Worked for" row
// standing for its work (thinking, steps, notes between), and its final
// reply. A prompt the CLI read mid-turn (steered) splits the work in parts,
// each folded on its own: prompt, work, steering prompt, work, reply. While
// the agent works, the running turn's last part is one live row (the live
// work box) instead of a "Worked for" row.
struct Part {
    int lead{};  // the prompt it follows: the turn's or a steering one
    int first{}; // its first and last items (first > last when empty)
    int last{};
    int steps{};          // items folded under its "Worked for" row
    qint64 duration_ms{}; // from the CLI and the items' times, or 0
    QString summary;      // what it did, once finished ("ran 4 commands, ...")
};
struct Turn {
    int start{};          // its first item (the prompt)
    int last{};           // its last item
    int final_reply{-1};  // the reply that ends it, or -1
    int steps{};          // items folded under its "Worked for" rows
    qint64 duration_ms{}; // from the CLI, else from the items' times
    bool closed{};        // finished: by its duration record or the next turn
    bool live{};          // still running while the agent works: a live row
    // What the folded work did, as the Codex app sums it up: "ran 4
    // commands, edited 2 files, read 3 files".
    QString summary;
    std::vector<Part> parts; // at least one
};
// The summary for the work items in [first, last], skipping `except`.
[[nodiscard]] QString work_summary(const std::vector<Item>& items, int first, int last, int except);
struct Row {
    int item{-1};   // the item shown, or -1 for a part's "Worked for" or live row
    int turn{-1};   // the turn it belongs to, or -1
    int part{};     // the part of the turn a "Worked for" or live row stands for
    bool in_work{}; // folded work, shown because its part is open
    bool live{};    // the running turn's live work box
};
struct Layout {
    std::vector<Turn> turns;
    std::vector<Row> rows;
};
// `expanded` holds the leads of the parts whose work is shown. `live`: the
// agent is working, so the last turn, while open, ends in a live row.
[[nodiscard]] Layout lay_out(const std::vector<Item>& items, const QSet<int>& expanded,
                             bool live = false);
// The items the live work box lists for the turn `turn` of `layout`: its
// last part's steps, thinking, notes and replies, in order.
[[nodiscard]] std::vector<int> live_items(const std::vector<Item>& items, const Layout& layout,
                                          int turn);

// Limits that keep one huge tool output or file write from costing the view.
inline constexpr int result_preview_lines = 14;
inline constexpr qsizetype result_preview_chars = 4000;
inline constexpr int diff_preview_lines = 40;
inline constexpr qsizetype body_chars = 24000;

// "12s", "3m 12s", "1h 4m".
[[nodiscard]] QString duration_text(qint64 ms);

// Paths in a message to local media the view can show, in order, at most 8:
// Markdown image and link targets, then bare absolute or ~ paths with a media
// extension. Only files that exist count; `skip` holds paths left out (shown
// elsewhere), and `missing`, when given, is set if a path with a media
// extension named no file (yet), so a caller knows the answer may change.
struct Media {
    QString path;
    QString kind; // image, animation, svg, audio, video, html, pdf, file
};
[[nodiscard]] std::vector<Media> find_media(const QString& text, const QSet<QString>& skip = {},
                                            bool* missing = nullptr);

// A reply split at its fenced code blocks: prose stays markdown, and each
// block is its own piece so it can be copied whole. A paragraph that is only
// a path to media that exists (bare, in backticks, or a Markdown image or
// link to it, starting at the line's first column) is a media piece in its
// place, so a picture shows where the reply names it; `text` keeps the
// paragraph as written. `missing` is set as for find_media.
struct Piece {
    bool code{};
    QString language;
    QString text;
    Media media; // set (path not empty) for a media piece
};
[[nodiscard]] std::vector<Piece> pieces(const QString& markdown, bool* missing = nullptr);

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

// The path written around `position` in `text` (absolute, ~, or relative to
// `folder`), resolved, with any ":line[:column]" after it kept, when that
// file or folder exists; else empty.
[[nodiscard]] QString path_at(const QString& text, int position, const QString& folder);

// The live tail: what a running turn shows on Claude Code's screen that the
// thread does not show yet. The thread never draws it (the live work box
// reads the transcript); the view watches it change to tell when a slash
// command with no turn has gone quiet. Claude Code draws, above its input box (the last
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

// Whether the CLI's input box still holds `text` after it was sent: the
// lowest row led by the input box's mark (Claude Code ❯, Codex ›, agy >)
// in the screen's last 16 rows starts with the text's first line (at most
// its first 40 characters, spaces collapsed). A sent prompt the CLI took
// leaves an empty box (or its placeholder) there; earlier copies of the
// text above it, as the CLI shows what was sent, do not count.
enum class Format;
[[nodiscard]] bool composer_holds(const QStringList& screen, const QString& text, Format format);
// What the CLI's input box holds, or empty: the same lowest marked row, then
// the rows under it that continue it (indented, up to a rule, a blank row or
// Codex's hint row), lines kept. A placeholder (Codex's "Ask Codex to do
// anything", "Ask a follow-up question") counts as empty. Heuristic: read
// from text alone, so a CLI's dimmed hint the screen does not mark as faint,
// or a row the CLI draws differently from the fixtures, can be misread.
[[nodiscard]] QString composer_text(const QStringList& screen, Format format);
// Whether Codex shows a steering message waiting ("Messages to be submitted
// after next tool call"), which Escape then sends at once as a new turn.
[[nodiscard]] bool steer_waiting(const QStringList& screen);

// Whether `record` (a prompt as the CLI saved it, or text it gave back)
// holds `sent`: the sent text's non-empty lines, spaces collapsed and @path
// mentions left out, appear in the record's lines as one run in order.
[[nodiscard]] bool holds_prompt(const QString& record, const QString& sent);

// How a CLI takes a message sent while it works (TranscriptModel::queueMode):
// Claude Code queues it and reads it at its next step ("queue"); Codex steers
// the turn with it once the running step ends ("steer"); agy holds it until
// the turn ends ("turn-end", its default) or reads it at its next step
// ("immediate") as its settings' `queuedMessages` says.
[[nodiscard]] QString queue_mode(Format format, bool agy_immediate);
// Whether agy's settings file (settings.json in its home) sets
// `queuedMessages` to "send-immediately"; absent, unreadable or any other
// value is agy's default, Queue.
[[nodiscard]] bool antigravity_sends_immediately(const QString& settings_path);

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
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = default;
    Reader& operator=(Reader&&) = default;
    // Index of the first item changed by this line, or -1 when none was;
    // appended items follow it.
    virtual int add(const QByteArray& line) = 0;
    [[nodiscard]] virtual Format format() const = 0;
    // A copy that reads on from where this one is, so another thread can
    // feed it what the file gained while this one stays shown or kept.
    [[nodiscard]] virtual std::unique_ptr<Reader> clone() const = 0;
    // Whether reading only a file's end can start at this line (read_file):
    // a line whose meaning does not rest on records before it.
    [[nodiscard]] virtual bool starts_reading(const QByteArray& line) const {
        Q_UNUSED(line);
        return true;
    }
    // About how many bytes of memory what was read takes, for the budget of
    // conversations kept parsed.
    [[nodiscard]] qint64 footprint() const;
    [[nodiscard]] const std::vector<Item>& items() const { return items_; }
    [[nodiscard]] const Status& status() const { return status_; }
    [[nodiscard]] const std::vector<Task>& tasks() const { return tasks_; }
    // A prompt the person sent through lapis: it waits, shown at once (as
    // queued when the agent was working), until the CLI records a prompt
    // with its text. Then that prompt and any sent before it leave the list,
    // and a steering prompt learns how many steps it waited. Never written.
    void note_sent(const QString& text, bool queued = true);
    [[nodiscard]] QStringList sent() const;
    // Each prompt in sent() as written (its lines kept), and whether it was
    // sent while the agent worked.
    [[nodiscard]] QStringList sent_as_written() const;
    [[nodiscard]] std::vector<bool> sent_queued() const;
    // Forgets the prompts sent whose text `text` holds (see holds_prompt):
    // they came back unsent (to the CLI's input box, then the bar). Returns
    // how many went.
    int drop_sent(const QString& text);
    // Takes over the prompts `from`, an earlier reading of the same
    // conversation (a tail this whole reading replaces), still waits for,
    // less those this reading has recorded since.
    void carry_sent(const Reader& from);

  protected:
    Reader(const Reader&) = default; // for clone()
    // Matches a prompt the CLI recorded (about to be pushed) against the
    // prompts sent; see note_sent. A record that joins several (agy's queue
    // delivered as one input, Claude Code's queue sent at once) takes every
    // prompt whose lines it holds in a run.
    void taken(Item& prompt);
    // Starts a task for its step (task.item, else the step about to be
    // pushed); returns its index.
    int start_task(Task task);
    // The task the CLI calls `id`, or nullptr.
    Task* task_named(const QString& id);
    // Records its id once the CLI names it.
    void name_task(int task, const QString& id);
    // Records its end: `status` (completed, failed, killed, stopped) at `at_ms`.
    // Returns the step that started it, or -1.
    int end_task(Task& task, const QString& status, qint64 at_ms);
    std::vector<Item> items_;
    Status status_;
    std::vector<Task> tasks_;
    QHash<QString, int> task_ids_; // a task's CLI id -> its index

  private:
    struct Sent {
        QString text; // simplified
        int at{};     // items read when it was sent
        bool queued{};
        QString written;
    };
    std::vector<Sent> sent_;
};
[[nodiscard]] std::unique_ptr<Reader> make_reader(Format format);

// A conversation file read into a reader: `offset` is where reading
// stopped and `pending` a line the CLI has not finished writing.
struct Read {
    std::unique_ptr<Reader> reader;
    qint64 offset{};
    QByteArray pending;
    bool tail{}; // only the file's end was read (see read_file)
};
// Reads the whole file, or with `tail_bytes` >= 0 only about that much of
// its end, from the first line that starts there (a record cut by the
// boundary is skipped), so a thread can show its newest turns before the
// rest is read. A file no longer than `tail_bytes` is read whole.
[[nodiscard]] Read read_file(const QString& path, Format format, qint64 tail_bytes = -1);
// Feeds `bytes` (after `pending`, a line started earlier) to `reader` and
// keeps a trailing unfinished line in `pending`. Returns the first item a
// line changed, or -1.
int feed(Reader& reader, QByteArray& pending, const QByteArray& bytes);
// The end of a conversation file holding at least two turns' starts, so
// its rows (trim_to_turns) are the last rows of the whole file: read from
// `bytes` before the end, four times further back each time it holds fewer,
// up to `most` bytes (or the whole file).
[[nodiscard]] Read read_end(const QString& path, Format format, qint64 bytes, qint64 most);
// A tail's rows from its second turn's prompt on: rows before it are the
// rest of a turn whose start the tail cut off, or a turn whose records
// before its prompt may be cut, which the whole file shows differently. A
// tail with one turn keeps it, and one with none keeps its rows.
void trim_to_turns(Layout& layout);
// Whether row `x` of one reading of a conversation shows what row `y` of
// another (a tail, the whole file) shows: the same record's item with the
// same text and state, or the same "Worked for" or live row. Item indexes
// differ between readings, so they are not compared.
[[nodiscard]] bool same_content(const Layout& a, const std::vector<Item>& a_items, const Row& x,
                                const Layout& b, const std::vector<Item>& b_items, const Row& y);

// Folds Claude Code session JSONL records, in file order, into items. Tool
// results attach to the tool call that asked for them; a subagent's records
// (isSidechain) are skipped unless the file is that subagent's own
// (`sidechain`, <session>/subagents/agent-<id>.jsonl).
// Subagents (Agent, Task), background commands (Bash run_in_background, or a
// command moved to the background when it ran long) and monitors start
// tasks. A <task-notification> ends one (its status, usage and result) or,
// for a monitor's event, counts as a check; reading a background command's
// output (BashOutput, TaskOutput, a Read or command naming its output file)
// and stopping it (KillShell, TaskStop) attach to its task instead of
// showing as steps, and SendMessage resuming a subagent runs it again.
class ClaudeReader final : public Reader {
  public:
    explicit ClaudeReader(bool sidechain = false) : sidechain_(sidechain) {}
    int add(const QByteArray& line) override;
    [[nodiscard]] Format format() const override { return Format::claude; }
    [[nodiscard]] std::unique_ptr<Reader> clone() const override {
        return std::make_unique<ClaudeReader>(*this);
    }

  private:
    void push(Item item);
    // A <task-notification>: ends or checks its task. Returns the step that
    // started it, or -1.
    int notified(const QString& raw);
    // A tool call that checks or stops a task already running: folded into
    // it (quiet) and remembered so its result attaches there.
    void check_call(Item& item, const QString& name, const QJsonObject& input);
    // A launch's or a check's result, from the record's toolUseResult.
    int task_result(int task, const QJsonObject& result, const Item& step);
    bool sidechain_{};
    QHash<QString, int> launches_;  // a launching tool_use id -> its task
    QHash<QString, int> checks_;    // a checking tool_use id -> the task it checks
    QHash<QString, int> outputs_;   // a background command's output file -> its task
    std::vector<int> open_outputs_; // those tasks, while they may still run
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
// 0.157 writes it) into items. Subagents come from its collaboration items
// (CollabAgentToolCall spawn_agent starts one per receiver thread; a later
// call's agents_states or a SubAgentActivity "completed" ends it); a command
// still running when exec_command yields starts a background task that its
// CommandExecution item, written when the process exits, ends. The
// conversation comes from its finished
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
    [[nodiscard]] std::unique_ptr<Reader> clone() const override {
        return std::make_unique<CodexReader>(*this);
    }
    // A turn's task_started: its prompts and steering are read by the turn
    // it opens.
    [[nodiscard]] bool starts_reading(const QByteArray& line) const override {
        return line.contains(R"("type":"task_started")");
    }

  private:
    void push(Item item);
    // One finished item (a payload.item) as zero or more items.
    void item(const QJsonObject& item, qint64 started_ms, qint64 completed_ms);
    // A tool's output in the model-facing copy: Codex's exec_command result
    // for a process still running ({session_id, wall_time_seconds, ...})
    // starts a background task for it, or counts as a check of one.
    int exec_output(const QJsonObject& payload);
    // A CollabAgentToolCall: spawn_agent starts a subagent per receiver;
    // any call's agents_states end those it reports done.
    void collab(const QJsonObject& item, qint64 started_ms);
    int ended_{-1}; // the first earlier step the line being read changed, or -1
    qint64 at_ms_{};
    QString record_;
    QString turn_;                     // the running turn's id, or empty between turns
    bool turn_prompted_{};             // the running turn has its prompt
    QHash<QString, QString> commands_; // a code-mode call's id -> the command it runs, if one
};

// Folds an Antigravity CLI transcript (brain/<conversation>/.system_generated
// /logs/transcript_full.jsonl, as agy 1.5 appends it) into items. Each line is
// a step: the person's input (USER_INPUT), a model response (thinking, text
// and tool calls), each call's result in call order (GENERIC), and the CLI's
// own messages. A turn ends at a response with text and no tool calls; the
// file records no durations, so turns and steps are timed from their times.
// The model and effort come from the settings change agy notes in a prompt.
// invoke_subagent starts a task per subagent it created (its conversation
// id); a message from that conversation, manage_subagents listing it idle or
// killing it ends one. A run_command agy left running ("background task with
// task id") starts a background task; manage_task checks attach to it and
// the "Task id ... finished" system message ends it.
class AntigravityReader final : public Reader {
  public:
    int add(const QByteArray& line) override;
    [[nodiscard]] Format format() const override { return Format::antigravity; }
    [[nodiscard]] std::unique_ptr<Reader> clone() const override {
        return std::make_unique<AntigravityReader>(*this);
    }

  private:
    void push(Item item);
    // A model step after the turn ended (a timer or background task woke
    // the agent) starts a turn of its own, with no row for its start.
    void wake(const QString& why);
    // A tool call as its step, with what it means for subagents and tasks.
    void call(const QJsonObject& call);
    // A tool's result for the step at `at`; returns the first item changed.
    int result(int at, const QJsonObject& record);
    // A system message about a task or from a subagent; returns the step
    // that started it, or -1.
    int system_note(const QString& content);
    qint64 at_ms_{};
    QString record_;
    // Tool items awaiting their result, in call order, with the tool's name.
    std::vector<int> waiting_;
    QHash<int, QString> called_;
    // An invoke_subagent step -> the tasks it started, in the order agy
    // lists the conversations it created; a manage_task step -> the task it
    // checks.
    QHash<int, std::vector<int>> spawned_;
    bool turn_open_{};
    qint64 turn_since_ms_{};
    // When the running turn's last tool step was written, or 0: a person's
    // input soon after one is read mid-turn and steers it.
    qint64 step_at_ms_{};
    QSet<qint64> steps_; // step indexes read, so a step written twice counts once
};
} // namespace transcript

class TranscriptModel;

// The running turn's steps for the live work box: one row per item of its
// last part, with the thread's roles, oldest first. Rows only append while
// the turn runs; anything else resets.
class LiveStepsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    // The thread has a live row (a turn runs while the agent works).
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
  public:
    explicit LiveStepsModel(TranscriptModel* owner);
    [[nodiscard]] bool active() const { return active_; }
    void setActive(bool active) {
        if (active != active_) {
            active_ = active;
            emit activeChanged();
        }
    }
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // The row's roles by name, for the step opened under the box; empty
    // when there is no such row.
    Q_INVOKABLE [[nodiscard]] QVariantMap entry(int row) const;
    // The row showing item `item`, or -1.
    Q_INVOKABLE [[nodiscard]] int rowOf(int item) const;
    // Shows `items`; rows whose item is `changed` or later are redrawn.
    void show(std::vector<int> items, int changed);
    void clear() { show({}, 0); }

  signals:
    void countChanged();
    void activeChanged();

  private:
    TranscriptModel* owner_;
    std::vector<int> items_;
    bool active_{};
};

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
    // (Claude Code's input box); for the others liveLines is empty and the
    // view never peeks at a dialog.
    Q_PROPERTY(bool screenKnown READ screenKnown NOTIFY pathChanged)
    // The agent is working (from its terminal): the running turn ends in
    // the live work box rather than folding.
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged)
    // The last turn started and its CLI has recorded no end yet (its final
    // reply, a turn-end record or a next prompt). A status estimated from
    // output can say working after the record shows the turn over.
    Q_PROPERTY(bool turnOpen READ turnOpen NOTIFY turnOpenChanged)
    // The live work box's steps (a LiveStepsModel).
    Q_PROPERTY(QObject* liveSteps READ liveSteps CONSTANT)
    // Prompts the bar sent that the CLI has not recorded yet, oldest first.
    Q_PROPERTY(QStringList pendingPrompts READ pendingPrompts NOTIFY pendingChanged)
    // The same as {text, queued}: queued when sent while the agent worked.
    Q_PROPERTY(QVariantList pendingEntries READ pendingEntries NOTIFY pendingChanged)
    // How this CLI takes a message sent mid-turn (transcript::queue_mode):
    // "queue", "steer", "turn-end" or "immediate". agy's setting is read
    // again on refreshQueueMode() (the view calls it when a turn starts).
    Q_PROPERTY(QString queueMode READ queueMode NOTIFY queueModeChanged)
    // The subagents and background tasks still running, oldest first, each
    // as the task role describes it plus `item` (the step that started it).
    // Kept current by the file and by tick().
    Q_PROPERTY(QVariantList running READ running NOTIFY runningChanged)
    // When the last turn ended (ms since the epoch) while work it started
    // still runs, else 0: what "Waiting on 2 subagents" counts from.
    Q_PROPERTY(double waitingSince READ waitingSince NOTIFY runningChanged)
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
        SteeredRole,  // a prompt the CLI read mid-turn
        WaitedRole,   // steps it waited before being read, or -1
        ItemRole,     // the item's index, or -1 for a "Worked for" or live row
        // The task a step started (a subagent, a background command): {type
        // (subagent, background, monitor), description, agentType, model,
        // since, ended, running, stale (no end recorded, too long silent),
        // status, duration, steps, tokens, exit (-1 if none), checks,
        // lastCheck, report}; null for other items.
        TaskRole,
    };
    // An item's value for a role (not the layout's roles: expanded, inWork,
    // steps), shared by the thread and the live work box.
    [[nodiscard]] QVariant item_data(int index, int role) const;
    // The same for any item, the task role aside (null).
    [[nodiscard]] static QVariant item_value(const transcript::Item& item, int index, int role);
    // An item's roles by name, as LiveStepsModel::entry gives them.
    Q_INVOKABLE [[nodiscard]] QVariantMap itemEntry(int item) const;
    // What an opened task step shows beyond its line, read when it is opened
    // (and again while it runs): a subagent's own steps as entries (`steps`,
    // each with the thread's roles, its own subagents one line each) and
    // its final report (`report`); a background command's last lines of
    // output (`output`). Empty where the CLI saved nothing to read.
    Q_INVOKABLE [[nodiscard]] QVariantMap taskDetail(int item);
    // Brings `running` up to date with the clock and the tasks' own files.
    Q_INVOKABLE void tick();
    // The clock tasks are judged by, for tests: 0 is the system's.
    void setNow(qint64 now_ms) { now_ms_ = now_ms; }
    [[nodiscard]] const QVariantList& running() const { return running_; }
    [[nodiscard]] double waitingSince() const { return waiting_since_; }
    [[nodiscard]] const std::vector<transcript::Task>& tasks() const { return reader_->tasks(); }
    [[nodiscard]] static QHash<int, QByteArray> role_names();
    explicit TranscriptModel(QObject* parent = nullptr);
    // Readings still in flight on other threads are dropped, never handed
    // to a model being destroyed.
    ~TranscriptModel() override;
    TranscriptModel(const TranscriptModel&) = delete;
    TranscriptModel& operator=(const TranscriptModel&) = delete;
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
    // A reply's prose, code and media pieces: [{code, language, text,
    // media}], media being null or a map as media() gives.
    Q_INVOKABLE [[nodiscard]] QVariantList pieces(const QString& markdown) const;
    // The media a reply names that its pieces do not already show in place,
    // for the list under it (as media()).
    Q_INVOKABLE [[nodiscard]] QVariantList replyMedia(const QString& markdown) const;
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
    // HTML and PDF that exist. Each {path, url, kind, name, type, width,
    // height, alpha, size}; kind is image, animation (GIF), svg, audio,
    // video, html or pdf; alpha marks a picture that can be see-through.
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
    // Whether this CLI's input box on the screen still holds `text` (see
    // transcript::composer_holds): a prompt sent but not taken.
    Q_INVOKABLE [[nodiscard]] bool composerHolds(const QStringList& screen,
                                                 const QString& text) const;
    // What this CLI's input box holds (transcript::composer_text), or empty.
    Q_INVOKABLE [[nodiscard]] QString composerText(const QStringList& screen) const;
    // Whether Codex shows a steering message waiting (transcript::steer_waiting).
    Q_INVOKABLE [[nodiscard]] bool steerWaiting(const QStringList& screen) const;
    // Reads agy's queued-message setting again; see queueMode.
    Q_INVOKABLE void refreshQueueMode();
    [[nodiscard]] const QString& queueMode() const { return queue_mode_; }
    // Forgets the prompts sent that `text` holds: the CLI gave them back
    // unsent and they are in the bar again (Reader::drop_sent).
    Q_INVOKABLE void forgetSent(const QString& text);
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
    // A prompt the bar sent (see pendingPrompts), shown at once until the
    // CLI records it; `queued` when the agent was working.
    Q_INVOKABLE void noteSent(const QString& text, bool queued = true);
    [[nodiscard]] bool live() const { return live_; }
    void setLive(bool live);
    [[nodiscard]] bool turnOpen() const { return turn_open_; }
    [[nodiscard]] QObject* liveSteps() { return &live_steps_; }
    [[nodiscard]] const LiveStepsModel& liveStepsModel() const { return live_steps_; }
    [[nodiscard]] QStringList pendingPrompts() const { return reader_->sent(); }
    [[nodiscard]] QVariantList pendingEntries() const;
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

    // Switching between agents. Each conversation read stays parsed (kept)
    // while it fits the budget, least recently shown going first, so showing
    // it again is at once and reads only what its CLI wrote since; the shown
    // one is never let go. A conversation not kept shows its end (the last
    // tail_bytes, read at once) and the rest is read on another thread and
    // put in above it. See docs/architecture.md (visual view loading).
    static constexpr qint64 tail_bytes = qint64{1} << 20;
    static constexpr qint64 default_kept_mb = 1024;
    static constexpr int default_kept_count = 32;
    // The budget for conversations kept parsed besides the shown one: about
    // `bytes` of memory and `count` conversations.
    void setBudget(qint64 bytes, int count);
    [[nodiscard]] qint64 keptBytes() const;
    [[nodiscard]] int keptCount() const { return static_cast<int>(kept_.size()); }
    [[nodiscard]] bool kept(const QString& path) const;
    // Reads these conversations ahead on a low-priority thread, one at a
    // time and in this order (most recently active first), while the budget
    // has room; ones already kept or being read are skipped. Kept ones are
    // then brought up to date in the background when their files grow.
    void warm(const QStringList& paths);
    // Where the view was in the shown conversation when another replaced
    // it (the view calls this on leaving()): the row at its top and how far
    // into that row, or following its end.
    Q_INVOKABLE void keepView(int row, double offset, bool following);
    // The view to restore for the conversation just shown (pathChanged) or
    // read again (reloaded): {following} or {following: false, row, offset}.
    Q_INVOKABLE [[nodiscard]] QVariantMap view() const;
    // Only the file's end is shown yet (the rest is being read).
    [[nodiscard]] bool tailShown() const { return tail_; }
    // Brings one kept conversation up to date now, as the timer does, and
    // whether a read ahead or such a catch-up is running; for tests.
    void followKept() { catchUp(); }
    [[nodiscard]] bool readingAhead() const { return warming_; }

  signals:
    // The rows are about to be replaced, by another conversation or by a
    // longer reading of this one: the view keeps its place (keepView) now
    // and makes no rows until pathChanged or reloaded.
    void leaving();
    // The shown conversation's rows were replaced by a longer reading of it
    // (its history came in above its end); the view goes back to view().
    void reloaded();
    void pathChanged();
    void loadingChanged();
    void countChanged();
    void statusChanged();
    void liveChanged();
    void turnOpenChanged();
    void pendingChanged();
    void runningChanged();
    void queueModeChanged();

  private:
    QString queue_mode_{QStringLiteral("queue")};
    // A task's own file, as this conversation's CLI keeps it (a Claude Code
    // subagent's transcript beside this one, a Codex child rollout, an agy
    // child conversation, a background command's output), or empty.
    [[nodiscard]] QString task_file(const transcript::Task& task) const;
    // A task file read so far: its reader, fed what the file gained.
    struct Child {
        QString path;
        std::unique_ptr<transcript::Reader> reader;
        qint64 offset{};
        QByteArray pending;
        qint64 modified_ms{};
    };
    // The child for `path` (a subagent's own conversation) read up to date;
    // nullptr when the file is missing.
    Child* child(const QString& path, transcript::Format format);
    std::vector<Child> children_; // most recent first, a few kept
    // A Claude Code subagent's tool calls so far, counted from its file's
    // bytes as it grows (cheaper than reading it); -1 when it is missing.
    int counted_steps(const QString& path);
    struct Count {
        qint64 offset{};
        int steps{};
        QByteArray pending;
    };
    QHash<QString, Count> counts_;
    [[nodiscard]] qint64 now() const;
    // The task's newest sign of life, its own file's last write included.
    [[nodiscard]] qint64 alive_ms(const transcript::Task& task) const;
    [[nodiscard]] bool task_running(const transcript::Task& task) const;
    [[nodiscard]] QVariantMap task_map(const transcript::Task& task) const;
    void updateRunning();
    // What a running subagent's own file says, by task index: its steps so
    // far, and whether it came to rest (its last turn ended).
    struct LiveTask {
        int steps{-1};
        bool rested{};
    };
    QHash<int, LiveTask> live_tasks_;
    QVariantList running_;
    double waiting_since_{};
    qint64 now_ms_{};
    // Task files found by search (Codex child rollouts), by task id.
    mutable QHash<QString, QString> found_files_;
    // Shows the live turn's items in the live work box, or none.
    void showLive(int changed);
    void notePending();
    bool live_{};
    bool turn_open_{};
    LiveStepsModel live_steps_{this};
    QStringList shown_pending_;
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
        QSet<int> expanded;
        QVariantMap view;
        qint64 bytes{};  // the reader's footprint when kept
        quint64 shown{}; // when it was last shown (shown_clock_), 0 if never
    };
    // Most recently shown first; read ahead and never shown last.
    std::vector<Parsed> kept_;
    void keep(Parsed parsed);
    void evict();
    qint64 kept_budget_{default_kept_mb << 20};
    int kept_limit_{default_kept_count};
    quint64 shown_clock_{};
    // A whole reading (or a longer tail) of `path` from another thread.
    void received(const QString& path, transcript::Read read, bool ahead);
    // A kept conversation's reader brought up to date from `base` on.
    void caughtUp(const QString& path, qint64 base, transcript::Read read);
    // Shows another reading of the shown conversation in place of this
    // one, changing only the rows that differ (the history above a tail).
    void adopt(transcript::Read read);
    void startLoad(const QString& path, transcript::Format format);
    void warmNext();
    void catchUp();
    // What the reading threads hand back (defined in transcript.cpp). They
    // touch only the mailbox, never the model, and the model takes the
    // readings out on its own thread (drain).
    struct Mailbox;
    std::shared_ptr<Mailbox> mailbox_;
    void drain();
    [[nodiscard]] transcript::Layout layoutNow() const;
    QSet<QString> loads_;       // conversations a thread is reading
    QStringList warm_queue_;    // to read ahead, in order
    bool warming_{};            // a read-ahead or catch-up is running
    bool tail_{};               // reader_ holds only the file's end
    QVariantMap view_;          // the shown conversation's (keepView)
    QThreadPool* background_{}; // one low-priority thread for read-ahead
    QTimer* catch_up_timer_{};
    QString path_;
    std::optional<transcript::Format> forced_format_;
    // Never null: an empty Claude Code reader until a path is set.
    std::unique_ptr<transcript::Reader> reader_;
    transcript::Status shown_status_;
    qint64 offset_{};
    QByteArray pending_; // a line the CLI has not finished writing
    bool loading_{};
    quint64 generation_{};
    // A reply's pieces and the media under it, worked out (with their disk
    // checks and picture headers) once per text rather than each time its
    // row is made while scrolling. An answer that named a missing file is
    // not kept, so the file is looked for again when the row is next made.
    struct ReplyParts {
        QVariantList pieces;
        QVariantList media;
    };
    [[nodiscard]] const ReplyParts& reply_parts(const QString& markdown) const;
    static constexpr qsizetype kept_replies = 256;
    mutable QHash<QString, ReplyParts> replies_;
    mutable ReplyParts unsettled_;
};
} // namespace lapis::desktop
#endif
