#ifndef LAPIS_DESKTOP_WORKSPACE_HPP
#define LAPIS_DESKTOP_WORKSPACE_HPP

#include "config.hpp"
#include "conversation_owner.hpp"
#include "factory_state.hpp"
#include "harness_catalog.hpp"
#include "harness_models.hpp"
#include "history_strip.hpp"
#include "tab_ranker.hpp"
#include <lapis/session/terminal.hpp>

#include "launch_spec.hpp"
#include "transport/attention_protocol.hpp"
#include "transport/local_protocol.hpp"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QFileSystemWatcher;
class QProcess;

namespace lapis::desktop {

class LiveConnection;

class Agent final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString agentId READ agentId CONSTANT)
    Q_PROPERTY(bool attentionPending READ attentionPending NOTIFY attentionChanged)
    Q_PROPERTY(QString attentionReason READ attentionReason NOTIFY attentionChanged)
    Q_PROPERTY(quint32 attentionSerial READ attentionSerial NOTIFY attentionChanged)
    Q_PROPERTY(int attentionCount READ attentionCount NOTIFY attentionChanged)
    Q_PROPERTY(bool hasAttentionSource READ hasAttentionSource NOTIFY attentionChanged)
    Q_PROPERTY(bool attentionReady READ attentionReady NOTIFY attentionChanged)
    Q_PROPERTY(QString attentionDiagnostic READ attentionDiagnostic NOTIFY attentionChanged)
    Q_PROPERTY(QVariantList attentionRequests READ attentionRequests NOTIFY attentionChanged)
    Q_PROPERTY(QString title READ title NOTIFY identityChanged)
    Q_PROPERTY(QString statusLabel READ statusLabel NOTIFY statusChanged)
    Q_PROPERTY(QString statusKind READ statusKind NOTIFY statusChanged)
    Q_PROPERTY(QString agentName READ agentName NOTIFY identityChanged)
    Q_PROPERTY(QString harnessId READ harnessId NOTIFY identityChanged)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QString activity READ activity NOTIFY snapshotChanged)
    Q_PROPERTY(bool live READ live CONSTANT)
    Q_PROPERTY(bool inputReady READ inputReady NOTIFY connectionChanged)
    Q_PROPERTY(bool reachable READ reachable NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionChanged)
    Q_PROPERTY(QString serviceSessionId READ serviceSessionId NOTIFY connectionChanged)
    Q_PROPERTY(QVariantMap snapshotTiming READ snapshotTiming NOTIFY snapshotChanged)
    Q_PROPERTY(bool historyActive READ historyActive NOTIFY historyChanged)
    Q_PROPERTY(bool historyRequestPending READ historyRequestPending NOTIFY historyChanged)
    Q_PROPERTY(QString historyMessage READ historyMessage NOTIFY historyChanged)
    // Where the shown history page sits in everything kept, from 0 (the
    // oldest row) to 1, and the share it covers; the scrubber's thumb. Only
    // services that place their pages can jump (historyScrubbable).
    Q_PROPERTY(bool historyScrubbable READ historyScrubbable NOTIFY historyChanged)
    Q_PROPERTY(qreal historyPosition READ historyPosition NOTIFY historyChanged)
    Q_PROPERTY(qreal historySpan READ historySpan NOTIFY historyChanged)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    // True while a close request waits for the process to end; cleared if the
    // service rejects the close and the agent remains attached.
    Q_PROPERTY(bool closing READ closing NOTIFY statusChanged)
    // Finished a turn or started needing a response while the person was not
    // looking at it; cleared for good once they look (Workspace::looking).
    // Bookkeeping for Tab, the chime and agent_state.json; the window does not
    // draw it.
    Q_PROPERTY(bool unseen READ unseen NOTIFY unseenChanged)
  public:
    Agent(QString title, QString directory, QString activity, QColor accent,
          std::string_view content);
    ~Agent() override;
    void startLive(const QString& endpoint, const session::LaunchSpec& launch,
                   session::wire::AttachMode mode = session::wire::AttachMode::reconnect);
    // What a session service this card starts adds to its environment (OMP's
    // overlay and hook nonce). Kept in memory only.
    void setServiceEnvironment(QHash<QString, QString> environment) {
        service_environment_ = std::move(environment);
    }
    [[nodiscard]] const QHash<QString, QString>& serviceEnvironment() const {
        return service_environment_;
    }
    // Attach again to its service, taking it from another view (Reconnect
    // agent in Commands); discoverSession adopts a matching running one, for
    // the side terminal's recovery.
    Q_INVOKABLE void reconnect();
    void discoverSession();
    // A screen back or forward through kept history.
    Q_INVOKABLE void olderHistory();
    Q_INVOKABLE void newerHistory();
    // Scrolls kept history by rows, positive back; scrolling forward past the
    // newest row returns to live.
    Q_INVOKABLE void scrollHistory(int rows);
    Q_INVOKABLE void returnToLive();
    // Moves the view to `fraction` of everything kept (0 the oldest row).
    Q_INVOKABLE void historyAt(qreal fraction);
    // Ask the session service to end this agent's process. Returns false when
    // the request could not be queued on a synchronized connection.
    bool terminate();
    [[nodiscard]] bool closing() const { return closing_; }
    void setClosing(bool closing);
    // Shown instead of the status while the agent's CLI updates before start.
    void setUpdating(const QString& label);
    [[nodiscard]] bool updating() const { return !updating_.isEmpty(); }
    [[nodiscard]] bool unseen() const { return unseen_; }
    [[nodiscard]] bool restoredUnseen() const { return restored_unseen_; }
    void setUnseen(bool unseen);
    // A mark an earlier window saved: unseen or not, and when it began to
    // need you. Restoring emits unseenChanged and nothing else.
    void restoreUnseen(bool unseen, qint64 neededAtMs);
    // When it last began to need you (ms since the epoch); 0 before that.
    [[nodiscard]] qint64 neededAtMs() const { return needed_at_ms_; }
    // Where activity comes from: a service-side observer (Claude Code's hook
    // relay) or, for other CLIs, an output-timing estimate. Codex agents and
    // agents on other machines start from the estimate; their notify program
    // or hooks relayed through the terminal then report what they observe
    // (see estimated()).
    enum class StatusSource : std::uint8_t { observer, output };
    void setStatusSource(StatusSource source) { status_source_ = source; }
    [[nodiscard]] StatusSource statusSource() const { return status_source_; }
    // Output estimate windows: ignore replays for `settle_ms` after attaching,
    // treat three frames within `burst_ms` as activity, and `quiet_ms` of
    // silence after it as a pause. Tests shorten them.
    struct OutputTiming {
        qint64 settle_ms{3000};
        qint64 burst_ms{1500};
        int quiet_ms{4000};
    };
    void setOutputTimingForTesting(const OutputTiming& timing) { output_timing_ = timing; }
    void applyAttention(session::wire::AttentionSnapshot snapshot);
    void invalidateAttention();
    void retryAttention(const session::wire::AttentionDecision& decision);
    [[nodiscard]] bool hasAttentionSource() const;
    [[nodiscard]] bool attentionReady() const;
    [[nodiscard]] QString attentionDiagnostic() const;
    [[nodiscard]] QVariantList attentionRequests() const;
    void setConnection(const QString& state, bool input_ready);
    void setServiceIdentity(const QByteArray& identity);
    [[nodiscard]] bool inputReady() const {
        return input_ready_ && !history_active_ && !history_request_pending_;
    }
    // Synchronized with its service, even while a history page is showing.
    [[nodiscard]] bool reachable() const { return input_ready_; }
    [[nodiscard]] const QString& connectionState() const { return connection_state_; }
    [[nodiscard]] const QString& serviceSessionId() const { return service_session_id_; }
    void applySnapshot(session::TerminalSnapshot snapshot);
    // A screen from the service, still encoded. Decoding a whole screen takes
    // most of a millisecond, so it is decoded only for the views showing this
    // agent, at the pace they draw (the stage at once, a preview card every
    // 250 ms); otherwise only the newest is kept until a view or a reader asks.
    void offerSnapshot(QByteArray encoded);
    // A view showing this agent's screen, redrawn every `interval_ms` (0 at
    // once). A hidden or closed window's views are not showing anything.
    void addViewer(int interval_ms);
    void removeViewer(int interval_ms);
    // Screens decoded so far, for tests.
    [[nodiscard]] quint64 decodedScreens() const { return decoded_screens_; }
    // Decode attempts so far, failed or not, for tests.
    [[nodiscard]] quint64 decodeAttempts() const { return decode_attempts_; }
    void setSnapshotTiming(const QVariantMap& timing) { snapshot_timing_ = timing; }
    [[nodiscard]] QVariantMap snapshotTiming() const { return snapshot_timing_; }
    void beginHistoryRequest();
    void captureHistoryScreen();
    void completeHistoryRequest(quint64 page_id, session::TerminalSnapshot snapshot,
                                const QString& message);
    void failHistoryRequest(const QString& message);
    void cancelHistoryRequests();
    void setHistoryRequestId(quint64 request_id);
    void setActivity(const QString& activity);
    // False when nothing was sent: no live connection, history showing, or
    // the input queue full.
    bool sendText(const QByteArray& bytes, bool paste = false);
    // Requires service admission; pasteResult reports the eventual outcome.
    quint64 sendPasteAndSubmit(const QByteArray& bytes);
    quint64 requestPaste(const QByteArray& bytes, bool submit = false);
    bool sendKey(session::TerminalKey key, session::KeyModifiers modifiers);
    // A turn of the wheel for the program on the alternate screen, over a
    // viewport cell; only when its snapshot says the service accepts wheels.
    // False means the clamped wheel was not queued.
    bool sendWheel(int steps, int column, int row);
    // Programs that do not report the mouse scroll with arrow keys: another
    // three lines a step, counted so typing returns it to the bottom too.
    // Unlike the wheel path, key delivery has no coordinates.
    bool sendKeyScroll(int steps);
    // The same in arrow-key lines, so a partially delivered scroll is counted
    // exactly and a retry sends only the lines still owed.
    bool sendKeyLines(int lines);
    // Sends queued scroll-back debt forward in bounded wheel messages.
    void returnProgramToBottom();
    void resizeTerminal(session::TerminalSize size);
    // Someone is at this window: take the size back from another device.
    void claimTerminalSize();
    [[nodiscard]] QString statusLabel() const;
    [[nodiscard]] QString statusKind() const;
    [[nodiscard]] QString agentName() const;
    [[nodiscard]] const QString& harnessId() const { return harness_id_; }
    void setHarnessId(const QString& id);
    void rename(const QString& title) {
        title_ = title;
        emit identityChanged();
    }
    void setAgentId(const QString& id) { agent_id_ = id; }
    [[nodiscard]] const QString& agentId() const { return agent_id_; }
    [[nodiscard]] bool attentionPending() const { return attentionCount() != 0; }
    [[nodiscard]] QString attentionReason() const;
    [[nodiscard]] quint32 attentionSerial() const { return attention_serial_; }
    [[nodiscard]] int attentionCount() const;
    // The source ids of the requests pending now ("n" + number or "s" + hex),
    // stable across a reconnect, so a request already marked is not again.
    [[nodiscard]] QStringList attentionRequestIds() const;
    bool addFixtureRequest(const QString& id, const QString& reason);
    bool resolveFixtureRequest(const QString& id);
    void clearFixtureRequests();
    [[nodiscard]] bool liveSnapshotReady() const { return live_snapshot_ready_; }
    [[nodiscard]] bool historyActive() const { return history_active_; }
    [[nodiscard]] bool historyRequestPending() const { return history_request_pending_; }
    [[nodiscard]] const QString& historyMessage() const { return history_message_; }
    [[nodiscard]] bool historyScrubbable() const { return history_scrubbable_; }
    [[nodiscard]] qreal historyPosition() const;
    [[nodiscard]] qreal historySpan() const;
    [[nodiscard]] bool live() const { return live_ != nullptr; }
    [[nodiscard]] const QString& title() const { return title_; }
    [[nodiscard]] const QString& directory() const { return directory_; }
    // The folder on this Mac that paths the agent prints are relative to;
    // empty for an agent over ssh, whose paths are on another machine.
    [[nodiscard]] const QString& linkFolder() const { return link_folder_; }
    [[nodiscard]] const QString& activity() const { return activity_; }
    [[nodiscard]] QColor accent() const { return accent_; }
    // The screen shown: a history page, else the newest live screen, decoded
    // now if it waited.
    [[nodiscard]] const session::TerminalSnapshot& snapshot() const {
        decodeWaiting();
        return snapshot_;
    }
    // While browsing history, the row just above the history page shown, for
    // a view scrolled part of a row; none when live, at the oldest row, or
    // while the page shown waits for the strip to catch up.
    [[nodiscard]] std::optional<session::TerminalSnapshot> historyRowAbove() const;

  signals:
    void identityChanged();
    void statusChanged();
    void connectionChanged();
    void snapshotChanged();
    void historyChanged();
    void attentionChanged();
    void attentionArrived();
    void unseenChanged();
    void pasteResult(quint64 requestId, bool queued, bool submit, const QString& message);

  private:
    std::unique_ptr<LiveConnection> live_;
    QString harness_id_{QStringLiteral("codex")};
    QString agent_id_;
    QMap<QString, QString> requests_;
    std::optional<session::wire::AttentionSnapshot> attention_;
    QSet<QString> submitted_attention_;
    QHash<QString, QString> attention_token_aliases_;
    quint32 attention_serial_{};
    bool awaiting_first_prompt_{};
    bool live_snapshot_ready_{};
    bool live_snapshot_received_{};
    bool closing_{};
    QString updating_;
    bool unseen_{};
    bool restored_unseen_{};
    qint64 needed_at_ms_{};
    StatusSource status_source_{StatusSource::observer};
    // Output estimate: several frames close together read as activity, and a
    // few quiet seconds after that as a pause. Neither implies a finished task.
    void noteOutput();
    [[nodiscard]] QString unobservedStatusKind() const;
    // Whether an output-estimated agent reads from that estimate now: no
    // observer, one not synchronized, or one that does not know the activity
    // (Codex on another machine reports only finished turns).
    [[nodiscard]] bool estimated() const;
    // Shows the strip's view once its pages are here, fetching the next one
    // it lacks.
    void showStrip();
    // Moves past the strip's oldest row: a service that does not place its
    // pages has older ones to put on top.
    void extendStrip();
    // Decodes the screen that waited, if any; true when there was one.
    bool decodeWaiting() const;
    // Milliseconds until the next decode attempt is allowed under the
    // fastest viewer's interval; the single rate-limit rule shared by the
    // offer path and the decode timer.
    [[nodiscard]] qint64 remainingDecodeDelay() const;
    std::vector<qint64> output_times_;
    bool output_active_{};
    bool output_quiet_{};
    qint64 ready_since_{};
    OutputTiming output_timing_;
    QTimer quiet_timer_;
    bool history_active_{};
    bool history_request_pending_{};
    bool history_scrubbable_{};
    // Kept history as one scrolling strip, from services that place their
    // pages; the rows to scroll back once the first page arrives; the oldest
    // page ID fetched, for a service that does not place its pages.
    std::optional<HistoryStrip> strip_;
    // Full-screen scroll-back steps the service accepted, and the cell where
    // they were delivered. This belongs to the agent so a rebound surface
    // cannot lose it; rejected wheels leave the previous debt intact. The
    // const decoder updates it when the newest screen ends the program that
    // owed the debt.
    mutable int program_wheel_debt_{};
    mutable QPoint program_wheel_cell_{-1, -1};
    // The same for a full-screen program that scrolls with arrow keys, in
    // arrow-key lines: one key is one line, and each key is counted the moment
    // the ordered connection accepts it, so a queue that fills mid-scroll owes
    // exactly the lines that never went out.
    mutable int program_key_debt_lines_{};
    // The screen when browsing was asked for: the archive answering is at
    // least as new, so it can repeat rows the screen shows but never miss one.
    std::optional<session::TerminalSnapshot> strip_screen_;
    int strip_rows_asked_{};
    quint64 strip_oldest_page_{};
    bool strip_extending_{};
    bool input_ready_{};
    QString connection_state_{QStringLiteral("disconnected")};
    QString service_session_id_;
    QString history_message_;
    QVariantMap snapshot_timing_;
    QString title_;
    QString directory_;
    QString link_folder_;
    QString activity_;
    QHash<QString, QString> service_environment_;
    QColor accent_;
    // Caches of what the service sent: decoding a waiting screen fills them,
    // so reading the screen is const.
    mutable session::TerminalSnapshot live_snapshot_;
    mutable session::TerminalSnapshot snapshot_;
    mutable std::optional<QByteArray> waiting_;
    mutable quint64 decoded_screens_{};
    mutable quint64 decode_attempts_{};
    std::multiset<int> viewers_;
    mutable QElapsedTimer last_decode_attempt_;
    QTimer decode_timer_;
};

