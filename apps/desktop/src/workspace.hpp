#ifndef LAPIS_DESKTOP_WORKSPACE_HPP
#define LAPIS_DESKTOP_WORKSPACE_HPP

#include "accounts.hpp"
#include "harness_catalog.hpp"
#include "harness_models.hpp"
#include "history_strip.hpp"
#include "keymap.hpp"
#include "tile_layout.hpp"
#include <lapis/session/terminal.hpp>

#include "launch_spec.hpp"
#include "transport/attention_protocol.hpp"
#include "transport/local_protocol.hpp"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QLockFile>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QProcess;

namespace lapis::desktop {

class LiveConnection;

class SessionPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sessionId READ sessionId CONSTANT)
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
    // Finished a turn or started needing a response while another agent was
    // selected; cleared when this agent is selected.
    Q_PROPERTY(bool unseen READ unseen NOTIFY unseenChanged)
  public:
    SessionPreview(QString title, QString directory, QString activity, QColor accent,
                   std::string_view content);
    ~SessionPreview() override;
    void startLive(const QString& endpoint, const session::LaunchSpec& launch,
                   session::wire::AttachMode mode = session::wire::AttachMode::reconnect);
    // What a session service this card starts adds to its environment: the
    // plan's credential, when lapis chose one. Kept in memory only.
    void setServiceEnvironment(QHash<QString, QString> environment) {
        service_environment_ = std::move(environment);
    }
    [[nodiscard]] const QHash<QString, QString>& serviceEnvironment() const {
        return service_environment_;
    }
    // When its screen last changed, in milliseconds since the epoch; 0 never.
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void discoverSession();
    Q_INVOKABLE void startNewSession();
    // A screen back or forward through kept history.
    Q_INVOKABLE void olderHistory();
    Q_INVOKABLE void newerHistory();
    // Scrolls kept history by rows, positive back; scrolling forward past the
    // newest row returns to live.
    Q_INVOKABLE void scrollHistory(int rows);
    Q_INVOKABLE void returnToLive();
    // Moves the view to `fraction` of everything kept (0 the oldest row).
    Q_INVOKABLE void historyAt(qreal fraction);
    Q_INVOKABLE bool respondAttention(const QString& token, const QVariantMap& response);
    // Ask the session service to end this agent's process. Returns false when
    // the request could not be queued on a synchronized connection.
    bool terminate();
    [[nodiscard]] bool closing() const { return closing_; }
    void setClosing(bool closing);
    // Shown instead of the status while the agent's CLI updates before start.
    void setUpdating(const QString& label);
    [[nodiscard]] bool updating() const { return !updating_.isEmpty(); }
    [[nodiscard]] bool unseen() const { return unseen_; }
    void setUnseen(bool unseen);
    // When it last began to need you (ms since the epoch); 0 before that.
    [[nodiscard]] qint64 neededAtMs() const { return needed_at_ms_; }
    // Where activity comes from: a service-side observer (the Codex app-server,
    // Claude Code's hook relay) or, for other CLIs, an output-timing estimate.
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
    void sendKey(session::TerminalKey key, session::KeyModifiers modifiers);
    // A turn of the wheel for the program on the alternate screen, over a
    // viewport cell; only when its snapshot says the service accepts wheels.
    // False means the clamped wheel was not queued.
    bool sendWheel(int steps, int column, int row);
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
    void setSessionId(const QString& id) { session_id_ = id; }
    [[nodiscard]] const QString& sessionId() const { return session_id_; }
    [[nodiscard]] bool attentionPending() const { return attentionCount() != 0; }
    [[nodiscard]] QString attentionReason() const;
    [[nodiscard]] quint32 attentionSerial() const { return attention_serial_; }
    [[nodiscard]] int attentionCount() const;
    bool addPreviewRequest(const QString& id, const QString& reason);
    bool resolvePreviewRequest(const QString& id);
    void clearPreviewRequests();
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
    QString session_id_;
    QMap<QString, QString> requests_;
    std::optional<session::wire::AttentionSnapshot> attention_;
    QSet<QString> submitted_attention_;
    quint32 attention_serial_{};
    bool awaiting_first_prompt_{};
    bool live_snapshot_ready_{};
    bool live_snapshot_received_{};
    bool closing_{};
    QString updating_;
    bool unseen_{};
    qint64 needed_at_ms_{};
    StatusSource status_source_{StatusSource::observer};
    // Output estimate: several frames close together read as activity, and a
    // few quiet seconds after that as a pause. Neither implies a finished task.
    void noteOutput();
    [[nodiscard]] QString unobservedStatusKind() const;
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
    // they were delivered. This belongs to the session so a rebound surface
    // cannot lose it; rejected wheels leave the previous debt intact.
    int program_wheel_debt_{};
    QPoint program_wheel_cell_{-1, -1};
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