enum class WorkspaceMode : std::uint8_t { live, fixture };

struct FixtureRequest {
    QString agent_id;
    QString request_id;
    QString reason;
};

// An agent to start: a CLI in a folder, in a category, on this Mac or over ssh.
struct AgentRequest {
    QString category;
    QString directory; // on `machine`; ~ is that machine's home
    QString title;
    QString harness;
    QString machine; // an ssh host; empty for this Mac
    QString program; // the CLI's path on `machine`, when known
    QString model;   // passed with the CLI's model flag; empty for its default
    QString mode;    // ask, edits, plan, auto or full; empty for the CLI's own setting
    QString effort;  // low, medium, high, xhigh or max; empty for the CLI's own setting
    // An explicit form or wire title is someone's choice, even when it happens
    // to match the folder. A wire request that omits the title still gets the
    // generated folder default and can follow the conversation.
    bool named{};
    // Show it on the stage; otherwise the category's selection stays.
    bool select{};
    // A conversation to resume, as the CLI's resume option takes it.
    QString resume;
    // Text left in a Claude Code agent's prompt, unsent (its --prefill).
    QString prefill;
};

// The approval mode a new agent starts in. A modeless request uses fallback
// (newAgent.mode), or Full access if absent. Unsupported preferences may use
// a lower mode, never a higher one; unknown preferences add no mode flags.
// Explicit request modes and configured approval options retain precedence.
// Claude Code 2.1.283-2.1.285 changed its no-mode behavior across entry points.
[[nodiscard]] QString launch_mode(const AgentRequest& request, const QStringList& configured,
                                  const QString& fallback);

struct WorkspaceOptions {
    QString endpoint;
    std::optional<session::LaunchSpec> launch;
    session::wire::AttachMode mode{session::wire::AttachMode::reconnect};
    QString storagePath{};
    // Restart agents whose session service is gone (after a reboot or crash),
    // resuming each recorded conversation. Off unless the app asks for it.
    bool restoreAgents{};
    // Run a supported CLI's own update command before a new agent of it
    // starts, at most every 30 minutes per CLI, so agents never open on an
    // update prompt. Existing-session reconnect and discovery do not update.
    bool updateHarnesses{};
    // CLIs pinned in lapis.json are known before constructor-started agents.
    QSet<QString> harnessUpdatesOff{};
    // Production bounds. Tests inject short values so stuck-updater cleanup is
    // observable without waiting two minutes or leaving installer children.
    qint64 updateTimeoutMs{qint64{2} * 60 * 1000};
    // No window: leave running services for a window to reattach, and hand the
    // workspace to a window that opens (the login helper and the windowless
    // host that serves the phone).
    bool headless{};
};

class Workspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList agents READ agents NOTIFY agentsChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(QVariantList categoryAgents READ categoryAgents NOTIFY agentsChanged)
    Q_PROPERTY(QString activeCategoryId READ activeCategoryId NOTIFY categoryChanged)
    Q_PROPERTY(QString workspaceError READ workspaceError NOTIFY errorChanged)
    Q_PROPERTY(bool fixtureMode READ fixtureMode CONSTANT)
    Q_PROPERTY(QString homeDirectory READ homeDirectory CONSTANT)
    Q_PROPERTY(int focusedIndex READ focusedIndex NOTIFY focusChanged)
    Q_PROPERTY(lapis::desktop::Agent* focusedAgent READ focusedAgent NOTIFY focusChanged)
    // An agent closed since lapis opened can come back with its conversation.
    Q_PROPERTY(bool canReopenAgent READ canReopenAgent NOTIFY closedChanged)
    // The visual view's conversation: the stage sets its path to an agent's
    // conversationFile() while that agent shows as a thread.
    Q_PROPERTY(QObject* transcript READ transcript CONSTANT)
    // Factory view (workspace_factory.cpp): one orchestrator conversation and
    // every other agent in four pipeline stages.
    Q_PROPERTY(QString homeAgentId READ homeAgentId NOTIFY homeAgentChanged)
    Q_PROPERTY(
        lapis::desktop::Agent* effectiveHomeAgent READ effectiveHomeAgent NOTIFY homeAgentChanged)
    Q_PROPERTY(QVariantList attentionInbox READ attentionInbox NOTIFY attentionInboxChanged)
    Q_PROPERTY(QVariantList factoryPipeline READ factoryPipeline NOTIFY factoryPipelineChanged)
    Q_PROPERTY(QVariantMap factorySummary READ factorySummary NOTIFY factoryPipelineChanged)
  public:
    explicit Workspace(WorkspaceMode mode = WorkspaceMode::live, WorkspaceOptions options = {});
    ~Workspace() override;
    [[nodiscard]] bool fixtureMode() const { return fixture_mode_; }
    [[nodiscard]] QString homeDirectory() const;
    Q_INVOKABLE [[nodiscard]] QVariantList availableHarnesses() const;
    [[nodiscard]] std::vector<ModelChoice> modelChoices(const QString& harness) const;
    Q_INVOKABLE [[nodiscard]] QString displayPath(const QString& directory) const;
    [[nodiscard]] Agent* agent(const QString& id) const;
    // Development fixture v1 only. No calls are accepted in a live workspace.
    bool requestAttention(const FixtureRequest& request);
    bool resolveAttention(const FixtureRequest& request);
    Q_INVOKABLE bool replayAttention(const QString& scenario);
    [[nodiscard]] QVariantList categories() const;
    [[nodiscard]] QVariantList categoryAgents() const;
    [[nodiscard]] const QString& activeCategoryId() const { return active_category_; }
    [[nodiscard]] const QString& workspaceError() const { return error_; }
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool addCategory(const QString& name);
    // A new category's id, or empty with the reason in workspaceError.
    QString createCategory(const QString& name);
    Q_INVOKABLE bool renameCategory(const QString& id, const QString& name);
    Q_INVOKABLE bool removeCategory(const QString& id);
    // One of the palette's names (category_colors.hpp), or empty to return
    // to the colour its position gives it.
    Q_INVOKABLE bool setCategoryColor(const QString& id, const QString& color);
    Q_INVOKABLE bool selectCategory(const QString& id);
    Q_INVOKABLE void nextCategory(int delta = 1);
    Q_INVOKABLE bool selectAgent(const QString& id);
    // `machine` is an ssh host; empty starts it on this Mac.
    Q_INVOKABLE bool createAgent(const QString& directory, const QString& title,
                                 const QString& harness = QStringLiteral("codex"),
                                 const QString& model = {}, const QString& mode = {},
                                 const QString& machine = {}, bool named = true);
    // The machines a new agent can start on besides this Mac: the ssh
    // config's hosts, as the side terminal offers them.
    Q_INVOKABLE [[nodiscard]] QStringList sshMachines() const;
    void setSshConfigForTesting(const QString& path) { ssh_config_ = path; }
    // The stage's terminal grid (columns by rows). A CLI starts at this size
    // rather than being resized just after it drew, which the classic
    // renderers of Claude Code and others cannot redraw from.
    Q_INVOKABLE void setLaunchSize(QSize size);
    // Tests reconnect in milliseconds: a first connection counts once it held
    // `first_hold`, and each wait before reconnecting is `wait`.
    struct ReconnectTiming {
        std::chrono::milliseconds first_hold;
        std::chrono::milliseconds wait;
    };
    void setReconnectTimingForTesting(ReconnectTiming timing) {
        reconnect_first_hold_ = timing.first_hold;
        reconnect_wait_ = timing.wait;
    }
    // Starts an agent in the active category that resumes `conversation`.
    // `prefill` is typed into a Claude Code agent's prompt, unsent.
    Q_INVOKABLE bool resumeAgent(const QString& directory, const QString& title,
                                 const QString& harness, const QString& conversation,
                                 const QString& mode = {}, const QString& prefill = {});
    // The config's new-agent defaults, for the forms: harness, folder, and
    // the folder on each ssh machine.
    Q_INVOKABLE [[nodiscard]] QVariantMap agentDefaults() const;
    // Where an agent is: its category's name, its ssh machine (or ""), and
    // its folder as the card shows it ("~/x", or "host:~/x" over ssh).
    Q_INVOKABLE [[nodiscard]] QVariantMap agentPlace(const QString& id) const;
    void setAgentDefaults(const AgentDefaults& defaults) { agent_defaults_ = defaults; }
    // Where the new-agent forms' model lists come from; lapis keeps it.
    void setHarnessModels(const HarnessModels* models) { harness_models_ = models; }
    // Starts an agent; returns its id, or "" with workspaceError(). Without
    // `select` the category's selection is left alone, so an agent started
    // from another device never takes the stage from a shown agent.
    QString startAgent(const AgentRequest& request);
    [[nodiscard]] const QString& storagePath() const { return storage_path_; }
    // This window holds the workspace registry's lock (it owns the workspace).
    [[nodiscard]] bool holdsRegistry() const {
        return registry_lock_ != nullptr && registry_lock_->isLocked();
    }
    // Close an agent. A reachable agent is ended through its session
    // service first and closes once the process exits. An unreachable one
    // stays unless `abandon` accepts that it may still be running outside
    // lapis.
    Q_INVOKABLE bool closeAgent(const QString& id, bool abandon = false);
    Q_INVOKABLE bool restartAgent(const QString& id);
    // Ends an agent's CLI and starts it again in its card, resuming its
    // conversation as a restart does: a CLI rereads its settings (Claude
    // Code's permissions, say) only when it starts. An ended agent starts at
    // once. An agent on another machine whose conversation lapis cannot name
    // is left alone, since it would start a new one. Returns how many reload.
    Q_INVOKABLE int reloadAgent(const QString& id);
    // Every agent in the category shown, or in every category.
    Q_INVOKABLE int reloadCategory();
    Q_INVOKABLE int reloadAll();
    Q_INVOKABLE bool moveAgent(const QString& id, const QString& categoryId);
    // A name someone chose; it stays until they choose another.
    Q_INVOKABLE bool renameAgent(const QString& id, const QString& title);
    // The agent's conversation title, from its CLI. It names an agent that
    // still has the name it started with; one someone renamed keeps theirs.
    bool followConversationTitle(const QString& id, QStringView title);
    // Each agent's current conversation, as its CLI resumes it: agent id to
    // conversation id, for the agents that have one.
    [[nodiscard]] QHash<QString, QString> agentConversations() const;
    // One agent's current conversation, or empty.
    [[nodiscard]] QString agentConversation(const QString& id) const;
    // The file an agent's CLI saves its current conversation in, on this Mac,
    // for the visual view: Claude Code's transcript, Codex's rollout or
    // Antigravity's transcript_full.jsonl. The conversation lapis knows
    // (observer, notify or hook record, the pinned one, or the resumed one)
    // names it. For Codex and Antigravity this first finds and pins the one
    // the agent's own process is writing (conversation_owner.hpp). Empty for
    // an agent on another machine, a CLI the view cannot read, or before the
    // CLI saved anything.
    Q_INVOKABLE [[nodiscard]] QString conversationFile(const QString& id);
    // Copies an agent's conversation up to `record` (a saved record's uuid;
    // `before` stops just ahead of it) into a new conversation beside it and
    // returns the new id for resumeAgent, or empty.
    Q_INVOKABLE [[nodiscard]] QString forkConversation(const QString& id, const QString& record,
                                                       bool before) const;
    // The folder an agent runs in, as launched.
    Q_INVOKABLE [[nodiscard]] QString agentFolder(const QString& id) const;
    [[nodiscard]] QObject* transcript() const { return transcript_; }
    Q_INVOKABLE bool moveAgentBy(const QString& id, int delta);
    // Puts agents, in their strip order, at `index` of a category's strip (at
    // its end when `index` is past it), moving them there from any category.
    Q_INVOKABLE bool placeAgents(const QStringList& ids, const QString& categoryId, int index);
    // Moves a category to `index` in the rail.
    Q_INVOKABLE bool placeCategory(const QString& id, int index);
    Q_INVOKABLE bool removeAgent(const QString& id);
    // The next agent in the active category's order, wrapping inside it.
    Q_INVOKABLE void nextAgent(int delta = 1);
    // The next agent in the sidebar's order: each category's agents in turn,
    // from the last agent of one category into the first of the next and
    // wrapping at the ends; categories without agents are passed over. The
    // active category follows. From an empty category it starts at the first
    // agent after it (or the last before it, going back).
    Q_INVOKABLE void nextAgentAcrossCategories(int delta = 1);
    // Tab's next agent, in any category, of those truly waiting on you: a
    // request, or a finished turn (no background work in flight) that is
    // unseen or has a guessed next prompt not yet seen: a turn the person
    // has looked at, guess and all, waits no longer. Agents at work, paused on their own
    // background work, or without a status never qualify. The TabRanker
    // orders them (anything overdue first, then work, requests, unseen,
    // newest). `ready` maps agents
    // with a guess to whether it was seen. Advances past the focused agent;
    // false (none waiting) leaves Tab with the program. Every move is logged
    // with the candidates and their scores.
    Q_INVOKABLE bool nextPriorityAttention(const QVariantMap& ready);
    // An agent is parked once the person has looked at its finished turn for
    // kParkAfterMs and moved on without a prompt: Tab skips it, and
    // agent_state.json marks it, until it begins to wait again. A request is
    // never parked.
    static constexpr qint64 kParkAfterMs = 2500;
    [[nodiscard]] qint64 parkedAtMs(const QString& id) const;
    [[nodiscard]] bool parked(const Agent* item) const;
    void setParkAfterMsForTesting(qint64 ms) { park_after_ms_ = ms; }
    void setTabAway(const TabAwaySettings& settings);
    [[nodiscard]] const TabAwaySettings& tabAway() const { return tab_away_; }
    // The visual view's budget for threads kept parsed (visual.keepMB,
    // keepThreads). With `ahead` (every agent shows as a thread), a few
    // seconds later the threads of this Mac's agents are read ahead once,
    // most recently written first, so the first switch to each is at once.
    void setVisualKeep(int mb, int threads, bool ahead);
    // Reads ahead the threads of this Mac's agents now (see setVisualKeep).
    void warmThreads();
    // Where the guesses come from when the person moves by hand, for the
    // choices the move log records (NextPrompt::readyAgents).
    void setGuesses(std::function<QVariantMap()> guesses) { guesses_ = std::move(guesses); }
    // How long the person stays on an agent before it counts as their choice.
    void setChoiceSettleMsForTesting(int ms) { choice_settle_.setInterval(ms); }
    [[nodiscard]] QVariantList agents() const;
    [[nodiscard]] int focusedIndex() const { return focused_index_; }
    [[nodiscard]] Agent* focusedAgent() const;
    // Whether the person is looking at `item` now: it is on the stage, the
    // window is active and someone is at the Mac (main sets the test).
    // Without one, the stage agent counts, as in fixtures and tests.
    using Looking = std::function<bool(const Agent*)>;
    void setLooking(Looking looking) { looking_ = std::move(looking); }
    [[nodiscard]] bool looking(const Agent* item) const;
    // Whether `item` shows nothing new since the person last looked at it
    // (SeenScreens): such a finished turn marks nothing.
    void setUnchanged(Looking unchanged) { unchanged_ = std::move(unchanged); }
    // The person is looking at the stage agent if looking() says so: its
    // unseen mark clears for good. Runs on every focus change; the window
    // calls it when it becomes active and each second while it is (`how`
    // says which, for the log).
    void noteLooking(const QString& how);
    // Every unseen mark set or cleared, one JSON object each, for
    // attention.jsonl: {"event": "unseen"|"seen", "id", "why"}. Ids only.
    void setMarkLog(std::function<void(const QJsonObject&)> log) { mark_log_ = std::move(log); }
    // Starts the most recently closed agent again, resuming its conversation
    // where its CLI can, in its category, and shows it.
    Q_INVOKABLE bool reopenAgent();
    [[nodiscard]] bool canReopenAgent() const { return !closed_.empty(); }
    // The window's own marks on each agent, for GuiState: unseen, when it
    // began to need you, whether it was at work, and the conversation they
    // belong to. Restoring skips agents that are gone or now in another
    // conversation (as after /clear) and never pings. A restored mark stays
    // until the person looks, the stage agent's included. An agent that was
    // at work and is found idle finished while no window watched: it is
    // marked unseen once, however long the window was gone, and
    // finishedWhileAway() says so (only within ten minutes), without a chime.
    [[nodiscard]] QJsonObject saveMarks() const;
    void restoreMarks(const QJsonObject& marks);
    // The agents Command-Shift-T can bring back, for GuiState.
    [[nodiscard]] QJsonArray saveClosed() const;
    void restoreClosed(const QJsonArray& closed);
    // Arguments from lapis.json added to each new agent of a harness.
    void setHarnessArguments(QHash<QString, QStringList> arguments) {
        harness_arguments_ = std::move(arguments);
    }
    // CLIs lapis.json pins: never updated on start.
    // The constructor copies the startup set; this setter applies live reloads.
    void setHarnessUpdatesOff(QSet<QString> harnesses) {
        harness_updates_off_ = std::move(harnesses);
    }
    // The orchestrator the person pinned (saved in the registry), or empty.
    [[nodiscard]] const QString& homeAgentId() const { return home_agent_id_; }
    // The pinned agent while it is open; else the agy agent chosen before
    // while it stays open, else the most recently active agy agent on this
    // Mac, else the stage agent. Once chosen, an automatic choice stays
    // while it is open, so the orchestrator does not follow the stage.
    [[nodiscard]] Agent* effectiveHomeAgent() const;
    // Pins `agentId` as the orchestrator; empty unpins. False for an agent
    // that is not open or a registry that cannot be saved.
    Q_INVOKABLE bool setHomeAgent(const QString& agentId);
    // The agent a citation names: lapis://agent/<id or unique prefix> or
    // conversation://<conversation id or unique prefix>; empty for none.
    Q_INVOKABLE [[nodiscard]] QString resolveAgentLink(const QString& link) const;
    // Agents other than the orchestrator with a request pending, oldest first.
    [[nodiscard]] QVariantList attentionInbox() const;
    // Every agent but the orchestrator, by stage then sidebar order (see
    // factory_state.hpp for the stage rules and the judge's file).
    [[nodiscard]] QVariantList factoryPipeline() const;
    [[nodiscard]] QVariantMap factorySummary() const;
    // <registry folder>/factory_state.json; empty without a registry.
    [[nodiscard]] const QString& factoryStatePath() const { return factory_state_path_; }
    // Reads the judge's file again now (it is also watched).
    Q_INVOKABLE void reloadFactoryState();
    // Fixture workspaces only, for tests and captures: another sample agent,
    // a conversation file to show for one, and where the judge's file is.
    bool addFixtureAgent(const QString& id, const QString& title, const QString& category,
                         const QString& harness, const QString& content);
    bool setFixtureConversation(const QString& id, const QString& file);
    void setFactoryStatePath(const QString& path);
  signals:
    void homeAgentChanged();
    void attentionInboxChanged();
    void factoryPipelineChanged();
    void focusChanged();
    // A Codex or Claude turn ended; terminal agents' output pauses do not count.
    // A new request for you arrives here too, once its attention is applied.
    void turnFinished(lapis::desktop::Agent* item);
    // The same moment, after the agent's mark: `outcome` is "marked" (newly
    // unseen: the one moment that may chime), "looking" (seen as it
    // happened), "waiting" (already unseen), "unchanged" (nothing new since
    // the person looked) or "repeat" (a request already marked once).
    void needsYou(lapis::desktop::Agent* item, const QString& outcome);
    void agentsChanged();
    void categoriesChanged();
    void categoryChanged();
    void errorChanged();
    void closedChanged();
    // What saveMarks() returns may have changed.
    void marksChanged();
    // An agent was parked (see parkedAtMs).
    void parkedChanged();
    // A turn that ended while no window watched (see restoreMarks).
    void finishedWhileAway(lapis::desktop::Agent* item);

  private:
    [[nodiscard]] static QString rootDirectory();
    [[nodiscard]] static QString defaultEndpoint();
    std::vector<std::unique_ptr<Agent>> items_;
    QHash<QString, QStringList> harness_arguments_;
    QSet<QString> harness_updates_off_;
    AgentDefaults agent_defaults_;
    QString ssh_config_{QDir::home().filePath(QStringLiteral(".ssh/config"))};
    const HarnessModels* harness_models_{};
    bool restore_agents_{};
    bool update_harnesses_{};
    bool headless_{};
    QHash<QString, qint64> harness_checked_ms_;
    QHash<QProcess*, QByteArray> updater_output_;
    QHash<QProcess*, bool> updater_stopping_;
    qint64 update_timeout_ms_{qint64{2} * 60 * 1000};
    // An update process, not yet started: its whole group stops at the
    // timeout, and `finished` runs once it has, with how it ended.
    QProcess* newUpdater(const QString& program, const QStringList& arguments,
                         const std::function<void(QProcess*, const QString&, bool)>& finished);
    // True when the agent waits for its CLI's update and starts after it.
    bool deferForUpdate(const QString& id);
    // One updater per CLI on this Mac, and the agents waiting to start.
    struct CliUpdate {
        QString harness;
        QPointer<QProcess> process;
        QStringList starts;
    };
    QHash<QString, CliUpdate> cli_updates_; // by harness
    void finishCliUpdate(const QString& harness, QProcess* process, const QString& outcome);
    void drainUpdater(QProcess* process);
    void startUpdatedAgents(const QStringList& ids);
    void logUpdate(const QString& line) const;
    QString update_log_directory_;
    struct Category {
        QString id;
        QString name;
        QString selected;
        // A colour chosen from the category menu, by name; empty takes one by
        // position. A name this build does not know is kept and saved as read.
        QString color{};
    };
    struct AgentRecord {
        QString category;
        QString endpoint;
        session::LaunchSpec launch;
        QString harness{QStringLiteral("codex")};
        // Its name was chosen (on the Mac or the phone), not taken from its
        // folder or its conversation.
        bool named{};
        // Its name is its conversation's title, which it keeps following.
        bool auto_title{};
        // The exact resume pair lapis appended, or no provenance for a
        // user-authored launch. Existing unmarked records stay user-owned.
        int managed_resume_index{-1};
        QString managed_resume_identity{};
        // The conversation lapis found this agent's own CLI writing, when the
        // CLI has not named it (agy never does; Codex before its first turn
        // ends): kept, and resumed on restart, until the process holds another.
        QString conversation{};
    };
    std::vector<Category> categories_;
    QMap<QString, AgentRecord> agents_;
    // Codex thread id -> its rollout, found once (conversationFile).
    mutable QHash<QString, QString> rollout_paths_;
    // Claude Code session id -> its transcript, found once: the search
    // looks in every project folder (milliseconds on each switch).
    QHash<QString, QString> claude_paths_;
    QHash<QString, qint64> claude_searched_; // id -> when a search last missed it
    bool warm_scheduled_{};                  // threads are read ahead (setVisualKeep)
    // Pins what each local Codex and agy agent's CLI is writing, as the
    // visual view does, so restore resumes it even for one never shown.
    QTimer conversation_pins_;
    [[nodiscard]] conversation_owner::Agent conversationOwner(const QString& id) const;
    void pinConversations();
    void applyPins(const QHash<QString, QString>& pins);
    // conversationFile without placing or re-checking pins: the file of the
    // conversation lapis already knows the agent writes, or empty.
    [[nodiscard]] QString knownConversationFile(const QString& id);
    QString active_category_{QStringLiteral("general")};
    QString storage_path_;
    std::unique_ptr<QLockFile> registry_lock_;
    QString error_;
    bool storage_failed_{};
    // Agent records restore() could not read, written back unchanged.
    QJsonArray unreadable_agents_;
    bool fail(const QString& message);
    struct ResumeLaunch {
        session::LaunchSpec launch;
        int managed_resume_index{-1};
        QString managed_resume_identity{};
    };
    // Build the launch and its managed resume provenance together.
    std::optional<ResumeLaunch> agentLaunch(const AgentRequest& request);
    QString insertCategory(const QString& name, bool select);
    // Starts an agent from a finished launch; the rest of startAgent. A
    // managed resume plan is committed in the same registry save as the
    // launch, before the process starts.
    QString launchAgent(const AgentRequest& request, const session::LaunchSpec& launch,
                        int managed_resume_index = -1, const QString& managed_resume_identity = {});
    Category* category(const QString& id);
    // Where agents moving to `categoryId` go among those that stay: before its
    // index-th agent, or after its last.
    [[nodiscard]] std::size_t insertionPoint(const std::vector<std::unique_ptr<Agent>>& staying,
                                             const QString& categoryId, int index) const;
    // Agents moving into `categoryId` leave their old category's stage.
    void recategorize(const QStringList& ids, const QString& categoryId);
    QString failed(const QString& message) {
        fail(message);
        return {};
    }
    struct RegistryState {
        std::vector<Category> categories;
        QMap<QString, AgentRecord> agents;
        QString active;
        int focused{-1};
    };
    [[nodiscard]] RegistryState checkpoint() const;
    void rollback(const RegistryState& previous);
    bool mutableRegistry();
    bool commit(const RegistryState& previous);
    bool save(const QString& renamedId = {}, const QString& renamedTitle = {});
    [[nodiscard]] static QJsonObject agentRecord(const AgentRecord& agent, const QString& id,
                                                 const QString& title);
    void lockRegistry();
    void restore();
    void connectRestoredAgents();
    void loadCategories(const QJsonArray& groups);
    static void loadManagedResume(const QJsonValue& value, AgentRecord& agent);
    void loadAgents(const QJsonArray& agents);
    void finishClosing(Agent* item);
    [[nodiscard]] static Agent::StatusSource statusSource(const AgentRecord& agent);
    [[nodiscard]] static QStringList savedArguments(const QJsonValue& value);
    static void applyStartupDefaults(const AgentRecord& agent, ResumeLaunch& plan);
    [[nodiscard]] static std::optional<ResumeLaunch> restoredLaunch(const AgentRecord& agent,
                                                                    QString* diagnostic = nullptr);
    struct ClosedAgent {
        QString category;
        QString title;
        QString harness;
        ResumeLaunch plan;
        bool remote{};
    };
    // Newest last; at most ten.
    std::vector<ClosedAgent> closed_;
    void rememberClosed(const AgentRecord& agent, const QString& title);
    // Marks an agent saved at work as finished if its first settled status
    // after the restart is not work; `recent` also reports finishedWhileAway.
    void watchFinishWhileAway(Agent* item, bool recent);
    [[nodiscard]] static bool serviceRunning(const QString& endpoint);
    void noteStatus(Agent* item);
    // The last settled status of each agent: reconnecting, unknown and
    // unreachable states leave it, so a republished status is no new turn.
    QHash<const Agent*, QString> last_kind_;
    // Why an agent may need the person; the one rule that marks it unseen.
    enum class Need : std::uint8_t { finished, quiet, request, away };
    QString markNeedsYou(Agent* item, Need need);
    void logMark(const Agent* item, const char* event, const QString& why) const;
    Looking looking_;
    Looking unchanged_;
    std::function<void(const QJsonObject&)> mark_log_;
    // Request ids each agent was already marked for, by agent id (bounded).
    QHash<QString, QSet<QString>> known_requests_;
    // Tab's ranking, and the choices it records: the agents waiting when
    // the person left one, kept until they settle on another (or Tab moved
    // them), then recorded with the one they settled on.
    std::unique_ptr<TabRanker> tab_ranker_{std::make_unique<TabRanker>()};
    TabAwaySettings tab_away_;
    std::function<QVariantMap()> guesses_;
    // When each agent began waiting (a turn finished or a request arrived),
    // and when its recent turns started (bounded to the last hour).
    QHash<QString, qint64> waiting_since_;
    QHash<QString, std::vector<qint64>> turn_starts_;
    struct PendingChoice {
        std::vector<TabCandidate> candidates;
        bool viaTab{};
        // Agents that began waiting while the person was still moving join.
        void add(const std::vector<TabCandidate>& fresh);
    };
    std::optional<PendingChoice> choice_;
    QString last_focus_id_;
    qint64 focus_since_ms_{}; // when the person moved to last_focus_id_
    // When the person left each agent after looking at its finished turn
    // (kParkAfterMs or more): it is parked until it next begins to wait.
    QHash<QString, qint64> parked_at_;
    qint64 park_after_ms_{kParkAfterMs};
    QTimer choice_settle_;
    [[nodiscard]] std::vector<TabCandidate> waitingCandidates(const QVariantMap& ready,
                                                              const Agent* exclude) const;
    void noteFocusMove();
    void settleChoice();
    void rememberInitialFocus();
    void noteStructuralFocus();
    bool discardAgent(const QString& id);
    void changed();
    void restoreSelection();
    void watch(Agent* item);
    // A remote agent's reconnection after its connection dropped.
    struct Reconnect {
        QElapsedTimer ready; // since its session last became ready
        QElapsedTimer since; // since the drop these attempts follow
        std::size_t count{};
        int generation{};
    };
    QHash<QString, Reconnect> reconnects_;
    QObject* transcript_{}; // a TranscriptModel this workspace owns
    // The ssh host a remote agent runs on; empty on this Mac.
    [[nodiscard]] static QString agentMachine(const AgentRecord& agent);
    // Sets what the agent's next start needs beyond its saved launch: OMP's
    // overlay in the card's service environment. A remote command saved with
    // a retired plan preamble loses it, so the machine's own sign-in is used.
    void prepareStart(AgentRecord& agent, Agent& item);
    // Agents asked to end so they can start again.
    QSet<QString> reloading_;
    enum class ReloadOutcome : std::uint8_t { requested, kept, failed };
    struct ReloadResult {
        ReloadOutcome outcome{};
        QString diagnostic;
    };
    ReloadResult requestReload(const QString& id);
    int reloadAgents(const QStringList& ids);
    void finishReload(const QString& id, int waits = 30);
    std::optional<session::TerminalSize> launch_size_;
    // `launch` at the stage's size, when the window has told it.
    [[nodiscard]] session::LaunchSpec sized(session::LaunchSpec launch) const;
    // Agents restarted as the window opens wait for the stage's size (at most
    // unsized_wait), so a resumed conversation is drawn at the width it is
    // shown in. Claude Code's classic renderer never redraws what it printed.
    struct UnsizedStart {
        QPointer<Agent> document;
        QString endpoint;
        session::LaunchSpec launch;
    };
    std::vector<UnsizedStart> unsized_starts_;
    QTimer unsized_timer_;
    static constexpr int unsized_wait_ms = 3000;
    void startUnsized();
    std::chrono::milliseconds reconnect_first_hold_{std::chrono::seconds(20)};
    std::optional<std::chrono::milliseconds> reconnect_wait_;
    void reconnectIfDropped(const QString& id);
    // Remote agents that reached their machine before: restored ones, and
    // ones whose connection held. They retry without giving up.
    QSet<QString> worked_remotely_;
    // A view's reattachment to a service that is still running.
    struct Reattach {
        std::size_t count{};
        int generation{};
        bool was_ready{};
        bool discovered{};
    };
    QHash<QString, Reattach> reattaches_;
    void reattachIfDisconnected(const QString& id);
    QTimer network_settle_;
    void watchNetwork();
    void retryConnectionsNow();
    int focused_index_{-1};
    bool fixture_mode_{};
    // Factory view state (workspace_factory.cpp).
    QString home_agent_id_;
    mutable QString auto_home_id_;  // the automatic orchestrator, once chosen
    mutable qint64 auto_home_at_{}; // when that choice last showed activity
    QString published_home_id_;
    QString published_pin_id_;
    QString factory_state_path_;
    factory::State factory_state_;
    QFileSystemWatcher* factory_watcher_{};
    QTimer factory_publish_;
    mutable QTimer factory_expiry_;
    // When each agent's settled status or requests last changed in this run.
    QHash<QString, qint64> factory_changed_at_;
    QHash<QString, QString> factory_kind_;
    QHash<QString, bool> factory_attention_pending_;
    QHash<QString, QString> fixture_files_;
    mutable bool factory_dirty_{true};
    mutable QVariantList factory_items_;
    mutable QVariantMap factory_summary_;
    mutable QVariantList attention_inbox_;
    void setupFactory();
    void watchFactory(Agent* item);
    void scheduleFactory();
    void publishFactory();
    void rebuildFactory() const;
    void watchFactoryFile();
    [[nodiscard]] QString factoryAutoHome() const;
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_WORKSPACE_HPP