enum class WorkspaceMode : std::uint8_t { live, preview };

struct PreviewRequest {
    QString session_id;
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
    // An explicit form or wire title is someone's choice, even when it happens
    // to match the folder. A wire request that omits the title still gets the
    // generated folder default and can follow the conversation.
    bool named{};
    // Show it on the stage; otherwise the category's selection stays.
    bool select{};
    // A conversation to resume, as the CLI's resume option takes it.
    QString resume;
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
    // Production bounds. Tests inject short values so stuck-updater cleanup is
    // observable without waiting two minutes or leaving installer children.
    qint64 updateTimeoutMs{qint64{2} * 60 * 1000};
    // No window: leave running services for a window to reattach, and hand the
    // workspace to a window that opens (the login helper and the windowless
    // host that serves the phone).
    bool headless{};
    // The config's plans, known before restored agents start (see setAccounts).
    AccountsConfig accounts{};
};

class Workspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    // Agents in any category that finished unseen or wait on a request.
    Q_PROPERTY(int attentionAgents READ attentionAgents NOTIFY categoriesChanged)
    Q_PROPERTY(QVariantList categorySessions READ categorySessions NOTIFY sessionsChanged)
    Q_PROPERTY(QString activeCategoryId READ activeCategoryId NOTIFY categoryChanged)
    Q_PROPERTY(QString workspaceError READ workspaceError NOTIFY errorChanged)
    Q_PROPERTY(bool previewMode READ previewMode CONSTANT)
    Q_PROPERTY(QString homeDirectory READ homeDirectory CONSTANT)
    Q_PROPERTY(int focusedIndex READ focusedIndex NOTIFY focusChanged)
    Q_PROPERTY(
        lapis::desktop::SessionPreview* focusedSession READ focusedSession NOTIFY focusChanged)
    // The active category's tiles, in unit coordinates of the stage: each
    // {sessionId, session, x, y, width, height}. Empty while the stage shows
    // one agent.
    Q_PROPERTY(QVariantList stageTiles READ stageTiles NOTIFY tilesChanged)
    // Each split's divider line and the area it divides, in the same units:
    // {path, stacked, x, y, width, height, areaX, areaY, areaWidth, areaHeight}.
    Q_PROPERTY(QVariantList stageDividers READ stageDividers NOTIFY tilesChanged)
    // An agent closed since lapis opened can come back with its conversation.
    Q_PROPERTY(bool canReopenAgent READ canReopenAgent NOTIFY closedChanged)
  public:
    explicit Workspace(WorkspaceMode mode = WorkspaceMode::live, WorkspaceOptions options = {});
    ~Workspace() override;
    [[nodiscard]] bool previewMode() const { return preview_mode_; }
    [[nodiscard]] QString homeDirectory() const;
    Q_INVOKABLE [[nodiscard]] QVariantList availableHarnesses() const;
    [[nodiscard]] std::vector<ModelChoice> modelChoices(const QString& harness) const;
    Q_INVOKABLE [[nodiscard]] QString displayPath(const QString& directory) const;
    [[nodiscard]] SessionPreview* session(const QString& id) const;
    // Development fixture v1 only. No calls are accepted in a live workspace.
    bool requestAttention(const PreviewRequest& request);
    bool resolveAttention(const PreviewRequest& request);
    Q_INVOKABLE bool replayAttention(const QString& scenario);
    [[nodiscard]] QVariantList categories() const;
    [[nodiscard]] QVariantList categorySessions() const;
    [[nodiscard]] const QString& activeCategoryId() const { return active_category_; }
    [[nodiscard]] const QString& workspaceError() const { return error_; }
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool addCategory(const QString& name);
    // A new category's id, or empty with the reason in workspaceError.
    QString createCategory(const QString& name);
    Q_INVOKABLE bool renameCategory(const QString& id, const QString& name);
    Q_INVOKABLE bool removeCategory(const QString& id);
    Q_INVOKABLE bool selectCategory(const QString& id);
    Q_INVOKABLE void nextCategory(int delta = 1);
    Q_INVOKABLE bool selectSession(const QString& id);
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
    Q_INVOKABLE bool resumeAgent(const QString& directory, const QString& title,
                                 const QString& harness, const QString& conversation,
                                 const QString& mode = {});
    // The config's new-agent defaults, for the forms: harness, folder, and
    // the folder on each ssh machine.
    Q_INVOKABLE [[nodiscard]] QVariantMap agentDefaults() const;
    // Where an agent is: its category's name, its ssh machine (or ""), and
    // its folder as the card shows it ("~/x", or "host:~/x" over ssh).
    Q_INVOKABLE [[nodiscard]] QVariantMap agentPlace(const QString& id) const;
    // The machines ("" for this Mac) where agents of these CLIs run.
    [[nodiscard]] QStringList machinesRunning(const QStringList& harnesses) const;
    void setAgentDefaults(const AgentDefaults& defaults) { agent_defaults_ = defaults; }
    // Where the new-agent forms' model lists come from; lapis keeps it.
    void setHarnessModels(const HarnessModels* models) { harness_models_ = models; }
    // Starts an agent; returns its id, or "" with workspaceError(). Without
    // `select` the category's selection is left alone, so an agent started
    // from another device never takes the stage from a shown agent.
    QString startAgent(const AgentRequest& request);
    [[nodiscard]] const QString& storagePath() const { return storage_path_; }
    // Close an agent's tab. A reachable agent is ended through its
    // session service first and its tab closes once the process exits. An
    // unreachable one keeps its tab unless `abandon` accepts that it may still
    // be running without one.
    Q_INVOKABLE bool closeSession(const QString& id, bool abandon = false);
    Q_INVOKABLE bool restartAgent(const QString& id);
    // The plans lapis may give Claude Code and Codex sessions, and how full
    // each is. A session whose plan passes the switch point starts again on
    // the one with the most room once it is between turns, resuming its
    // conversation (see AccountPool).
    void setAccounts(AccountsConfig accounts);
    void setAccountLoads(QHash<QString, AccountLoad> loads);
    // The plan an agent runs on; empty for its machine's own sign-in.
    Q_INVOKABLE [[nodiscard]] QString agentAccount(const QString& id) const;
    // Where the credential lapis gave the agent's plan is kept on the agent's
    // machine: a Claude Code token file or a Codex home ("~/" on another
    // machine). Empty when the agent uses its machine's own sign-in.
    [[nodiscard]] QString agentPlanCredential(const QString& id) const;
    Q_INVOKABLE [[nodiscard]] bool canSwitchAccount(const QString& id) const;
    // Starts the agent again on the plan with the most room besides its own.
    Q_INVOKABLE bool switchAccount(const QString& id);
    // Tests: where this Mac keeps the credentials lapis hands out.
    void setAccountsRootForTesting(const QString& root) { accounts_root_ = root; }
    // Ends an agent's CLI and starts it again in its tab, resuming its
    // conversation as a restart does: a CLI rereads its settings (Claude
    // Code's permissions, say) only when it starts. An ended agent starts at
    // once. An agent on another machine whose conversation lapis cannot name
    // is left alone, since it would start a new one. Returns how many reload.
    Q_INVOKABLE int reloadAgent(const QString& id);
    // Every agent in the category shown, or in every category.
    Q_INVOKABLE int reloadCategory();
    Q_INVOKABLE int reloadAll();
    // Updates the agent's CLI where it runs, with the CLI's own update
    // command, then reloads it so it runs the new version. One update per
    // CLI and machine serves every agent that asks; one that fails leaves
    // its agents running. Returns how many agents wait for an update.
    Q_INVOKABLE int updateAndReloadAgent(const QString& id);
    // The same for every Claude Code agent, on each machine that has one.
    Q_INVOKABLE int updateClaudeAndReload();
    // Whether the agent's CLI has an update command lapis can run.
    Q_INVOKABLE [[nodiscard]] bool canUpdateAgent(const QString& id) const;
    Q_INVOKABLE bool moveSession(const QString& id, const QString& categoryId);
    // A name someone chose; it stays until they choose another.
    Q_INVOKABLE bool renameSession(const QString& id, const QString& title);
    // The agent's conversation title, from its CLI. It names an agent that
    // still has the name it started with; one someone renamed keeps theirs.
    bool followConversationTitle(const QString& id, QStringView title);
    // Each agent's current conversation, as its CLI resumes it: agent id to
    // conversation id, for the agents that have one.
    [[nodiscard]] QHash<QString, QString> agentConversations() const;
    // One agent's current conversation, or empty.
    [[nodiscard]] QString agentConversation(const QString& id) const;
    Q_INVOKABLE bool moveSessionBy(const QString& id, int delta);
    // Puts agents, in their strip order, at `index` of a category's strip (at
    // its end when `index` is past it), moving them there from any category.
    Q_INVOKABLE bool placeSessions(const QStringList& ids, const QString& categoryId, int index);
    // Moves a category to `index` in the rail.
    Q_INVOKABLE bool placeCategory(const QString& id, int index);
    Q_INVOKABLE bool removeSession(const QString& id);
    Q_INVOKABLE void nextSession(int delta = 1);
    // Selects the next agent, in any category, that finished a turn or asked
    // for something while unseen; false when none is waiting.
    Q_INVOKABLE bool nextAttention();
    // Selects the agent that most recently began to need you; again, the one
    // before it. False when none is waiting.
    Q_INVOKABLE bool latestAttention();
    // Tab's next agent, in any category, of those that need you: first one
    // with a guessed next prompt not yet seen, then a turn that finished unseen
    // or a request, then a guess already seen (so Tab cannot bounce between
    // two guesses while others wait), the one waiting longest within each.
    // `ready` maps agents with a guess to whether it was seen. False when none.
    // Advances past the focused session; false leaves Tab with the program.
    Q_INVOKABLE bool nextPriorityAttention(const QVariantMap& ready);
    [[nodiscard]] QVariantList sessions() const;
    [[nodiscard]] int focusedIndex() const { return focused_index_; }
    [[nodiscard]] SessionPreview* focusedSession() const;
    [[nodiscard]] int attentionAgents() const;
    [[nodiscard]] QVariantList stageTiles() const;
    [[nodiscard]] QVariantList stageDividers() const;
    // Tiles `id` beside `target` on its category's stage (left, right, top,
    // bottom), or in its place (center), and selects it. With no target, beside
    // the selected agent.
    Q_INVOKABLE bool tileSession(const QString& id, const QString& target, const QString& edge);
    // Takes an agent off the stage; it keeps running in the strip.
    Q_INVOKABLE bool untileSession(const QString& id);
    // Moves a divider; `persist` saves it (once, when a drag ends).
    Q_INVOKABLE bool setTileRatio(const QString& path, qreal ratio, bool persist);
    // Selects the tile beside the selected one (left, right, top or bottom).
    Q_INVOKABLE bool focusTile(const QString& direction);
    // Starts an agent like the selected one (folder, CLI, model and mode) and
    // tiles it beside it; returns its id.
    Q_INVOKABLE QString splitAgent(const QString& edge);
    // Starts the most recently closed agent again, resuming its conversation
    // where its CLI can, in its category, and shows it.
    Q_INVOKABLE bool reopenAgent();
    [[nodiscard]] bool canReopenAgent() const { return !closed_.empty(); }
    // Arguments from lapis.json added to each new agent of a harness.
    void setHarnessArguments(QHash<QString, QStringList> arguments) {
        harness_arguments_ = std::move(arguments);
    }
  signals:
    void focusChanged();
    // An agent has a new request for you. Requests ping as finished turns do
    // (turnFinished); nothing emits this now.
    void agentNeedsYou(lapis::desktop::SessionPreview* item);
    // A Codex or Claude turn ended; terminal agents' output pauses do not count.
    void turnFinished(lapis::desktop::SessionPreview* item);
    void sessionsChanged();
    void categoriesChanged();
    void categoryChanged();
    void errorChanged();
    void tilesChanged();
    void closedChanged();

  private:
    [[nodiscard]] static QString rootDirectory();
    [[nodiscard]] static QString defaultEndpoint();
    std::vector<std::unique_ptr<SessionPreview>> sessions_;
    QHash<QString, QStringList> harness_arguments_;
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
    // One updater per CLI and machine (empty means local), with separate
    // consumers for first starts and successful-update reloads.
    struct CliUpdate {
        QString harness;
        QString machine;
        QPointer<QProcess> process;
        QStringList agents;
        QStringList starts;
        bool manual{};
    };
    QHash<QString, CliUpdate> cli_updates_;
    int updateAndReload(const QStringList& ids);
    void finishCliUpdate(const QString& key, QProcess* process, const QString& outcome,
                         bool succeeded);
    void drainUpdater(QProcess* process);
    void startUpdatedAgents(const QStringList& ids);
    void logUpdate(const QString& line) const;
    QString update_log_directory_;
    // Records conversations for agents whose services do not.
    QTimer conversation_timer_;
    std::shared_ptr<std::atomic_bool> probing_{std::make_shared<std::atomic_bool>(false)};
    void recordConversations();
    struct Category {
        QString id;
        QString name;
        QString selected;
        // Two or more agents tiled on the stage, or empty for one agent.
        TileLayout tiles;
    };
    struct Agent {
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
        // The Claude Code or Codex plan it runs on (an AccountPool name);
        // empty for its machine's own sign-in.
        QString account{};
    };
    std::vector<Category> categories_;
    QMap<QString, Agent> agents_;
    QString active_category_{QStringLiteral("general")};
    QString storage_path_;
    std::unique_ptr<QLockFile> registry_lock_;
    QString error_;
    bool storage_failed_{};
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
    [[nodiscard]] const Category* activeCategory() const;
    // After an agent leaves a category: off its stage, and one tile is no split.
    void untile(Category& category, const QString& id);
    void loadTiles(const QJsonArray& groups);
    // Where agents moving to `categoryId` go among those that stay: before its
    // index-th agent, or after its last.
    [[nodiscard]] std::size_t
    insertionPoint(const std::vector<std::unique_ptr<SessionPreview>>& staying,
                   const QString& categoryId, int index) const;
    // Agents moving into `categoryId` leave their old category's stage.
    void recategorize(const QStringList& ids, const QString& categoryId);
    QString failed(const QString& message) {
        fail(message);
        return {};
    }
    struct RegistryState {
        std::vector<Category> categories;
        QMap<QString, Agent> agents;
        QString active;
        int focused{-1};
    };
    [[nodiscard]] RegistryState checkpoint() const;
    void rollback(const RegistryState& previous);
    bool mutableRegistry();
    bool commit(const RegistryState& previous);
    bool save(const QString& renamedId = {}, const QString& renamedTitle = {});
    [[nodiscard]] static QJsonObject agentRecord(const Agent& agent, const QString& id,
                                                 const QString& title);
    void lockRegistry();
    void restore();
    void connectRestoredAgents();
    void loadCategories(const QJsonArray& groups);
    static void loadManagedResume(const QJsonValue& value, Agent& agent);
    void loadAgents(const QJsonArray& agents);
    void finishClosing(SessionPreview* item);
    [[nodiscard]] static SessionPreview::StatusSource statusSource(const Agent& agent);
    [[nodiscard]] static QStringList savedArguments(const QJsonValue& value);
    static void applyStartupDefaults(const Agent& agent, ResumeLaunch& plan);
    [[nodiscard]] static std::optional<ResumeLaunch> restoredLaunch(const Agent& agent,
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
    void rememberClosed(const Agent& agent, const QString& title);
    [[nodiscard]] static bool serviceRunning(const QString& endpoint);
    void noteStatus(SessionPreview* item);
    QHash<const SessionPreview*, QString> last_kind_;
    bool batching_categories_{};
    bool discardSession(const QString& id);
    void changed();
    void restoreSelection();
    void watch(SessionPreview* item);
    // A remote agent's reconnection after its connection dropped.
    struct Reconnect {
        QElapsedTimer ready; // since its session last became ready
        QElapsedTimer since; // since the drop these attempts follow
        std::size_t count{};
        int generation{};
    };
    QHash<QString, Reconnect> reconnects_;
    AccountPool accounts_;
    QString accounts_root_;
    // Agents whose plan filled, moving at their next pause.
    QSet<QString> switching_;
    [[nodiscard]] QString accountsRoot() const;
    [[nodiscard]] static QString accountCli(const QString& harness);
    [[nodiscard]] static QString agentMachine(const Agent& agent);
    // Rejects a selected remote plan whose managed credential cannot be used.
    // The reason names the plan, never credential contents or provider identity.
    [[nodiscard]] static QString remoteAccountFailure(const AccountPool& accounts,
                                                      const Agent& agent,
                                                      const QString& accountsRoot);
    // Chooses the agent's plan and writes it where its start reads it: a
    // remote command's preamble, or the card's service environment.
    [[nodiscard]] bool applyAccount(Agent& agent, SessionPreview& item);
    void balanceAccounts();
    void switchWhenIdle();
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
    std::chrono::milliseconds reconnect_first_hold_{std::chrono::seconds(20)};
    std::optional<std::chrono::milliseconds> reconnect_wait_;
    void reconnectIfDropped(const QString& id);
    int focused_index_{-1};
    bool preview_mode_{};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_WORKSPACE_HPP
