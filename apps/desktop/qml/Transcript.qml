import QtQuick
import QtQuick.Controls
import QtQml.Models
import QtQuick.Dialogs

// The selected agent's conversation as a readable thread: the person's
// prompts as bubbles, the agent's replies as prose, and its work as quiet
// one-line steps ("Ran", "Edited", "Read") that open for detail. Text is
// selectable and every message can be copied; a prompt can be edited into a
// new agent. The terminal stays where input goes.
// Kinds the reader does not know show as plain blocks so new CLI features
// never vanish.
FocusScope {
    id: root

    property var model: null
    // Appearance, from the visual settings: a theme and font by name and a
    // text size in pixels, 0 to follow the terminal's.
    property string themeName: "terminal"
    property string fontName: "system"
    property string codeFontName: "system"
    property int sizeSetting: 0
    // What "match the terminal" matches.
    property int terminalFontSize: 14
    property string terminalFamily: "Menlo"
    property color terminalBackground: "#1a1b26"
    property color terminalAccent: "#7aa2f7"
    // The platform's interface font (SF Pro on the Mac).
    property string systemFamily: Qt.application.font.family
    property int radius: 8
    readonly property alias list: list
    // Follow new items while the reader sits at the end, as a terminal does.
    property bool following: true

    // The bar at the bottom types into the agent through these, which the
    // window wires to the terminal underneath: submit(text) returns whether
    // it was sent, interrupt() stops a turn, tabAway() moves to the next
    // agent that needs you (Tab with nothing typed or offered).
    property var submit: null
    property var interrupt: null
    property var tabAway: null
    // pressReturn() sends the agent one Return, as the key would: the
    // second try for a prompt its input box still holds.
    property var pressReturn: null
    // lapis's predicted next prompt (Tab), shown in the empty bar.
    property string suggestion: ""
    property bool suggestionConfident: false
    property bool working: false        // a turn is running: Send becomes Stop
    property bool waiting: false        // a request waits in the terminal
    property string waitingReason: ""
    property bool canSend: true
    // What Command-click on a file path opens it with (an editors() id).
    property string editor: "auto"
    // The agent's terminal rows as text (screenLines()), null where there
    // is no terminal. The thread never shows them: it reads them only to
    // see Claude Code compacting, a command going quiet, and whether a sent
    // prompt left the agent's input box.
    property var screen: null
    // The terminal's rows above its input box, read a few times a second
    // while a command or turn runs, to see when the screen goes quiet.
    property var liveRows: []
    // The running turn's work shows in the live work box, fed by the
    // transcript's own records as they land; the model knows it is running
    // from here.
    Binding {
        target: root.model
        property: "live"
        value: root.working
        when: root.model !== null
    }
    readonly property var liveSteps: model ? model.liveSteps : null
    readonly property bool liveShown: liveSteps !== null && liveSteps.active
    // A slash command the bar sent that the CLI has not recorded yet. Some
    // run for minutes with no turn (/compact), so it shows as running until
    // its record lands or the screen goes quiet.
    property string pendingCommand: ""
    property double pendingSince: 0
    property int quietTicks: 0
    // A prompt sent to an idle agent: the turn shows as starting at once,
    // until the agent works (or the prompt is found not sent).
    property double sentAt: 0
    // The agent's status says working, yet its file shows the last turn
    // ended (final reply, turn-end record) and nothing sent waits to be
    // recorded: a status estimated from terminal output (agy, Codex) can
    // lag the record by many seconds. Then the thread shows no running
    // turn, unless the CLI is compacting.
    readonly property bool turnEnded: model !== null && !model.turnOpen
                                      && model.pendingEntries.length === 0
    readonly property bool busy: (working && (!turnEnded || screenCompacting
                                              || model.compactingSince > 0))
                                 || pendingCommand.length > 0 || sentAt > 0
    // Compacting the conversation (a /compact, or Claude Code's own when the
    // context fills): a quiet state of its own, never the screen, which
    // still shows the turns before it.
    property bool screenCompacting: false
    readonly property bool compacting: busy && (screenCompacting
                                                || (model !== null && model.compactingSince > 0))
    // The bar sent a slash command; it may open a dialog in the terminal.
    signal commandSent(string text)
    // A dialog already fills the terminal, so nothing was sent.
    signal dialogFound()
    // The bar sent a slash command to a CLI whose dialogs the view cannot
    // see; the window shows that agent's terminal.
    signal terminalNeeded()
    Timer {
        interval: 150
        repeat: true
        running: (root.busy || root.working) && root.visible && !!root.screen && root.model !== null
        triggeredOnStart: true
        onTriggered: {
            const screen = root.screen()
            root.screenCompacting = root.model.compactingShown(screen)
            const rows = root.model.liveLines(screen)
            if (rows.join("\n") !== root.liveRows.join("\n")) {
                root.liveRows = rows
                root.quietTicks = 0
            } else if (!root.working && !root.screenCompacting && root.pendingCommand.length > 0
                       && ++root.quietTicks >= 20) {
                root.pendingCommand = "" // three quiet seconds: it finished unrecorded
            }
        }
        onRunningChanged: if (!running) { root.liveRows = []; root.screenCompacting = false }
    }
    Connections {
        target: root.pendingCommand.length > 0 ? root.model : null
        function onRowsInserted() { root.pendingCommand = "" }
        function onModelReset() { root.pendingCommand = "" }
    }
    onWorkingChanged: if (working) { pendingCommand = ""; sentAt = 0 }
    // The turn a sent prompt started has ended in the file.
    onTurnEndedChanged: if (turnEnded) sentAt = 0
    // The prompt last sent, checked once it should have been taken: an
    // input box still holding it gets one more Return (only then, so a
    // Return never lands in anything else); still there, its bubble says
    // so. The note goes quietly once the box no longer holds it.
    property string checkingText: ""
    property bool retried: false
    property string unsentText: ""
    function checkSent() {
        const holds = root.checkingText.length > 0 && root.screen && root.model && !root.waiting
                      && !root.model.dialogShowing(root.screen())
                      && root.model.composerHolds(root.screen(), root.checkingText)
        if (!holds) {
            root.checkingText = ""
            return
        }
        if (!root.retried && root.pressReturn) {
            root.retried = true
            root.pressReturn()
            sendCheck.restart()
            return
        }
        root.unsentText = root.checkingText
        root.checkingText = ""
        root.sentAt = 0
        unsentCheck.restart()
    }
    property int sendCheckMs: 1500 // shorter in the scripted checks
    Timer { id: sendCheck; interval: root.sendCheckMs; onTriggered: root.checkSent() }
    // An agent that never reports working leaves the starting state.
    Timer { interval: 20000; running: root.sentAt > 0; onTriggered: root.sentAt = 0 }
    Timer {
        id: unsentCheck
        interval: 60000
        onTriggered: {
            if (root.unsentText.length > 0 && root.screen && root.model
                    && root.model.composerHolds(root.screen(), root.unsentText))
                restart()
            else
                root.unsentText = ""
        }
    }
    Connections {
        target: root.model
        function onPendingChanged() {
            if (root.unsentText.length > 0
                    && root.model.pendingPrompts.indexOf(root.unsentText.replace(/\s+/g, " ").trim()) < 0)
                root.unsentText = ""
        }
    }
    // Subagents and background tasks still running (model.running), and
    // whether they keep the agent's work going after its turn ended.
    readonly property var runningWork: model ? model.running : []
    readonly property bool waitingOnWork: !busy && runningWork.length > 0
    // The clock the running turn's elapsed time follows; it also keeps the
    // running tasks current (their files, their silence).
    property double now: Date.now()
    property double turnSince: 0
    Timer {
        interval: 1000
        repeat: true
        running: (root.busy || root.runningWork.length > 0) && root.visible
        triggeredOnStart: true
        onTriggered: {
            if (root.model)
                root.model.tick()
            root.now = Date.now()
            root.turnSince = root.working ? (root.model ? root.model.runningSince() : 0)
                           : root.pendingCommand.length > 0 ? root.pendingSince : root.sentAt
        }
    }
    // The agent's folder, for its project's own slash commands, its CLI
    // (a harness id), and whether lapis knows that CLI's slash commands
    // (Claude Code's, Codex's and Antigravity's).
    property string folder: ""
    property string harness: ""
    property bool slashCommands: false

    // Slash commands: typing "/" and a few letters lists the three best
    // matches above the bar. The list is read once per CLI and agent folder.
    property var commandList: []
    property string commandFolder: "\u0000"
    property int slashIndex: 0
    property bool slashDismissed: false
    readonly property string slashQuery: {
        const text = input.text
        return slashCommands && text.startsWith("/") && !/\s/.test(text) ? text.slice(1).toLowerCase() : "\u0000"
    }
    function loadSlashCommands() {
        const key = harness + "\u0001" + folder
        if (commandFolder !== key && model) {
            commandList = model.slashCommands(harness, folder)
            commandFolder = key
        }
    }
    onSlashQueryChanged: {
        slashIndex = 0
        if (slashQuery === "\u0000")
            slashDismissed = false
        else
            loadSlashCommands()
    }
    // Whether a command sent to a CLI whose screen the view cannot read
    // shows its answer only in the terminal: one its list marks so, or one
    // lapis does not know.
    function answersInTerminal(name) {
        if (!slashCommands)
            return true
        loadSlashCommands()
        const known = commandList.find(c => c.name === name)
        return !known || known.terminal !== false
    }
    readonly property var slashMatches: slashQuery === "\u0000" || slashDismissed ? [] : rankSlash(slashQuery)
    function slashScore(name, description, q) {
        if (q.length === 0)
            return ["compact", "model", "effort", "context", "clear", "resume"].indexOf(name) >= 0
                   ? 10 - ["compact", "model", "effort", "context", "clear", "resume"].indexOf(name) : -1
        if (name === q) return 1000
        if (name.startsWith(q)) return 800 - name.length
        const at = name.indexOf(q)
        if (at >= 0) return 600 - at * 4 - name.length
        // Letters in order ("rp" finds reload-plugins), tighter is better.
        let i = 0, gaps = 0, last = -1
        for (let k = 0; k < name.length && i < q.length; ++k)
            if (name[k] === q[i]) { gaps += last < 0 ? k : k - last - 1; last = k; ++i }
        if (i === q.length) return 400 - gaps * 8 - name.length
        return description.toLowerCase().indexOf(q) >= 0 ? 100 : -1
    }
    function rankSlash(q) {
        const scored = []
        for (const c of commandList) {
            const score = slashScore(c.name, c.description || "", q)
            if (score >= 0)
                scored.push({ score: score, command: c })
        }
        scored.sort((a, b) => b.score - a.score)
        return scored.slice(0, 3).map(s => s.command)
    }
    function completeSlash(name) {
        input.text = "/" + name + " "
        input.cursorPosition = input.text.length
        input.forceActiveFocus()
    }

    // The bar's line editing, as the terminal does it on a Mac: Command
    // moves or deletes to the line's start or end (Up/Down: the whole text),
    // Option by word, and Control-A/E/K/U/W as the shells' emacs keys. Shift
    // extends the selection. Returns whether the key was handled.
    function wordChar(c) { return /\w/.test(c) || c.charCodeAt(0) > 127 }
    function wordBack(text, at) {
        while (at > 0 && !wordChar(text[at - 1])) --at
        while (at > 0 && wordChar(text[at - 1])) --at
        return at
    }
    function wordForward(text, at) {
        while (at < text.length && !wordChar(text[at])) ++at
        while (at < text.length && wordChar(text[at])) ++at
        return at
    }
    function lineStart(text, at) { return text.lastIndexOf("\n", at - 1) + 1 }
    function lineEnd(text, at) { const end = text.indexOf("\n", at); return end < 0 ? text.length : end }
    function editKey(field, key, mods) {
        const text = field.text
        const at = field.cursorPosition
        const command = (mods & ~Qt.ShiftModifier) === Qt.MetaModifier
        const option = (mods & ~Qt.ShiftModifier) === Qt.AltModifier
        const control = mods === Qt.ControlModifier
        const shift = (mods & Qt.ShiftModifier) !== 0
        let target = -1
        if (command && key === Qt.Key_Left) target = lineStart(text, at)
        else if (command && key === Qt.Key_Right) target = lineEnd(text, at)
        else if (command && key === Qt.Key_Up) target = 0
        else if (command && key === Qt.Key_Down) target = text.length
        else if (option && key === Qt.Key_Left) target = wordBack(text, at)
        else if (option && key === Qt.Key_Right) target = wordForward(text, at)
        else if (control && key === Qt.Key_A) target = lineStart(text, at)
        else if (control && key === Qt.Key_E) target = lineEnd(text, at)
        if (target >= 0) {
            if (shift)
                field.moveCursorSelection(target, TextEdit.SelectCharacters)
            else
                field.cursorPosition = target
            return true
        }
        // Deleting: a selection goes whole; otherwise the span the key names.
        let from = -1, to = -1
        if (command && !shift && key === Qt.Key_Backspace) { from = lineStart(text, at); to = at }
        else if (command && !shift && key === Qt.Key_Delete) { from = at; to = lineEnd(text, at) }
        else if (option && !shift && key === Qt.Key_Backspace) { from = wordBack(text, at); to = at }
        else if (option && !shift && key === Qt.Key_Delete) { from = at; to = wordForward(text, at) }
        else if (control && key === Qt.Key_U) { from = lineStart(text, at); to = at }
        else if (control && key === Qt.Key_K) { from = at; to = lineEnd(text, at) }
        else if (control && key === Qt.Key_W) { from = wordBack(text, at); to = at }
        else return false
        if (field.selectedText.length > 0 && (command || option)) {
            field.remove(field.selectionStart, field.selectionEnd)
        } else if (to > from) {
            field.remove(from, to)
        } else if (to === from && from === at && (command || control)) {
            // At a line's start (or end), Command-Backspace (or -Delete)
            // joins it with the line before (or after), as on macOS.
            if (key === Qt.Key_Backspace || key === Qt.Key_U) { if (at > 0) field.remove(at - 1, at) }
            else if (at < text.length) field.remove(at, at + 1)
        }
        return true
    }
    readonly property alias composer: input
    // The same controls for every CLI: the models its agent can switch to
    // ({id, name}), its effort levels, and what it reported last (from the
    // model). The window turns a choice into that CLI's own command.
    property var models: []
    property var efforts: []
    signal modelChosen(string id)
    signal effortChosen(string level)
    // Files to send with the next message, as local paths.
    property var attachments: []
    function attach(path) {
        if (path.length === 0 || attachments.indexOf(path) >= 0)
            return
        attachments = attachments.concat([path])
    }
    function detach(path) { attachments = attachments.filter(p => p !== path) }
    // Short model names, as the apps show them: "Opus 5.5", "Fable 5.1".
    function modelLabel(id) {
        for (let i = 0; i < models.length; ++i)
            if (models[i].id === id && models[i].name)
                return models[i].name
        const m = /^claude-([a-z]+)-(\d+)(?:-(\d+))?/.exec(id || "")
        if (m)
            return m[1].charAt(0).toUpperCase() + m[1].slice(1) + " " + m[2] + (m[3] ? "." + m[3] : "")
        return id || qsTr("Model")
    }
    // An effort level as the Codex app names it: "xhigh" is "Extra High".
    function effortLabel(level) {
        return level === "xhigh" ? qsTr("Extra High")
             : level.length > 0 ? level.charAt(0).toUpperCase() + level.slice(1) : ""
    }
    // "12s", "3m 12s", "1h 4m", "2d 3h": the scale follows the length.
    function durationLabel(ms) {
        const s = Math.max(0, Math.round(ms / 1000))
        return s < 60 ? s + "s"
             : s < 3600 ? Math.floor(s / 60) + "m " + (s % 60) + "s"
             : s < 86400 ? Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m"
             : Math.floor(s / 86400) + "d " + Math.floor((s % 86400) / 3600) + "h"
    }
    // The quiet line under a prompt: queued, or read mid-turn (steered).
    function promptMark(entry) {
        if (!entry)
            return ""
        if (entry.queued)
            return qsTr("Queued · the agent reads it at its next step")
        if (!entry.steered)
            return ""
        return entry.waited < 0 ? qsTr("Steered")
             : entry.waited === 0 ? qsTr("Steered · read at the next step")
             : entry.waited === 1 ? qsTr("Steered · read after 1 more step")
             : qsTr("Steered · read after %1 more steps").arg(entry.waited)
    }
    // A step as one line: its verb and object ("Read greet.cpp", "Ran
    // ctest"), a file by its name only; the path shows once it is opened.
    function stepTarget(entry) {
        if (!entry)
            return ""
        if (entry.kind === "reply")
            return entry.body.trim().split("\n")[0]
        if (entry.kind !== "tool")
            return ""
        const first = entry.body.split("\n")[0]
        const file = ["Read", "Edited", "Wrote", "Deleted", "Viewed image"].indexOf(entry.title) >= 0
        return file && first.indexOf("/") >= 0 ? first.slice(first.lastIndexOf("/") + 1) : first
    }
    // A thought's first sentence, as plain words (its markdown marks gone),
    // for the one dim line it folds to; empty when it has no text.
    function thoughtLine(text) {
        const plain = (text || "").replace(/[*_`#>]+/g, "").trim()
        const line = plain.split("\n")[0].trim()
        const sentence = /^(.+?[.!?])(\s|$)/.exec(line)
        return sentence ? sentence[1] : line
    }
    // A thought's text styled as markdown, a step smaller than replies.
    readonly property var thoughtStyle: ({ sans: sansFamily, mono: monoFamily, size: stepSize,
                                           codeSize: codeSize - 1, codeColor: codeColor,
                                           linkColor: accentColor, inlineCodeColor: inlineCodeColor,
                                           inlineCodeTextColor: mutedTextColor,
                                           borderColor: borderColor, quoteColor: mutedTextColor })
    // Subagents and background tasks (a step's `task`): what follows the
    // step's target on its line, " · "-joined: its type and model (in the
    // running strip), how long it ran or has run, its steps or how it ended.
    function taskParts(task, withKind) {
        if (!task)
            return []
        const parts = []
        if (withKind && task.type === "subagent") {
            const kind = [task.agentType, task.model ? modelLabel(task.model) : ""]
                         .filter(s => s && s.length > 0).join(" / ")
            if (kind.length > 0)
                parts.push(kind)
        }
        const ms = task.running ? now - task.since : task.duration
        if (ms >= 1000)
            parts.push(durationLabel(ms))
        if (task.type === "subagent" && task.steps >= 0)
            parts.push(task.steps === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(task.steps))
        if (!task.running) {
            if (task.stale)
                parts.push(qsTr("no end recorded"))
            else if (task.status === "failed" && task.exit < 0)
                parts.push(qsTr("failed"))
            else if (task.status === "stopped" || task.status === "killed")
                parts.push(qsTr("stopped"))
            if (task.exit > 0)
                parts.push(qsTr("exit %1").arg(task.exit))
        }
        return parts
    }
    function taskLine(task, withKind) {
        const parts = taskParts(task, withKind)
        return parts.length > 0 ? " · " + parts.join(" · ") : ""
    }
    // "2 subagents and 1 background task" (monitors count as background).
    function workCount(list) {
        let agents = 0, other = 0
        for (let i = 0; i < list.length; ++i)
            list[i].type === "subagent" ? ++agents : ++other
        const said = []
        if (agents > 0)
            said.push(agents === 1 ? qsTr("1 subagent") : qsTr("%1 subagents").arg(agents))
        if (other > 0)
            said.push(other === 1 ? qsTr("1 background task") : qsTr("%1 background tasks").arg(other))
        return said.join(qsTr(" and "))
    }
    // How an opened task stands: "Running · 3m 12s · 14 steps", "Finished
    // after 4m 10s · 23 steps · 54k tokens", "Exited with code 1 after 12s".
    function taskState(task) {
        if (!task)
            return ""
        const parts = []
        const ms = task.running ? now - task.since : task.duration
        const took = ms >= 1000 ? durationLabel(ms) : ""
        if (task.running)
            parts.push(took.length > 0 ? qsTr("Running for %1").arg(took) : qsTr("Running"))
        else if (task.stale)
            parts.push(qsTr("No end recorded"))
        else if (task.exit >= 0)
            parts.push(took.length > 0 ? qsTr("Exited with code %1 after %2").arg(task.exit).arg(took)
                                       : qsTr("Exited with code %1").arg(task.exit))
        else if (task.status === "failed")
            parts.push(took.length > 0 ? qsTr("Failed after %1").arg(took) : qsTr("Failed"))
        else if (task.status === "stopped" || task.status === "killed")
            parts.push(took.length > 0 ? qsTr("Stopped after %1").arg(took) : qsTr("Stopped"))
        else
            parts.push(took.length > 0 ? qsTr("Finished after %1").arg(took) : qsTr("Finished"))
        if (task.type === "subagent") {
            if (task.steps >= 0)
                parts.push(task.steps === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(task.steps))
            if (task.tokens > 0)
                parts.push(qsTr("%1 tokens").arg(tokensLabel(task.tokens)))
            if (task.agentType.length > 0)
                parts.push(task.agentType)
            if (task.model.length > 0)
                parts.push(modelLabel(task.model))
        } else if (task.checks > 0) {
            parts.push(task.type === "monitor"
                       ? (task.checks === 1 ? qsTr("1 event") : qsTr("%1 events").arg(task.checks))
                       : (task.checks === 1 ? qsTr("checked once") : qsTr("checked %1 times").arg(task.checks)))
        }
        return parts.join(" · ")
    }
    function tokensLabel(n) {
        return n >= 1000000 ? (n / 1000000).toFixed(n % 1000000 === 0 ? 0 : 1) + "M"
             : n >= 1000 ? Math.round(n / 1000) + "k" : String(n)
    }

    // Escape asks to go back to the terminal.
    signal dismissed()
    // Tab put the suggestion in the bar, and a second Tab sent it unchanged.
    signal suggestionFilled()
    signal suggestionSent()
    // Start a new agent from just before a prompt, with it in the input.
    signal editRequested(string record, string text)

    readonly property var fonts: model ? model.fonts() : ({ serif: "Georgia", mono: "Menlo" })
    // Inspired by the Codex and Claude desktop apps; "terminal" takes the
    // shown terminal's own background so the thread sits where it did.
    readonly property var themes: [
        { name: "terminal", label: qsTr("Match terminal"), dark: true },
        { name: "tokyo-night", label: "Tokyo Night", dark: true, background: "#1a1b26", raised: "#24283b",
          code: "#16161e", border: "#292e42", text: "#c0caf5", muted: "#737aa2", accent: "#7aa2f7",
          added: "#449dab", removed: "#914c54", fault: "#f7768e" },
        { name: "codex", label: "Codex", dark: true, background: "#181818", raised: "#2a2a2a",
          code: "#111111", border: "#303030", text: "#ececec", muted: "#8f8f8f", accent: "#339cff",
          added: "#3fb950", removed: "#f85149", fault: "#ff6b6b" },
        { name: "claude", label: "Claude", dark: true, background: "#262624", raised: "#30302e",
          code: "#1f1e1d", border: "#3e3e38", text: "#e8e6dc", muted: "#a6a39a", accent: "#d97757",
          added: "#7fa86a", removed: "#c46a5b", fault: "#e0705f" },
        { name: "graphite", label: qsTr("Graphite"), dark: true, background: "#1e1f22", raised: "#2b2d31",
          code: "#17181a", border: "#35373c", text: "#dcdee3", muted: "#8b8f98", accent: "#5e9cff",
          added: "#4fb57a", removed: "#d0605e", fault: "#ef6b73" },
        { name: "black", label: qsTr("Black"), dark: true, background: "#000000", raised: "#161616",
          code: "#0b0b0b", border: "#262626", text: "#e6e6e6", muted: "#8a8a8a", accent: "#7aa2f7",
          added: "#4fb57a", removed: "#d0605e", fault: "#ef6b73" },
        // Light themes end in "-light" (Config tells the kinds apart by it).
        // Muted text and links keep 4.5:1 on the page and the bubbles.
        { name: "claude-light", label: qsTr("Claude light"), dark: false, background: "#faf9f5", raised: "#efede4",
          code: "#f1efe7", border: "#e2dfd4", text: "#29261b", muted: "#66635a", accent: "#ad4f2c",
          added: "#2e7d32", removed: "#b3412f", fault: "#b3261e" },
        { name: "codex-light", label: qsTr("Codex light"), dark: false, background: "#ffffff", raised: "#f2f2f2",
          code: "#f7f7f7", border: "#e5e5e5", text: "#0d0d0d", muted: "#6e6e6e", accent: "#0169cc",
          added: "#1a7f37", removed: "#cf222e", fault: "#c62828" }
    ]
    readonly property var readingFonts: [
        { name: "system", label: qsTr("System"), family: systemFamily },
        { name: "serif", label: qsTr("Serif"), family: fonts.serif },
        { name: "avenir", label: "Avenir Next", family: "Avenir Next" },
        { name: "helvetica", label: "Helvetica Neue", family: "Helvetica Neue" }
    ]
    readonly property var codeFonts: [
        { name: "system", label: "SF Mono", family: fonts.mono },
        { name: "terminal", label: qsTr("Terminal font"), family: terminalFamily },
        { name: "menlo", label: "Menlo", family: "Menlo" }
    ]
    function pick(choices, name) {
        for (let i = 0; i < choices.length; ++i)
            if (choices[i].name === name)
                return choices[i]
        return choices[0]
    }
    // A theme's full colors; the terminal theme is derived from its background.
    function colorsOf(theme) {
        if (theme.name !== "terminal")
            return theme
        const base = terminalBackground
        return {
            name: theme.name, label: theme.label, dark: true, background: base,
            raised: Qt.tint(base, "#14ffffff"), code: Qt.tint(base, "#38000000"),
            border: Qt.tint(base, "#1effffff"), text: "#d6dbe5", muted: "#8b93a7",
            accent: terminalAccent, added: "#449dab", removed: "#b4545f", fault: "#f7768e"
        }
    }
    readonly property var colors: colorsOf(pick(themes, themeName))
    readonly property color backgroundColor: colors.background
    readonly property color raisedColor: colors.raised
    readonly property color codeColor: colors.code
    readonly property color borderColor: colors.border
    readonly property color textColor: colors.text
    readonly property color mutedTextColor: colors.muted
    readonly property color accentColor: colors.accent
    readonly property color addedColor: colors.added
    readonly property color removedColor: colors.removed
    readonly property color faultColor: colors.fault
    readonly property color activityColor: colors.accent
    readonly property string sansFamily: pick(readingFonts, fontName).family
    readonly property string monoFamily: pick(codeFonts, codeFontName).family
    // One scale for everything, from the body size, as the Codex app keeps
    // its 15/14/12 steps: prose, step rows, code.
    readonly property int fontSize: sizeSetting > 0 ? sizeSetting : Math.max(12, terminalFontSize)
    readonly property int stepSize: Math.round(fontSize * 0.93)
    readonly property int codeSize: Math.round(fontSize * 0.86)
    readonly property int columnWidth: Math.round(fontSize * 48)
    // A message's row of buttons (Copy, Fork, Edit).
    readonly property int actionHeight: Math.round(codeSize * 1.3) + 8
    // Changes whenever prose has to be laid out again.
    readonly property string styleKey: [sansFamily, monoFamily, fontSize, codeSize, codeColor, accentColor,
                                        borderColor, raisedColor, mutedTextColor, inlineCodeColor,
                                        inlineCodeTextColor].join("|")
    readonly property var proseStyle: ({ sans: sansFamily, mono: monoFamily, size: fontSize, codeSize: codeSize,
                                         codeColor: codeColor, linkColor: accentColor,
                                         inlineCodeColor: inlineCodeColor, inlineCodeTextColor: inlineCodeTextColor,
                                         borderColor: borderColor, quoteColor: mutedTextColor })
    // Code inside a sentence stays quiet, as it is read often: a faint step
    // from the page toward the raised surface (never the darker code well)
    // and text one step dimmer than the body's.
    readonly property color inlineCodeColor: Qt.tint(backgroundColor,
                                                     Qt.rgba(raisedColor.r, raisedColor.g, raisedColor.b, 0.45))
    readonly property color inlineCodeTextColor: Qt.tint(textColor,
                                                         Qt.rgba(mutedTextColor.r, mutedTextColor.g, mutedTextColor.b, 0.35))
    // Code takes Tokyo Night's colors, its Day variant on a light code card.
    readonly property bool lightCode: codeColor.r * 0.299 + codeColor.g * 0.587 + codeColor.b * 0.114 > 0.5
    readonly property color codeTextColor: lightCode ? "#3760bf" : "#c0caf5"

    // Charts agents draw follow this theme: lapis keeps a matplotlib style
    // and a palette written from it.
    readonly property string chartKey: [backgroundColor, textColor, mutedTextColor, borderColor].join("|")
    onChartKeyChanged: chartTimer.restart()
    Timer {
        id: chartTimer
        interval: 300
        onTriggered: if (root.model) root.model.writeChartStyle({
            background: root.backgroundColor, text: root.textColor, muted: root.mutedTextColor,
            grid: root.borderColor,
            light: root.backgroundColor.r * 0.299 + root.backgroundColor.g * 0.587 + root.backgroundColor.b * 0.114 > 0.5 })
    }
    onModelChanged: chartTimer.restart()

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function copy(text) { if (model) model.copy(text) }

    Keys.onEscapePressed: dismissed()

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    // Another conversation opens where it was left: at its newest message,
    // or where it was scrolled to; one whose history comes in later stays on
    // the rows it shows. While the rows are replaced the list makes none
    // (no screen below its top, no cache), so after the replacement only
    // the screen it opens on is made; the cache fills in after a few frames.
    Connections {
        target: root.model
        function onLeaving() {
            const top = list.indexAt(list.width / 2, list.contentY + 1)
            const item = top >= 0 ? list.itemAtIndex(top) : null
            root.model.keepView(top, item ? list.contentY - item.y : 0,
                                root.following || item === null)
            root.following = false // placeView decides; the reset's count change must not
            list.cacheBuffer = 0
            list.anchors.bottom = undefined
            list.height = 0
        }
        function onPathChanged() { root.placeView() }
        function onReloaded() { root.placeView() }
    }
    function placeView() {
        const view = model.view()
        following = view.following
        // Placed while it has no height, then given its height back: the
        // rows the screen shows are made, and none it does not.
        const place = () => {
            if (following) {
                list.positionViewAtEnd()
            } else {
                list.positionViewAtIndex(view.row, ListView.Beginning)
                list.contentY += view.offset
            }
        }
        place()
        list.anchors.bottom = composerArea.top
        place()
        listCache.restart()
        if (following)
            Qt.callLater(list.positionViewAtEnd) // once rows have their heights
    }
    Timer { id: listCache; interval: 250; onTriggered: list.cacheBuffer = 2000 }

    ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: composerArea.top
        model: root.model
        clip: true
        // Dragging selects text; the wheel, trackpad and scroll bar scroll.
        interactive: false
        reuseItems: true
        cacheBuffer: 2000
        header: Item { height: 24 }
        footer: liveFooter
        onCountChanged: if (root.following) Qt.callLater(positionViewAtEnd)
        onContentHeightChanged: if (root.following) Qt.callLater(positionViewAtEnd)
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            // Following would pull the view back to the end mid-drag.
            onPressedChanged: root.following = !pressed && list.atYEnd
        }

        // One delegate per kind, so a row scrolled out is reused by the next
        // row of the same kind with its parts already made.
        delegate: DelegateChooser {
            role: "kind"
            DelegateChoice { roleValue: "prompt"; delegate: Entry { block: promptBlock } }
            DelegateChoice { roleValue: "reply"; delegate: Entry { block: replyBlock } }
            DelegateChoice { roleValue: "tool"; delegate: Entry { block: stepBlock } }
            DelegateChoice { roleValue: "thinking"; delegate: Entry { block: stepBlock } }
            DelegateChoice { roleValue: "turn"; delegate: Entry { block: turnBlock } }
            DelegateChoice { roleValue: "command"; delegate: Entry { block: commandBlock } }
            DelegateChoice { roleValue: "notice"; delegate: Entry { block: noticeBlock } }
            DelegateChoice { roleValue: "divider"; delegate: Entry { block: dividerBlock } }
            DelegateChoice { roleValue: "work"; delegate: Entry { block: workBlock } }
            DelegateChoice { roleValue: "live"; delegate: Entry { block: liveBlock } }
            DelegateChoice { delegate: Entry { block: otherBlock } }
        }
    }

    // One row of the thread: the item's roles, its gap above, and its block.
    component Entry: Item {
        id: row
        required property int index
        required property string kind
        required property string title
        required property string body
        required property string detail
        required property string result
        required property var diff
        required property int added
        required property int removed
        required property int resultLines
        required property bool failed
        required property bool answered
        required property bool afterStep
        required property string record
        required property bool expanded
        required property bool inWork
        required property int steps
        required property bool queued
        required property double at
        required property double duration
        required property bool steered
        required property int waited
        required property int item
        required property var task
        property Component block

        readonly property int gapAbove: index === 0 ? 0
                                      : afterStep ? 2
                                      : inWork ? Math.round(root.fontSize * 0.4)
                                      : kind === "prompt" ? (steered ? root.fontSize : root.fontSize * 2)
                                      : kind === "work" || kind === "live" ? Math.round(root.fontSize * 0.9)
                                      : kind === "turn" ? 10 : root.fontSize
        // Opened work sits indented under its "Worked for" row, on a rule.
        readonly property int indent: inWork ? Math.round(root.fontSize * 0.9) : 0
        readonly property int column: Math.min(root.columnWidth, row.width - 56)
        width: ListView.view ? ListView.view.width : 0
        height: loader.height + gapAbove
        // A row the list let go of (index -1), or a live box with no running
        // turn behind it, never draws: Qt can leave a released row where it
        // was, over the rows that took its place.
        visible: index >= 0 && (kind !== "live" || root.liveShown)
        // A reused row starts closed, whatever the last item opened.
        ListView.onReused: if (loader.item && loader.item.open !== undefined) loader.item.open = false
        Rectangle {
            visible: row.inWork
            x: (row.width - row.column) / 2 + 3
            width: 1
            height: row.height
            color: root.borderColor
        }
        Loader {
            id: loader
            y: row.gapAbove
            width: row.column - row.indent
            x: (row.width - row.column) / 2 + row.indent
            sourceComponent: row.block
            property var entry: row
        }
    }

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                  : event.angleDelta.y / 120 * root.fontSize * 4
            const top = list.originY
            const bottom = Math.max(top, list.originY + list.contentHeight - list.height)
            list.contentY = Math.max(top, Math.min(bottom, list.contentY - step))
            root.following = list.contentY >= bottom - 2
        }
    }

    // Read-only text that selects with the mouse and copies with Command-C.
    component Selectable: TextEdit {
        readOnly: true
        selectByMouse: true
        persistentSelection: false
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        color: root.textColor
        selectionColor: root.tint(root.accentColor, 0.4)
        selectedTextColor: root.textColor
        font.family: root.sansFamily
        font.pixelSize: root.fontSize
        Keys.onPressed: function(event) {
            if (event.matches(StandardKey.Copy)) {
                if (selectedText.length > 0) {
                    if (textFormat === TextEdit.RichText)
                        root.model.copySelection(textDocument, selectionStart, selectionEnd)
                    else
                        root.copy(selectedText)
                }
                event.accepted = true
            } else if (event.matches(StandardKey.SelectAll)) {
                selectAll()
                event.accepted = true
            } else if (event.text.length > 0 && event.text.charCodeAt(0) >= 32
                       && !(event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                // Typing while reading goes to the bar, as in the Codex app.
                input.forceActiveFocus()
                input.insert(input.cursorPosition, event.text)
                event.accepted = true
            }
        }
        onLinkActivated: link => Qt.openUrlExternally(link)
        // A link underlines while the pointer is on it.
        onHoveredLinkChanged: if (textFormat === TextEdit.RichText && root.model)
                                  root.model.hoverLink(textDocument, hoveredLink)
        HoverHandler {
            id: textHover
            cursorShape: parent.hoveredLink.length > 0
                         || ((point.modifiers & Qt.MetaModifier) && parent.pathUnder(point.position).length > 0)
                         ? Qt.PointingHandCursor : Qt.IBeamCursor
        }
        // The file or folder path under a point, or a link to a local file.
        function pathUnder(p) {
            if (!root.model)
                return ""
            if (hoveredLink.length > 0)
                return /^(file:|\/|~)/.test(hoveredLink) ? hoveredLink : ""
            return root.model.pathAt(textDocument, positionAt(p.x, p.y), root.folder)
        }
        // Command-click opens a path in the chosen editor (a folder in the
        // Finder), as Command-click does in a terminal.
        TapHandler {
            acceptedModifiers: Qt.MetaModifier
            onTapped: function(point) {
                const path = parent.pathUnder(point.position)
                if (path.length > 0)
                    root.model.openPath(path, root.editor)
            }
        }
    }

    // A quiet button on the bar: text that lights on hover, with a tooltip.
    // A drawn chevron, centered on its box rather than on a font's baseline.
    // It points right; `turned` rotates it a quarter turn clockwise (down)
    // and `flipped` half a turn more, with a short motion between.
    component Chevron: Canvas {
        id: chevronShape
        property color color: root.mutedTextColor
        property bool turned: false
        property bool flipped: false
        implicitWidth: Math.round(root.codeSize * 0.75)
        implicitHeight: implicitWidth
        rotation: (turned ? 90 : 0) + (flipped ? 180 : 0)
        Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        onColorChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const g = getContext("2d")
            g.reset()
            const s = Math.min(width, height)
            g.strokeStyle = color
            g.lineWidth = Math.max(1.4, s / 7)
            g.lineCap = "round"
            g.lineJoin = "round"
            g.beginPath()
            g.moveTo(width / 2 - s * 0.15, height / 2 - s * 0.3)
            g.lineTo(width / 2 + s * 0.15, height / 2)
            g.lineTo(width / 2 - s * 0.15, height / 2 + s * 0.3)
            g.stroke()
        }
    }

    component BarButton: Rectangle {
        id: barButton
        property string label
        property int labelSize: root.codeSize
        // A menu button: a chevron after the label, pointing up while its
        // menu is open (menus here open upward).
        property bool menu: false
        property bool menuOpen: false
        signal triggered()
        implicitWidth: barRow.implicitWidth + 16
        implicitHeight: Math.round(root.fontSize * 1.6)
        radius: 8
        color: barMouse.containsMouse || menuOpen ? root.tint(root.textColor, 0.08) : "transparent"
        Row {
            id: barRow
            anchors.centerIn: parent
            spacing: Math.round(root.codeSize * 0.35)
            Text {
                id: barText
                anchors.verticalCenter: parent.verticalCenter
                text: barButton.label
                color: barMouse.containsMouse || barButton.menuOpen ? root.textColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: barButton.labelSize
            }
            Chevron {
                visible: barButton.menu
                anchors.verticalCenter: parent.verticalCenter
                color: barText.color
                turned: true
                flipped: barButton.menuOpen
            }
        }
        MouseArea {
            id: barMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: barButton.triggered()
        }
    }

    // A menu of choices ({id, label}) with the current one marked.
    component ChoiceMenu: Menu {
        id: choiceMenu
        property var choices: []
        property string current: ""
        signal chosen(string id)
        // As wide as its longest label, so none is cut short.
        TextMetrics {
            id: widest
            font.family: root.sansFamily
            font.pixelSize: root.codeSize
            text: choiceMenu.choices.reduce((a, c) => c.label.length > a.length ? c.label : a, "")
        }
        width: Math.max(140, Math.ceil(widest.advanceWidth) + Math.round(root.codeSize * 2.6) + 24)
        background: Rectangle {
            radius: 10
            color: root.raisedColor
            border.color: root.borderColor
        }
        Instantiator {
            model: choiceMenu.choices
            delegate: MenuItem {
                id: choiceItem
                required property var modelData
                text: modelData.label
                indicator: null
                arrow: null
                onTriggered: choiceMenu.chosen(modelData.id)
                contentItem: Row {
                    spacing: Math.round(root.codeSize * 0.5)
                    Text {
                        width: Math.round(root.codeSize * 1.1)
                        anchors.verticalCenter: parent.verticalCenter
                        text: choiceItem.modelData.id === choiceMenu.current ? "✓" : ""
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: choiceItem.text
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                    }
                }
                background: Rectangle {
                    radius: 6
                    color: choiceItem.highlighted ? root.tint(root.textColor, 0.1) : "transparent"
                }
            }
            onObjectAdded: (index, object) => choiceMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => choiceMenu.removeItem(object)
        }
    }

    FolderDialog {
        id: folderPicker
        title: qsTr("Attach a folder")
        onAccepted: root.attach(root.model ? root.model.localPath(selectedFolder) : "")
    }
    FileDialog {
        id: filePicker
        title: qsTr("Attach files")
        fileMode: FileDialog.OpenFiles
        onAccepted: selectedFiles.forEach(url => root.attach(root.model ? root.model.localPath(url) : ""))
    }

    // Files dropped anywhere on the thread attach to the next message.
    DropArea {
        id: dropArea
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: function(drop) {
            if (!root.model)
                return
            drop.urls.forEach(url => root.attach(root.model.localPath(url)))
            drop.acceptProposedAction()
            input.forceActiveFocus()
        }
    }

    // A small labeled button in a message's action row.
    component Action: Rectangle {
        id: action
        property string label
        property bool done: false
        signal triggered()
        function flash() { done = true; doneTimer.restart() }
        implicitWidth: actionText.implicitWidth + 16
        implicitHeight: actionText.implicitHeight + 8
        radius: 6
        color: actionMouse.containsMouse ? root.raisedColor : "transparent"
        Text {
            id: actionText
            anchors.centerIn: parent
            text: action.done ? qsTr("Copied") : action.label
            color: actionMouse.containsMouse ? root.textColor : root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.codeSize
        }
        MouseArea {
            id: actionMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.triggered()
        }
        Timer { id: doneTimer; interval: 1200; onTriggered: action.done = false }
    }

    // What a shown file does beyond a click (the window's MediaActions:
    // Finder, clipboard, save, drag out); null where there is none, as in a
    // capture, and the menu then offers only Open.
    property var mediaActions: null
    // A light page (light mode's themes), for pictures drawn for the other.
    readonly property bool lightPage: backgroundColor.hslLightness > 0.5
    function openMedia(media) {
        if (!media)
            return
        if (!mediaActions || !mediaActions.open(media.path))
            Qt.openUrlExternally(media.url)
    }
    function isPicture(media) {
        return media !== null && media !== undefined
               && (media.kind === "image" || media.kind === "svg" || media.kind === "animation")
    }

    // Files a message names, shown in place: images, SVG and GIFs drawn at
    // their size (up to the column), audio and video played in place, other
    // files as a card that opens them. Click an image to open it full size;
    // drag any of them out to another app, or right-click for the rest.
    component MediaList: Column {
        id: mediaList
        property var items: []
        property bool alignRight: false
        spacing: 8
        visible: items.length > 0
        Repeater {
            model: mediaList.items
            delegate: Item {
                id: mediaCell
                objectName: "mediaItem"
                required property var modelData
                readonly property var media: modelData
                anchors.right: mediaList.alignRight ? parent.right : undefined
                width: shown.item ? shown.item.width : 0
                height: shown.item ? shown.item.height : 0
                Loader {
                    id: shown
                    readonly property var media: mediaCell.media
                    property real room: mediaList.width
                    sourceComponent: mediaCell.modelData.kind === "image" || mediaCell.modelData.kind === "svg" ? imageMedia
                                   : mediaCell.modelData.kind === "animation" ? animationMedia
                                   : mediaCell.modelData.kind === "audio" || mediaCell.modelData.kind === "video" ? playbackMedia
                                   : fileMedia
                }
                MediaGrip {
                    anchors.fill: parent
                    media: mediaCell.media
                    // A player takes its own clicks (play, seek); its fallback
                    // card opens the file as the others do.
                    plays: shown.item !== null && shown.item.item !== undefined && shown.item.item !== null
                           && typeof shown.item.item.toggle === "function"
                    seeking: plays && shown.item.item.seeking === true
                }
            }
        }
    }
    // Over a shown file: a click opens it (not over a player), a drag takes
    // the file out of the window, and a right-click or Control-click opens
    // its menu. A drag never also counts as the click.
    component MediaGrip: Item {
        id: grip
        property var media: null
        property bool plays: false
        // A player's line being dragged to seek, which is not a file drag.
        property bool seeking: false
        readonly property string path: media ? media.path : ""
        // A press moved past the drag distance starts the file drag. The
        // handler only watches (it never takes the press), so the player's
        // own buttons still work and a press that stays put is a click.
        PointHandler {
            id: gripPoint
            acceptedButtons: Qt.LeftButton
            property bool started: false
            onActiveChanged: if (!active) started = false
            onPointChanged: {
                if (!active || started || !root.mediaActions || grip.path.length === 0 || grip.seeking)
                    return
                const moved = Math.hypot(point.position.x - point.pressPosition.x,
                                         point.position.y - point.pressPosition.y)
                if (moved < Application.styleHints.startDragDistance)
                    return
                started = true
                // Runs until the drop; macOS then sends the release the drop
                // consumed, which ends this press.
                root.mediaActions.drag(grip.path, root.isPicture(grip.media), grip)
            }
        }
        HoverHandler {
            enabled: !grip.plays
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            enabled: !grip.plays
            acceptedButtons: Qt.LeftButton
            acceptedModifiers: Qt.NoModifier
            onTapped: root.openMedia(grip.media)
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function(point) { root.showMediaMenu(grip.media, grip, point.position) }
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            acceptedModifiers: Qt.ControlModifier
            onTapped: function(point) { root.showMediaMenu(grip.media, grip, point.position) }
        }
    }
    // One menu for whichever file was right-clicked, in the bar menus' style.
    component MediaMenuItem: MenuItem {
        id: mediaItem
        indicator: null
        arrow: Chevron {
            visible: mediaItem.subMenu !== null
            x: mediaItem.width - width - 10
            anchors.verticalCenter: parent.verticalCenter
            color: root.mutedTextColor
        }
        contentItem: Text {
            leftPadding: 6
            text: mediaItem.text
            color: mediaItem.enabled ? root.textColor : root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.codeSize
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6
            color: mediaItem.highlighted ? root.tint(root.textColor, 0.1) : "transparent"
        }
    }
    component MediaMenu: Menu {
        id: mediaMenu
        property var media: null
        readonly property string path: media ? media.path : ""
        readonly property bool picture: root.isPicture(media)
        readonly property bool acts: root.mediaActions !== null && path.length > 0
        function show(media, item, at) {
            mediaMenu.media = media
            mediaMenu.popup(item, at.x, at.y)
        }
        width: Math.max(180, Math.round(root.codeSize * 15))
        background: Rectangle {
            radius: 10
            color: root.raisedColor
            border.color: root.borderColor
        }
        // The item that opens Open With.
        delegate: MediaMenuItem {}
        MediaMenuItem {
            objectName: "mediaOpen"
            text: qsTr("Open")
            onTriggered: root.openMedia(mediaMenu.media)
        }
        Menu {
            id: openWith
            title: qsTr("Open With")
            // Only for pictures, with the apps Command-click opens paths in.
            enabled: mediaMenu.picture && root.model !== null
            width: Math.max(160, Math.round(root.codeSize * 12))
            background: Rectangle {
                radius: 10
                color: root.raisedColor
                border.color: root.borderColor
            }
            delegate: MediaMenuItem {}
            Instantiator {
                model: openWith.enabled && mediaMenu.opened ? root.model.editors() : []
                delegate: MediaMenuItem {
                    required property var modelData
                    text: modelData.label
                    onTriggered: root.model.openPath(mediaMenu.path, modelData.id)
                }
                onObjectAdded: (index, object) => openWith.insertItem(index, object)
                onObjectRemoved: (index, object) => openWith.removeItem(object)
            }
        }
        MediaMenuItem {
            objectName: "mediaReveal"
            text: qsTr("Show in Finder")
            enabled: mediaMenu.acts
            onTriggered: root.mediaActions.reveal(mediaMenu.path)
        }
        MenuSeparator {
            contentItem: Rectangle { implicitHeight: 1; color: root.borderColor }
        }
        MediaMenuItem {
            objectName: "mediaCopy"
            text: mediaMenu.picture ? qsTr("Copy Image") : qsTr("Copy File")
            enabled: mediaMenu.acts
            onTriggered: mediaMenu.picture ? root.mediaActions.copyImage(mediaMenu.path)
                                           : root.mediaActions.copyFile(mediaMenu.path)
        }
        MediaMenuItem {
            objectName: "mediaCopyPath"
            text: qsTr("Copy Path")
            enabled: mediaMenu.acts
            onTriggered: root.mediaActions.copyPath(mediaMenu.path)
        }
        MediaMenuItem {
            objectName: "mediaSaveAs"
            text: qsTr("Save As…")
            enabled: mediaMenu.acts
            onTriggered: {
                mediaSave.source = mediaMenu.path
                mediaSave.selectedFile = root.mediaActions.saveSuggestion(mediaMenu.path)
                mediaSave.open()
            }
        }
    }
    // Made on the first right-click, so the thread never builds (or lays out
    // again on a theme or font change) a menu nobody opened.
    property var mediaMenu: null
    Component {
        id: mediaMenuComponent
        MediaMenu { objectName: "mediaMenu" }
    }
    function showMediaMenu(media, item, at) {
        if (!mediaMenu)
            mediaMenu = mediaMenuComponent.createObject(root)
        mediaMenu.show(media, item, at)
    }
    FileDialog {
        id: mediaSave
        property string source: ""
        title: qsTr("Save a copy")
        fileMode: FileDialog.SaveFile
        onAccepted: if (root.mediaActions) root.mediaActions.saveCopy(source, selectedFile)
    }
    Component {
        id: imageMedia
        Image {
            id: picture
            readonly property var media: parent ? parent.media : null
            readonly property bool vector: media !== null && media.kind === "svg"
            // Its own size from the file's header, before it is decoded.
            readonly property real ownWidth: media && media.width > 0 ? media.width : implicitWidth
            readonly property real ownHeight: media && media.height > 0 ? media.height : implicitHeight
            source: media ? media.url : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            // Never decoded larger than it can be shown; an SVG is drawn at
            // twice its shown size so it stays sharp on Retina.
            sourceSize.width: vector ? Math.round(width * 2) : parent ? Math.round(parent.room * 2) : 0
            // Large images are taken as Retina (2x), as screenshots and
            // 200 dpi charts are; small ones and SVGs show at their own size.
            width: Math.min(parent ? parent.room : 0, !vector && ownWidth >= 800 ? ownWidth / 2 : ownWidth)
            height: ownWidth > 0 ? width * ownHeight / ownWidth : 0
            // A see-through picture drawn for the other kind of page (white
            // strokes made for a dark thread, now on a light one, or the
            // reverse) sits on a card of that kind, so its lines stay
            // visible; one that suits the page sits on it bare.
            readonly property string ink: media && media.alpha && root.mediaActions
                                          ? root.mediaActions.inkTone(media.path) : ""
            readonly property bool carded: (ink === "light" && root.lightPage) || (ink === "dark" && !root.lightPage)
            Rectangle {
                objectName: "inkCard"
                z: -1
                visible: picture.carded
                anchors.fill: parent
                anchors.margins: -6
                radius: root.radius
                color: root.lightPage ? "#26272b" : "#f3f2ee"
            }
            // A frame round an opaque picture; a see-through one (a diagram
            // of strokes) sits on the thread's own background, unframed.
            Rectangle {
                anchors.fill: parent
                visible: !(parent.media && parent.media.alpha)
                color: "transparent"
                border.color: root.borderColor
                radius: 4
            }
        }
    }
    Component {
        id: animationMedia
        AnimatedImage {
            readonly property var media: parent ? parent.media : null
            source: media ? media.url : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            width: Math.min(parent ? parent.room : 0, implicitWidth)
            height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
        }
    }
    // The player lives in its own file, so the thread still loads where Qt
    // Multimedia is missing; the file then shows as a card.
    Component {
        id: playbackMedia
        Loader {
            id: playbackLoader
            readonly property var media: parent ? parent.media : null
            readonly property real room: parent ? parent.room : 0
            source: Qt.resolvedUrl("MediaPlayback.qml")
            onLoaded: {
                if (typeof item.toggle !== "function")
                    return // the card it fell back to
                item.view = root
                item.media = Qt.binding(() => playbackLoader.media)
                item.room = Qt.binding(() => playbackLoader.room)
            }
            onStatusChanged: if (status === Loader.Error) sourceComponent = fileMedia
        }
    }
    Component {
        id: fileMedia
        Rectangle {
            readonly property var media: parent ? parent.media : null
            width: Math.min(parent ? parent.room : 0, fileRow.implicitWidth + 28)
            height: fileRow.implicitHeight + 18
            radius: root.radius - 2
            color: fileHover.hovered ? root.tint(root.textColor, 0.1) : root.raisedColor
            HoverHandler { id: fileHover }
            Row {
                id: fileRow
                anchors.verticalCenter: parent.verticalCenter
                x: 14
                spacing: 10
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: !parent.parent.media ? "" : ({ audio: "♪", video: "▶", html: "◧", pdf: "▤", svg: "◇" })[parent.parent.media.kind] || "▢"
                    color: root.accentColor
                    font.pixelSize: root.stepSize + 2
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: fileRow.parent.media ? fileRow.parent.media.name : ""
                        color: root.textColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                        elide: Text.ElideMiddle
                        width: Math.min(implicitWidth, root.columnWidth * 0.7)
                    }
                    Text {
                        text: !fileRow.parent.media ? ""
                              : qsTr("%1 · %2 · opens in its app").arg(fileRow.parent.media.type)
                                .arg(root.bytesLabel(fileRow.parent.media.size))
                        color: root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize - 1
                    }
                }
            }
        }
    }
    function bytesLabel(n) {
        return n >= 1e9 ? (n / 1e9).toFixed(1) + " GB" : n >= 1e6 ? (n / 1e6).toFixed(1) + " MB"
             : n >= 1e3 ? Math.round(n / 1e3) + " KB" : n + " B"
    }

    // Code or output on a darker card that can be selected and copied.
    component Code: Rectangle {
        id: code
        property alias text: codeText.text
        property alias textColor: codeText.color
        property string language: ""
        // The label over the code; its language unless set.
        property string caption: language
        // Colored by its language (a reply's fenced code, a command).
        property bool highlighted: false
        readonly property bool hovered: codeHover.hovered
        function rehighlight() {
            if (highlighted && root.model)
                root.model.highlight(codeText.textDocument, language, root.lightCode)
        }
        onLanguageChanged: rehighlight()
        Component.onCompleted: rehighlight()
        Connections {
            target: root
            function onLightCodeChanged() { code.rehighlight() }
        }
        height: codeText.height + (caption.length > 0 ? header.height : 0) + 20
        color: root.codeColor
        radius: root.radius - 2
        HoverHandler { id: codeHover }
        Text {
            id: header
            visible: code.caption.length > 0
            x: 12; y: 8
            text: code.caption
            color: root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.codeSize - 1
        }
        // Made on hover only: scrolling past code builds no buttons.
        Loader {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4
            z: 1
            active: code.hovered
            sourceComponent: Component {
                Action {
                    label: qsTr("Copy")
                    onTriggered: { root.copy(code.text); flash() }
                }
            }
        }
        Selectable {
            id: codeText
            x: 12
            y: 10 + (code.caption.length > 0 ? header.height + 4 : 0)
            width: parent.width - 24
            wrapMode: TextEdit.WrapAnywhere
            font.family: root.monoFamily
            font.pixelSize: root.codeSize
        }
    }

    Component {
        id: promptBlock
        Column {
            id: prompt
            readonly property var entry: parent ? parent.entry : null
            spacing: 4
            HoverHandler { id: promptHover }
            Rectangle {
                id: bubble
                anchors.right: parent.right
                width: Math.min(measure.implicitWidth + 36, parent.width * 0.8)
                height: promptText.height + 22
                radius: Math.round(root.fontSize * 1.1)
                color: root.raisedColor
                // Sent while the agent worked: quieter until it takes it in.
                opacity: prompt.entry && prompt.entry.queued ? 0.6 : 1
                Behavior on opacity { NumberAnimation { duration: 160 } }
                Text {
                    id: measure
                    visible: false
                    text: prompt.entry ? prompt.entry.body : ""
                    font: promptText.font
                    textFormat: Text.PlainText
                }
                Selectable {
                    id: promptText
                    x: 18; y: 11
                    width: parent.width - 36
                    text: prompt.entry ? prompt.entry.body : ""
                }
            }
            // Waiting in the CLI's queue, or read by the agent mid-turn.
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 6
                visible: text.length > 0
                text: root.promptMark(prompt.entry)
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.codeSize - 1
            }
            // Images attached to the prompt, under its bubble.
            MediaList {
                anchors.right: parent.right
                width: parent.width * 0.6
                // Paths written in the prompt, then the images pasted into it.
                items: prompt.entry && root.model
                       ? root.model.media(prompt.entry.body + "\n" + prompt.entry.detail) : []
            }
            // The row's room is kept so hovering never moves the thread; its
            // buttons are made only while the pointer is over the message.
            Item {
                width: parent.width
                height: root.actionHeight
                Loader {
                    anchors.right: parent.right
                    active: promptHover.hovered
                    sourceComponent: Component {
                        Row {
                            spacing: 2
                            Action {
                                label: qsTr("Copy")
                                onTriggered: { root.copy(prompt.entry.body); flash() }
                            }
                            Action {
                                // Only Claude Code conversations fork from a prompt.
                                visible: prompt.entry && prompt.entry.record.length > 0
                                         && root.model !== null && root.model.forkable
                                label: qsTr("Edit")
                                onTriggered: root.editRequested(prompt.entry.record, prompt.entry.body)
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: replyBlock
        Column {
            id: reply
            readonly property var entry: parent ? parent.entry : null
            spacing: Math.round(root.fontSize * 0.6)
            HoverHandler { id: replyHover }
            Repeater {
                model: reply.entry && root.model ? root.model.pieces(reply.entry.body) : []
                delegate: Loader {
                    required property var modelData
                    width: reply.width
                    sourceComponent: modelData.media ? mediaPiece : modelData.code ? codePiece : prosePiece
                    property var piece: modelData
                }
            }
            // Media the reply names in a sentence; a path standing alone
            // already shows in its place above.
            MediaList {
                width: parent.width
                items: reply.entry && root.model ? root.model.replyMedia(reply.entry.body) : []
            }
            Item {
                width: parent.width
                height: root.actionHeight
                Loader {
                    x: -8
                    active: replyHover.hovered
                    sourceComponent: Component {
                        Row {
                            spacing: 2
                            Action {
                                label: qsTr("Copy")
                                onTriggered: { root.copy(reply.entry.body); flash() }
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: prosePiece
        Selectable {
            id: prose
            readonly property string source: parent && parent.piece ? parent.piece.text : ""
            function restyle() {
                if (root.model)
                    root.model.style(textDocument, source, root.proseStyle)
            }
            textFormat: TextEdit.RichText
            onSourceChanged: restyle()
            Component.onCompleted: restyle()
            Connections {
                target: root
                function onStyleKeyChanged() { prose.restyle() }
            }
        }
    }

    // A path standing alone in a reply, shown as its picture (or player or
    // card) where the reply names it.
    Component {
        id: mediaPiece
        MediaList {
            items: parent && parent.piece && parent.piece.media ? [parent.piece.media] : []
        }
    }

    Component {
        id: codePiece
        Code {
            text: parent && parent.piece ? parent.piece.text : ""
            language: parent && parent.piece ? parent.piece.language : ""
            highlighted: true
            textColor: root.codeTextColor
        }
    }

    // A tool call or a stretch of thinking: one quiet line that opens.
    Component {
        id: stepBlock
        Column {
            id: step
            objectName: "step"
            readonly property var entry: parent ? parent.entry : null
            readonly property bool thinking: entry && entry.kind === "thinking"
            // A thought folds to its first sentence, dimmed, in place of "Thought".
            readonly property string thought: thinking ? root.thoughtLine(entry.body) : ""
            readonly property bool edit: entry && entry.diff.length > 0
            readonly property bool command: entry && entry.title === "Ran"
            // A subagent or background task this step started.
            readonly property var task: entry && entry.task ? entry.task : null
            readonly property bool running: entry && !thinking && (task ? task.running : !entry.answered)
            readonly property bool expandable: entry && (thinking ? entry.body.length > 0
                                               : task !== null || entry.result.length > 0 || entry.detail.length > 0
                                                 || edit || command || entry.body.indexOf("\n") >= 0)
            property bool open: false
            spacing: 6

            Item {
                width: parent.width
                height: Math.max(verb.implicitHeight, root.stepSize + 8)
                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: step.expandable
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: step.open = !step.open
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    spacing: 6
                    Rectangle {
                        visible: step.running
                        width: 6; height: 6; radius: 3
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.activityColor
                    }
                    Text {
                        id: verb
                        visible: text.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        // "Subagent · <description>", "Started in background · <command>"
                        text: !step.entry || step.thought.length > 0 ? ""
                              : step.entry.title + (step.task ? " ·" : "")
                        color: step.entry && step.entry.failed ? root.faultColor
                             : hover.containsMouse ? root.textColor : root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                    }
                    Text {
                        id: target
                        visible: text.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width - verb.width - counts.width - took.width
                                                       - taskInfo.width - chevron.width - 42)
                        text: step.thinking ? step.thought
                              : step.entry ? step.entry.body.split("\n")[0] : ""
                        elide: Text.ElideRight
                        color: step.thinking ? (hover.containsMouse ? root.textColor : root.tint(root.mutedTextColor, 0.9))
                             : step.entry && step.entry.failed ? root.tint(root.faultColor, 0.85) : root.tint(root.textColor, 0.82)
                        // A thought, a subagent's description and what a monitor
                        // watches read as prose.
                        readonly property bool prose: step.thinking
                                                      || (!!step.task && step.task.type !== "background")
                        font.family: prose ? root.sansFamily : root.monoFamily
                        font.pixelSize: prose ? root.stepSize : root.codeSize
                    }
                    // A task's run: "· 4m 10s · 23 steps".
                    Text {
                        id: taskInfo
                        visible: step.task !== null
                        anchors.verticalCenter: parent.verticalCenter
                        text: visible ? root.taskLine(step.task, false).slice(1) : ""
                        color: root.tint(root.mutedTextColor, 0.75)
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize - 2
                        font.features: { "tnum": 1 }
                    }
                    Row {
                        id: counts
                        visible: step.edit
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text { text: step.entry ? "+" + step.entry.added : ""; color: root.addedColor; font.pixelSize: root.codeSize; font.family: root.monoFamily; visible: step.entry && step.entry.added > 0 }
                        Text { text: step.entry ? "−" + step.entry.removed : ""; color: root.removedColor; font.pixelSize: root.codeSize; font.family: root.monoFamily; visible: step.entry && step.entry.removed > 0 }
                    }
                    // How long the step took, once it took a second or more.
                    Text {
                        id: took
                        // Hidden it has no text, so no width: a Row skips it,
                        // and the target's width can count it either way.
                        visible: step.entry !== null && (!step.thinking || step.thought.length > 0)
                                 && !step.task && step.entry.duration >= 1000
                        anchors.verticalCenter: parent.verticalCenter
                        text: visible ? root.durationLabel(step.entry.duration) : ""
                        color: root.tint(root.mutedTextColor, 0.75)
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize - 2
                        font.features: { "tnum": 1 }
                    }
                    Chevron {
                        id: chevron
                        visible: step.expandable && (hover.containsMouse || step.open)
                        anchors.verticalCenter: parent.verticalCenter
                        turned: step.open
                    }
                }
            }

            // What opens on click is made on click: a collapsed step is one
            // line, an edit included (its diff shows only once opened).
            Loader {
                width: parent.width
                active: step.open
                visible: active
                sourceComponent: Component {
                    StepDetail { entry: step.entry }
                }
            }
        }
    }

    // What an opened step shows: its description, its command, its diff,
    // its output, or its thinking; an interim reply or note in full. It sits
    // a little in from its title row on a thin rule, a small gap under that
    // row and a larger one before the next step, so it reads as that step's.
    component StepDetail: Item {
        id: stepDetail
        property var entry: null
        // Opened inside a subagent's own steps: its subagents stay lines.
        property bool nested: false
        readonly property int gapAbove: 2
        readonly property int gapBelow: Math.round(root.stepSize * 0.8)
        implicitHeight: detail.height + gapAbove + gapBelow
        height: implicitHeight
        Rectangle {
            objectName: "detailRule"
            x: 3
            y: stepDetail.gapAbove
            width: 1
            height: detail.height
            color: root.borderColor
        }
        Column {
        id: detail
        x: Math.round(root.stepSize * 0.85)
        y: stepDetail.gapAbove
        width: stepDetail.width - x
        readonly property var entry: stepDetail.entry
        readonly property bool nested: stepDetail.nested
        readonly property bool thinking: entry !== null && entry.kind === "thinking"
        readonly property bool tool: entry !== null && entry.kind === "tool"
        readonly property bool edit: tool && entry.diff.length > 0
        readonly property bool command: tool && entry.title === "Ran"
        // A subagent or background task shows its run instead (TaskDetail).
        readonly property bool task: tool && !nested && !!entry.task
        spacing: 6
        Loader {
            width: parent.width
            active: detail.task
            visible: active
            sourceComponent: Component {
                TaskDetail { entry: detail.entry }
            }
        }
        Selectable {
            visible: detail.tool && !detail.task && detail.entry.detail.length > 0
            width: parent.width
            text: detail.tool ? detail.entry.detail : ""
            color: root.mutedTextColor
            font.pixelSize: root.stepSize - 1
        }
        Code {
            visible: detail.tool && !detail.edit && !detail.task
                     && (detail.command || detail.entry.body.indexOf("\n") >= 0)
            width: parent.width
            text: detail.tool ? detail.entry.body : ""
            // A command reads as shell, without a label.
            language: detail.command ? "bash" : ""
            caption: ""
            highlighted: detail.command
            textColor: detail.command ? root.codeTextColor : root.textColor
        }
        // An edit's diff, whole, with its path over it.
        Loader {
            width: parent.width
            active: detail.edit
            visible: active
            sourceComponent: Component {
                Rectangle {
                    objectName: "diffCard"
                    height: diffColumn.height + 12
                    radius: root.radius
                    color: root.codeColor
                    clip: true
                    Column {
                        id: diffColumn
                        y: 6
                        width: parent.width
                        Text {
                            x: 12
                            width: parent.width - 24
                            bottomPadding: 4
                            text: detail.entry ? detail.entry.body.split("\n")[0] : ""
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: root.mutedTextColor
                            font.family: root.sansFamily
                            font.pixelSize: root.codeSize - 1
                        }
                        Repeater {
                            model: detail.entry ? detail.entry.diff : []
                            Rectangle {
                                required property var modelData
                                width: diffColumn.width
                                height: diffLine.implicitHeight + 2
                                color: modelData.sign === "+" ? root.tint(root.addedColor, 0.16)
                                     : modelData.sign === "-" ? root.tint(root.removedColor, 0.22)
                                     : "transparent"
                                Text {
                                    id: diffLine
                                    x: 12
                                    width: parent.width - 24
                                    text: modelData.text.length > 0 ? modelData.text : " "
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: root.tint(root.textColor, 0.9)
                                    font.family: root.monoFamily
                                    font.pixelSize: root.codeSize
                                    lineHeight: 1.2
                                }
                            }
                        }
                    }
                }
            }
        }
        Code {
            visible: detail.tool && !detail.task && detail.entry.result.length > 0
            width: parent.width
            text: detail.tool ? detail.entry.result : ""
            textColor: detail.tool && detail.entry.failed ? root.faultColor : root.tint(root.textColor, 0.75)
        }
        Selectable {
            visible: detail.entry !== null && !detail.tool && !detail.thinking
            width: parent.width
            text: !detail.entry ? "" : detail.entry.kind === "notice" || detail.entry.kind === "divider"
                                       ? detail.entry.title : detail.entry.body
            color: root.tint(root.textColor, 0.9)
            font.pixelSize: root.stepSize
        }
        // A thought in full, dim, its markdown laid out as replies are, a
        // step smaller.
        Selectable {
            id: thoughtText
            objectName: "thoughtText"
            readonly property string source: detail.thinking ? detail.entry.body : ""
            function restyle() {
                if (root.model && source.length > 0)
                    root.model.style(textDocument, source, root.thoughtStyle)
            }
            visible: source.length > 0
            width: parent.width
            textFormat: TextEdit.RichText
            color: root.mutedTextColor
            onSourceChanged: restyle()
            Component.onCompleted: restyle()
            Connections {
                target: root
                function onStyleKeyChanged() { thoughtText.restyle() }
            }
        }
        }
    }

    // A step as one line in a box (a subagent's own steps, the running
    // strip): its verb and target, a task's run after them, a dot while it
    // runs. Clicking it asks to open it.
    component StepLine: Item {
        id: stepLine
        property var entry: null
        property bool opened: false
        property bool withKind: false // a subagent's type and model too
        signal clicked()
        readonly property var task: entry && entry.task ? entry.task : null
        readonly property string thought: entry && entry.kind === "thinking" ? root.thoughtLine(entry.body) : ""
        readonly property bool prose: entry !== null && (entry.kind === "reply" || thought.length > 0
                                                         || (task !== null && task.type !== "background"))
        height: root.liveLineHeight
        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            radius: 4
            color: stepLine.opened ? root.tint(root.textColor, 0.07) : "transparent"
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: stepLine.clicked()
        }
        Row {
            anchors.verticalCenter: parent.verticalCenter
            x: 12
            width: parent.width - 24
            spacing: 6
            Rectangle {
                visible: !!stepLine.entry && stepLine.entry.kind === "tool"
                         && (stepLine.task ? stepLine.task.running : !stepLine.entry.answered)
                width: 6; height: 6; radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: root.activityColor
            }
            Text {
                id: stepLineVerb
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: !stepLine.entry || stepLine.entry.kind === "reply" || stepLine.thought.length > 0 ? ""
                      : stepLine.entry.title + (stepLine.task ? " ·" : "")
                color: stepLine.entry && stepLine.entry.failed ? root.faultColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
            }
            Text {
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - stepLineVerb.width - stepLineInfo.width - 24)
                text: stepLine.thought.length > 0 ? stepLine.thought : root.stepTarget(stepLine.entry)
                elide: Text.ElideRight
                textFormat: Text.PlainText
                color: stepLine.thought.length > 0 ? root.tint(root.mutedTextColor, 0.85)
                     : root.tint(root.textColor, stepLine.prose ? 0.78 : 0.82)
                font.family: stepLine.prose ? root.sansFamily : root.monoFamily
                font.pixelSize: stepLine.prose ? root.stepSize - 1 : root.codeSize - 1
            }
            Text {
                id: stepLineInfo
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: !stepLine.entry ? ""
                      : stepLine.task ? root.taskLine(stepLine.task, stepLine.withKind).slice(1)
                      : [stepLine.entry.added > 0 ? "+" + stepLine.entry.added : "",
                         stepLine.entry.removed > 0 ? "−" + stepLine.entry.removed : "",
                         stepLine.entry.kind === "tool" && stepLine.entry.duration >= 1000
                         ? root.durationLabel(stepLine.entry.duration) : ""]
                        .filter(s => s.length > 0).join(" ")
                color: root.tint(root.mutedTextColor, 0.75)
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 2
                font.features: { "tnum": 1 }
            }
        }
    }

    // The wheel over a box of lines scrolls it, past either end the thread.
    component BoxWheel: WheelHandler {
        property var view: null
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                  : event.angleDelta.y / 120 * root.liveLineHeight * 2
            const top = view.originY
            const bottom = Math.max(top, view.originY + view.contentHeight - view.height)
            const next = Math.max(top, Math.min(bottom, view.contentY - step))
            if (next === view.contentY) {
                event.accepted = false
                return
            }
            view.contentY = next
            if (view.follow !== undefined)
                view.follow = next >= bottom - 2
        }
    }

    // What an opened subagent or background task shows: how it stands, what
    // it was asked or runs, then a subagent's own steps as one-line rows in
    // a box like the live work box (each opens under it; its own subagents
    // stay lines) and its final report, or a background command's last
    // lines of output. Read from its files when opened, and again every two
    // seconds while it runs.
    component TaskDetail: Column {
        id: taskDetail
        property var entry: null
        readonly property var task: entry && entry.task ? entry.task : null
        readonly property bool subagent: task !== null && task.type === "subagent"
        property var loaded: ({})
        readonly property var steps: loaded.steps ? loaded.steps : []
        property int openStep: -1
        function load() {
            loaded = root.model && entry && entry.item >= 0 ? root.model.taskDetail(entry.item) : ({})
        }
        onEntryChanged: load()
        Component.onCompleted: load()
        Timer {
            interval: 2000
            repeat: true
            running: taskDetail.task !== null && taskDetail.task.running && taskDetail.visible
            onTriggered: taskDetail.load()
        }
        spacing: 6
        Text {
            width: parent.width
            text: root.taskState(taskDetail.task)
            wrapMode: Text.Wrap
            color: root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.stepSize - 1
            font.features: { "tnum": 1 }
        }
        // What it was asked (a subagent's prompt) or is for.
        Selectable {
            visible: text.length > 0
            width: parent.width
            text: !taskDetail.entry ? ""
                  : taskDetail.task && taskDetail.task.type === "monitor" ? taskDetail.entry.body
                  : taskDetail.entry.detail
            color: root.mutedTextColor
            font.pixelSize: root.stepSize - 1
        }
        Code {
            visible: !taskDetail.subagent && text.length > 0
            width: parent.width
            text: !taskDetail.entry || taskDetail.subagent ? ""
                  : taskDetail.task.type === "monitor" ? taskDetail.entry.detail : taskDetail.entry.body
            language: "bash"
            caption: ""
            highlighted: true
            textColor: root.codeTextColor
        }
        Rectangle {
            objectName: "subagentSteps"
            visible: taskDetail.subagent && taskDetail.steps.length > 0
            width: parent.width
            height: Math.min(taskDetail.steps.length, root.liveBoxLines + 2) * root.liveLineHeight + 12
            radius: root.radius
            color: root.tint(root.raisedColor, 0.35)
            border.color: root.borderColor
            clip: true
            ListView {
                id: subList
                objectName: "subagentList"
                property bool follow: true
                anchors.fill: parent
                anchors.topMargin: 6
                anchors.bottomMargin: 6
                model: taskDetail.steps
                interactive: false
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                // A running subagent's newest step stays in view; a finished
                // one opens at its start.
                function toEnd() { if (follow && taskDetail.task && taskDetail.task.running) positionViewAtEnd() }
                onCountChanged: Qt.callLater(toEnd)
                delegate: StepLine {
                    required property var modelData
                    required property int index
                    width: subList.width
                    entry: modelData
                    opened: taskDetail.openStep === index
                    onClicked: taskDetail.openStep = opened ? -1 : index
                }
            }
            BoxWheel { view: subList }
        }
        Loader {
            width: parent.width
            active: taskDetail.openStep >= 0 && taskDetail.openStep < taskDetail.steps.length
            visible: active
            property var subEntry: active ? taskDetail.steps[taskDetail.openStep] : null
            // By id, so StepDetail and TaskDetail do not hold each other.
            sourceComponent: nestedStepDetail
        }
        Text {
            visible: taskDetail.subagent && taskDetail.loaded.found !== true
            width: parent.width
            text: qsTr("Its own steps are not saved where lapis can read them")
            wrapMode: Text.Wrap
            color: root.tint(root.mutedTextColor, 0.7)
            font.family: root.sansFamily
            font.pixelSize: root.stepSize - 2
        }
        // Its final report, as its parent received it.
        Text {
            visible: report.visible
            topPadding: 4
            text: qsTr("Report")
            color: root.tint(root.mutedTextColor, 0.75)
            font.family: root.sansFamily
            font.pixelSize: root.stepSize - 2
        }
        Selectable {
            id: report
            objectName: "subagentReport"
            visible: taskDetail.subagent && text.length > 0
            width: parent.width
            text: taskDetail.loaded.report ? taskDetail.loaded.report : ""
            color: root.tint(root.textColor, 0.9)
            font.pixelSize: root.stepSize
        }
        Item { width: 1; height: 4 } // a breath before the next step
        Code {
            objectName: "taskOutput"
            visible: !taskDetail.subagent && text.length > 0
            width: parent.width
            text: taskDetail.loaded.output ? taskDetail.loaded.output : ""
            textColor: root.tint(root.textColor, 0.75)
        }
    }

    // A subagent's own step opened under its box.
    Component {
        id: nestedStepDetail
        StepDetail {
            entry: parent ? parent.subEntry : null
            nested: true
        }
    }

    // Subagents and background tasks still running, one quiet line each
    // ("Subagent · review the parser · general-purpose · 3m 12s · 14
    // steps", "Background · npm test · 1m 05s"), at most four lines tall and
    // scrolling past that. More than fit collapse to one line ("3 subagents
    // and 2 background tasks running") that opens the list. A line opens
    // its task under the strip; finished ones leave it.
    property int stripOpenItem: -1
    property var stripDetail: null
    function refreshStripDetail() {
        let still = false
        for (let i = 0; i < runningWork.length; ++i)
            still = still || runningWork[i].item === stripOpenItem
        if (!still)
            stripOpenItem = -1
        stripDetail = stripOpenItem >= 0 && model ? model.itemEntry(stripOpenItem) : null
    }
    onRunningWorkChanged: refreshStripDetail()
    onStripOpenItemChanged: refreshStripDetail()
    component RunningStrip: Column {
        id: strip
        readonly property var tasks: root.runningWork
        property bool expanded: false
        readonly property bool many: tasks.length > root.liveBoxLines
        visible: tasks.length > 0
        spacing: 6
        Item {
            objectName: "runningSummary"
            visible: strip.many
            width: parent.width
            height: root.liveLineHeight
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: strip.expanded = !strip.expanded
            }
            Row {
                anchors.verticalCenter: parent.verticalCenter
                x: 12
                spacing: 8
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 6; height: 6; radius: 3
                    color: root.activityColor
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("%1 running").arg(root.workCount(strip.tasks))
                    color: root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize - 1
                }
                Chevron {
                    anchors.verticalCenter: parent.verticalCenter
                    turned: strip.expanded
                }
            }
        }
        Rectangle {
            objectName: "runningStrip"
            visible: !strip.many || strip.expanded
            width: parent.width
            height: Math.min(strip.tasks.length, root.liveBoxLines) * root.liveLineHeight + 8
            radius: root.radius
            color: "transparent"
            border.color: root.borderColor
            clip: true
            ListView {
                id: stripList
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                model: strip.tasks
                interactive: false
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                delegate: StepLine {
                    required property var modelData
                    width: stripList.width
                    withKind: true
                    entry: ({ kind: "tool", title: modelData.type === "subagent" ? qsTr("Subagent") : qsTr("Background"),
                              body: modelData.description, task: modelData, answered: true, failed: false,
                              added: 0, removed: 0, duration: 0 })
                    opened: root.stripOpenItem === modelData.item
                    onClicked: root.stripOpenItem = opened ? -1 : modelData.item
                }
            }
            BoxWheel { view: stripList }
        }
        Loader {
            width: parent.width
            active: root.stripDetail !== null
            visible: active
            sourceComponent: Component {
                StepDetail { entry: root.stripDetail }
            }
        }
    }

    // The running state's line: "Working · 2m 10s · 14 steps", "Compacting
    // conversation for 1m 3s", or "Running /review for 40s", with a slow
    // pulse. `steps` is -1 where no steps are known.
    component BusyLine: Item {
        id: busyLine
        property int steps: -1
        readonly property double since: root.compacting && root.model && root.model.compactingSince > 0
                                        ? root.model.compactingSince
                                        : root.waitingOnWork && root.model ? root.model.waitingSince
                                        : root.turnSince
        readonly property string elapsed: since > 0 && root.now - since >= 1000
                                          ? root.durationLabel(root.now - since) : ""
        height: root.stepSize + 10
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: root.activityColor
                SequentialAnimation on opacity {
                    running: (root.busy || root.waitingOnWork) && busyLine.visible
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.3; duration: 700; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
                }
            }
            Text {
                objectName: "busyText"
                anchors.verticalCenter: parent.verticalCenter
                text: root.compacting
                      ? (busyLine.elapsed.length > 0 ? qsTr("Compacting conversation for %1").arg(busyLine.elapsed)
                                                     : qsTr("Compacting conversation"))
                      : !root.working && root.pendingCommand.length > 0
                      ? (busyLine.elapsed.length > 0 ? qsTr("Running %1 for %2").arg(root.pendingCommand).arg(busyLine.elapsed)
                                                     : qsTr("Running %1").arg(root.pendingCommand))
                      // The agent's turn ended, but work it started runs on.
                      : root.waitingOnWork
                      ? [qsTr("Waiting on %1").arg(root.workCount(root.runningWork)), busyLine.elapsed]
                        .filter(s => s.length > 0).join(" · ")
                      : [qsTr("Working"),
                         busyLine.elapsed,
                         busyLine.steps < 0 ? "" : busyLine.steps === 1 ? qsTr("1 step")
                                                 : qsTr("%1 steps").arg(busyLine.steps)]
                        .filter(s => s.length > 0).join(" · ")
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize
                font.features: { "tnum": 1 }
            }
        }
    }

    // The step opened under the live work box: its item (stable while the
    // turn runs) and its roles, refreshed as the step's record changes.
    property int liveOpenItem: -1
    property var liveDetail: null
    function refreshLiveDetail() {
        const row = liveOpenItem >= 0 && liveSteps ? liveSteps.rowOf(liveOpenItem) : -1
        if (row < 0) {
            liveOpenItem = -1
            liveDetail = null
        } else {
            liveDetail = liveSteps.entry(row)
        }
    }
    onLiveOpenItemChanged: refreshLiveDetail()
    onLiveShownChanged: if (!liveShown) liveOpenItem = -1
    Connections {
        target: root.liveSteps
        function onDataChanged() { if (root.liveOpenItem >= 0) root.refreshLiveDetail() }
        function onModelReset() { if (root.liveOpenItem >= 0) root.refreshLiveDetail() }
    }
    // Lines the live work box shows at once; it never grows past them.
    readonly property int liveBoxLines: 4
    readonly property int liveLineHeight: Math.round(stepSize * 1.65)

    // The running turn, while the agent works: its state line, then one
    // compact box of its steps as the CLI records them ("Read greet.cpp",
    // "Edited greet_test.cpp +12 −3", "Ran ctest 14s", the model's interim
    // words), the newest at the bottom, earlier ones rolling up and out.
    // The box scrolls back through the turn without growing; clicking a
    // step opens it under the box, and clicking it again closes it. It is
    // the transcript's own record, never the terminal's screen.
    Component {
        id: liveBlock
        Column {
            id: live
            readonly property var entry: parent ? parent.entry : null
            spacing: 6
            // Counted from the box itself, so the line and the box agree.
            BusyLine {
                width: parent.width
                steps: liveList.count
            }
            Rectangle {
                id: box
                objectName: "liveBox"
                visible: !root.compacting
                width: parent.width
                height: root.liveBoxLines * root.liveLineHeight + 12
                radius: root.radius
                color: root.tint(root.raisedColor, 0.35)
                border.color: root.borderColor
                clip: true
                ListView {
                    id: liveList
                    objectName: "liveList"
                    property bool follow: true
                    anchors.fill: parent
                    anchors.topMargin: 6
                    anchors.bottomMargin: 6
                    model: root.liveSteps
                    interactive: false
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    function toEnd() { if (follow) positionViewAtEnd() }
                    onCountChanged: Qt.callLater(toEnd)
                    onContentHeightChanged: Qt.callLater(toEnd)
                    Component.onCompleted: Qt.callLater(toEnd)
                    // Claude Code writes a step as it is called and its
                    // result when done, so after a finished step the model
                    // is thinking; other CLIs write steps once done.
                    footer: Text {
                        readonly property bool shown: root.model !== null && root.model.format === "claude"
                                                      && (liveList.count === 0 || root.liveLastAnswered)
                                                      && !root.liveLastThought
                        width: liveList.width
                        height: shown || liveList.count === 0 ? root.liveLineHeight : 0
                        leftPadding: 12
                        verticalAlignment: Text.AlignVCenter
                        text: shown ? qsTr("Thinking…") : liveList.count === 0 ? qsTr("Waiting for the first step…") : ""
                        color: root.tint(root.mutedTextColor, 0.7)
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize - 1
                    }
                    delegate: Item {
                        id: line
                        required property int index
                        required property string kind
                        required property string title
                        required property string body
                        required property string detail
                        required property string result
                        required property var diff
                        required property int added
                        required property int removed
                        required property bool failed
                        required property bool answered
                        required property double duration
                        required property int item
                        required property var task
                        readonly property bool opened: root.liveOpenItem === item
                        // A thought shows its own words, dimmed: the newest,
                        // while it is the box's last line, flows over up to
                        // all lines but one (its latest lines in view); an
                        // older one is its first sentence.
                        readonly property bool thinking: kind === "thinking"
                        readonly property bool flowing: thinking && body.length > 0
                                                        && index === liveList.count - 1
                        readonly property bool openable: kind === "thinking" ? body.length > 0
                                                         : kind !== "tool" || !!task || result.length > 0
                                                         || detail.length > 0 || diff.length > 0
                                                         || title === "Ran" || body.indexOf("\n") >= 0
                                                         || body.indexOf("/") >= 0
                        width: liveList.width
                        height: flowing ? Math.min(Math.max(1, flow.lineCount), root.liveBoxLines - 1)
                                          * root.liveLineHeight
                                        : root.liveLineHeight
                        clip: flowing
                        Text {
                            id: flow
                            objectName: "thoughtFlow"
                            visible: line.flowing
                            x: 12
                            width: parent.width - 24
                            // Its newest lines at the bottom, earlier ones cut
                            // at the top.
                            y: parent.height - height
                            text: line.flowing ? line.body.replace(/[*_`#>]+/g, "") : ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            lineHeightMode: Text.FixedHeight
                            lineHeight: root.liveLineHeight
                            verticalAlignment: Text.AlignVCenter
                            color: root.tint(root.mutedTextColor, 0.85)
                            font.family: root.sansFamily
                            font.pixelSize: root.stepSize - 1
                        }
                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 4
                            radius: 4
                            color: line.opened ? root.tint(root.textColor, 0.07) : "transparent"
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: line.openable
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: root.liveOpenItem = line.opened ? -1 : line.item
                        }
                        Row {
                            visible: !line.flowing
                            anchors.verticalCenter: parent.verticalCenter
                            x: 12
                            width: parent.width - 24
                            spacing: 6
                            Rectangle {
                                visible: line.kind === "tool" && (line.task ? line.task.running : !line.answered)
                                width: 6; height: 6; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                color: root.activityColor
                            }
                            Text {
                                id: lineVerb
                                visible: text.length > 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: line.kind === "reply" || (line.thinking && line.body.length > 0) ? ""
                                      : line.title + (line.task ? " ·" : "")
                                color: line.failed ? root.faultColor : root.mutedTextColor
                                font.family: root.sansFamily
                                font.pixelSize: root.stepSize - 1
                            }
                            Text {
                                id: lineTarget
                                visible: text.length > 0
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, parent.width - lineVerb.width - lineCounts.width
                                                - lineTook.width - lineChevron.width - 46)
                                text: line.thinking ? root.thoughtLine(line.body) : root.stepTarget(line)
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                color: line.thinking ? root.tint(root.mutedTextColor, 0.85)
                                     : line.kind === "reply" ? root.tint(root.textColor, 0.78)
                                     : root.tint(root.textColor, 0.82)
                                readonly property bool prose: line.kind === "reply" || line.thinking
                                                              || (!!line.task && line.task.type !== "background")
                                font.family: prose ? root.sansFamily : root.monoFamily
                                font.pixelSize: prose ? root.stepSize - 1 : root.codeSize - 1
                            }
                            Row {
                                id: lineCounts
                                visible: line.added > 0 || line.removed > 0
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4
                                Text { text: "+" + line.added; visible: line.added > 0; color: root.addedColor; font.pixelSize: root.codeSize - 1; font.family: root.monoFamily }
                                Text { text: "−" + line.removed; visible: line.removed > 0; color: root.removedColor; font.pixelSize: root.codeSize - 1; font.family: root.monoFamily }
                            }
                            // How long it took; a task's run as its line says it.
                            Text {
                                id: lineTook
                                visible: text.length > 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: line.task ? root.taskLine(line.task, false).slice(1)
                                      : (line.kind === "tool" || (line.thinking && line.body.length > 0))
                                        && line.duration >= 1000 ? root.durationLabel(line.duration) : ""
                                color: root.tint(root.mutedTextColor, 0.75)
                                font.family: root.sansFamily
                                font.pixelSize: root.stepSize - 2
                                font.features: { "tnum": 1 }
                            }
                            Chevron {
                                id: lineChevron
                                visible: line.opened
                                anchors.verticalCenter: parent.verticalCenter
                                turned: true
                            }
                        }
                    }
                }
                // The wheel or trackpad scrolls back through the turn inside
                // the box; at either end it scrolls the thread instead.
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: function(event) {
                        const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                              : event.angleDelta.y / 120 * root.liveLineHeight * 2
                        const top = liveList.originY
                        const bottom = Math.max(top, liveList.originY + liveList.contentHeight - liveList.height)
                        const next = Math.max(top, Math.min(bottom, liveList.contentY - step))
                        if (next === liveList.contentY) {
                            event.accepted = false
                            return
                        }
                        liveList.contentY = next
                        liveList.follow = next >= bottom - 2
                    }
                }
            }
            // The step opened from the box, as an opened "Worked for" step.
            Loader {
                width: parent.width
                active: root.liveDetail !== null && !root.compacting
                visible: active
                sourceComponent: Component {
                    StepDetail { entry: root.liveDetail }
                }
            }
            RunningStrip {
                width: parent.width
            }
        }
    }
    // The newest step in the box is done (or is not a tool call).
    property bool liveLastAnswered: false
    // ...and is a thought with words, already flowing in the box.
    property bool liveLastThought: false
    function checkLiveLast() {
        const last = liveSteps && liveSteps.count > 0 ? liveSteps.entry(liveSteps.count - 1) : null
        liveLastAnswered = last !== null && (last.kind !== "tool" || last.answered)
        liveLastThought = last !== null && last.kind === "thinking" && last.body.length > 0
    }
    Connections {
        target: root.liveSteps
        function onRowsInserted() { root.checkLiveLast() }
        function onDataChanged() { root.checkLiveLast() }
        function onModelReset() { root.checkLiveLast() }
    }

    // A prompt the bar sent that the CLI has not recorded yet, drawn as its
    // row will draw it: "Queued" when sent while the agent worked, and a
    // note when it was found still in the agent's input box.
    component PendingPrompt: Column {
        required property var modelData
        required property int index
        readonly property bool unsent: root.unsentText.length > 0
                                       && root.unsentText.replace(/\s+/g, " ").trim() === modelData.key
        objectName: "pendingPrompt"
        width: parent ? parent.width : 0
        spacing: 4
        // Where its row will be: a prompt's gap above it (less the footer's
        // own top margin), and its buttons' room under it (less the
        // footer's spacing), so the row takes its place without a jump.
        topPadding: !modelData.queued && index === 0 ? Math.max(0, root.fontSize * 2 - 12) : 0
        bottomPadding: modelData.queued ? 0 : root.actionHeight + 6
        Rectangle {
            anchors.right: parent.right
            width: Math.min(pendingMeasure.implicitWidth + 36, parent.width * 0.8)
            height: pendingText.implicitHeight + 22
            radius: Math.round(root.fontSize * 1.1)
            color: root.raisedColor
            opacity: modelData.queued ? 0.6 : 1
            Text {
                id: pendingMeasure
                visible: false
                text: modelData.text
                font: pendingText.font
                textFormat: Text.PlainText
            }
            Text {
                id: pendingText
                x: 18; y: 11
                width: parent.width - 36
                text: modelData.text
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: root.textColor
                font.family: root.sansFamily
                font.pixelSize: root.fontSize
            }
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 6
            visible: text.length > 0
            text: parent.unsent ? qsTr("Not sent yet · press Return in the terminal")
                : modelData.queued ? qsTr("Queued · the agent reads it at its next step") : ""
            color: parent.unsent ? root.faultColor : root.mutedTextColor
            font.family: root.sansFamily
            font.pixelSize: root.codeSize - 1
        }
    }

    // The thread's end: a prompt just sent to an idle agent until the CLI
    // records it; while the agent works with no running turn recorded yet
    // (a prompt just sent, a slash command, a compaction), only the state
    // line; then the prompts sent mid-turn that the CLI has not recorded
    // yet, queued, in order.
    Component {
        id: liveFooter
        Item {
            id: footer
            readonly property int column: Math.min(root.columnWidth, width - 56)
            width: ListView.view ? ListView.view.width : 0
            height: footerColumn.height + 40
            Column {
                id: footerColumn
                x: (footer.width - footer.column) / 2
                y: 12
                width: footer.column
                spacing: 10
                // A prompt just sent to an idle agent, as its row will show
                // it, until the CLI records it.
                Repeater {
                    model: root.model ? root.model.pendingEntries.filter(p => !p.queued) : []
                    delegate: PendingPrompt {}
                }
                // With no live box: the agent busy, or idle while its
                // subagents and background tasks run ("Waiting on ...").
                BusyLine {
                    visible: (root.busy || root.waitingOnWork) && !root.liveShown
                    width: parent.width
                }
                RunningStrip {
                    visible: !root.liveShown && root.runningWork.length > 0
                    width: parent.width
                }
                Repeater {
                    model: root.model ? root.model.pendingEntries.filter(p => p.queued) : []
                    delegate: PendingPrompt {}
                }
            }
        }
    }

    // A finished turn's work, folded: "Worked for 3m 12s · 14 steps". Click
    // to show the thinking and steps between the prompt and the reply; each
    // step then opens on its own.
    Component {
        id: workBlock
        Item {
            id: work
            readonly property var entry: parent ? parent.entry : null
            readonly property int line: Math.max(workText.implicitHeight, root.stepSize + 10)
            height: line + (workSummary.visible ? workSummary.implicitHeight + 2 : 0)
            MouseArea {
                id: workHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (work.entry && root.model) root.model.toggleWork(work.entry.index)
            }
            Row {
                id: workRow
                y: (work.line - height) / 2
                spacing: 8
                Text {
                    id: workText
                    anchors.verticalCenter: parent.verticalCenter
                    text: work.entry ? work.entry.title : ""
                    color: workHover.containsMouse ? root.textColor : root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize
                }
                Chevron {
                    anchors.verticalCenter: parent.verticalCenter
                    turned: work.entry !== null && work.entry.expanded
                    color: workHover.containsMouse ? root.textColor : root.mutedTextColor
                }
            }
            Rectangle {
                y: work.line / 2
                x: workRow.width + 14
                width: parent.width - x
                height: 1
                color: root.borderColor
            }
            // What the work did, as the Codex app sums it up, shown only once
            // the work is opened; a turn of only thinking counts its steps.
            Text {
                id: workSummary
                visible: work.entry !== null && work.entry.expanded && work.entry.steps > 0
                y: work.line
                width: parent.width
                wrapMode: Text.Wrap
                text: !work.entry ? ""
                      : work.entry.detail !== "" ? work.entry.detail
                      : work.entry.steps === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(work.entry.steps)
                color: root.tint(root.mutedTextColor, 0.75)
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
            }
        }
    }

    Component {
        id: turnBlock
        Row {
            readonly property var entry: parent ? parent.entry : null
            spacing: 10
            Text {
                id: turnText
                text: entry ? entry.title : ""
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - turnText.width - 10
                height: 1
                color: root.borderColor
            }
        }
    }

    Component {
        id: commandBlock
        // The command, then what it printed: six lines, all of it on a click.
        Column {
            id: commandColumn
            readonly property var entry: parent ? parent.entry : null
            property bool open: false
            width: parent ? parent.width : 0
            spacing: 3
            Text {
                text: commandColumn.entry ? (commandColumn.entry.title
                      + (commandColumn.entry.body.length > 0 ? " " + commandColumn.entry.body : "")) : ""
                color: root.mutedTextColor
                font.family: root.monoFamily
                font.pixelSize: root.codeSize
            }
            Selectable {
                id: commandOutput
                readonly property bool tall: commandColumn.entry ? commandColumn.entry.resultLines > 6 : false
                visible: text.length > 0
                width: parent.width
                text: {
                    const all = commandColumn.entry ? commandColumn.entry.result : ""
                    return tall && !commandColumn.open ? all.split("\n").slice(0, 6).join("\n") : all
                }
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                color: root.tint(root.mutedTextColor, 0.85)
                font.family: root.monoFamily
                font.pixelSize: root.codeSize - 1
            }
            Text {
                visible: commandOutput.tall
                text: commandColumn.open ? qsTr("Show less")
                                         : qsTr("Show all %1 lines").arg(commandColumn.entry ? commandColumn.entry.resultLines : 0)
                color: moreHover.hovered ? root.textColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
                HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: commandColumn.open = !commandColumn.open }
            }
        }
    }

    Component {
        id: noticeBlock
        Selectable {
            readonly property var entry: parent ? parent.entry : null
            text: entry ? entry.title : ""
            color: entry && entry.failed ? root.faultColor : root.mutedTextColor
            font.pixelSize: root.stepSize
        }
    }

    Component {
        id: dividerBlock
        Item {
            readonly property var entry: parent ? parent.entry : null
            height: root.stepSize + 12
            Rectangle { y: parent.height / 2; width: parent.width; height: 1; color: root.borderColor }
            Rectangle {
                anchors.centerIn: parent
                width: dividerText.implicitWidth + 24
                height: parent.height
                color: root.backgroundColor
                Text {
                    id: dividerText
                    anchors.centerIn: parent
                    text: entry ? entry.title : ""
                    color: root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize - 1
                }
            }
        }
    }

    Component {
        id: otherBlock
        Column {
            readonly property var entry: parent ? parent.entry : null
            spacing: 6
            Text {
                text: entry ? "Unrecognized " + entry.title : ""
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize
            }
            Code {
                width: parent.width
                text: entry ? entry.detail : ""
                textColor: root.mutedTextColor
            }
        }
    }

    // The bar to type in, as in the Codex and Claude apps: Return sends to
    // the agent, Shift-Return starts a new line, Tab takes lapis's guess.
    property bool filledFromSuggestion: false
    property string notice: ""
    // Attached files go as @ mentions, which the CLI reads whatever their
    // type (images are seen as images); quoted when the path has spaces.
    function message() {
        const mentions = attachments.map(p => /\s/.test(p) ? "@\"" + p + "\"" : "@" + p)
        const text = input.text.trim()
        return mentions.length === 0 ? input.text
             : text.length === 0 ? mentions.join(" ")
             : input.text.replace(/\s+$/, "") + "\n\n" + mentions.join(" ")
    }
    function send() {
        const text = message()
        if (text.trim().length === 0 || !root.submit)
            return
        // A dialog in the terminal (a picker, settings) would take these
        // keys as its own, so show it instead and keep the message here.
        if (!root.waiting && root.slashCommands && root.screen && root.model
                && root.model.dialogShowing(root.screen())) {
            root.dialogFound()
            return
        }
        const command = root.slashCommands && /^\/[^\s\/]+/.test(text.trim()) ? text.trim().split(/\s/)[0] : ""
        // A CLI whose screen the view cannot read (any but Claude Code) may
        // answer a slash command with a picker, panel or output the thread
        // would hide, so its terminal shows instead; a command its list
        // knows runs as a turn (/compact, a skill) stays in the thread.
        const uncovers = root.model && !root.model.screenKnown && /^\/[^\s\/]+/.test(text.trim())
                         && root.answersInTerminal(text.trim().split(/\s/)[0].slice(1))
        // A turn the file shows ended is not working, whatever the status.
        const working = root.working && !root.turnEnded
        if (root.submit(text)) {
            // The prompt shows at once, at the thread's end, until the CLI
            // records it (queued when the agent was working), and is then
            // checked for having been taken.
            if (command.length === 0 && root.model) {
                root.model.noteSent(text, working)
                if (!working)
                    root.sentAt = Date.now()
                root.checkingText = text
                root.retried = false
                root.unsentText = ""
                sendCheck.restart()
            }
            if (uncovers)
                root.terminalNeeded()
            if (command.length > 0 && !uncovers) {
                root.pendingCommand = command
                root.pendingSince = Date.now()
                root.quietTicks = 0
                root.commandSent(text.trim())
            }
            if (root.filledFromSuggestion && text === root.suggestion)
                root.suggestionSent()
            root.filledFromSuggestion = false
            root.attachments = []
            input.clear()
            root.following = true
            list.positionViewAtEnd()
            root.notice = ""
        } else {
            root.notice = root.waiting ? qsTr("Answer the request in the terminal first (Esc)")
                                       : qsTr("Not sent: the agent is not ready for input")
            noticeTimer.restart()
        }
    }
    Timer { id: noticeTimer; interval: 4000; onTriggered: root.notice = "" }

    Item {
        id: composerArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: composerBox.height + Math.round(root.fontSize * 0.8) + hints.height + 6

        // The slash commands that best match what is typed, three at most.
        Rectangle {
            id: slashList
            objectName: "visualSlashList"
            visible: root.slashMatches.length > 0
            z: 2
            width: composerBox.width
            x: composerBox.x
            anchors.bottom: composerBox.top
            anchors.bottomMargin: 6
            height: slashColumn.implicitHeight + 10
            radius: 10
            color: root.raisedColor
            border.color: root.borderColor
            Column {
                id: slashColumn
                x: 5; y: 5
                width: parent.width - 10
                Repeater {
                    model: root.slashMatches
                    delegate: Rectangle {
                        id: slashRow
                        required property var modelData
                        required property int index
                        width: slashColumn.width
                        height: Math.round(root.stepSize * 2.1)
                        radius: 6
                        color: index === root.slashIndex ? root.tint(root.textColor, 0.1) : "transparent"
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            spacing: 12
                            Text {
                                text: "/" + slashRow.modelData.name
                                color: root.textColor
                                font.family: root.monoFamily
                                font.pixelSize: root.codeSize
                            }
                            Text {
                                width: slashColumn.width - x - 20
                                text: slashRow.modelData.description
                                elide: Text.ElideRight
                                color: root.mutedTextColor
                                font.family: root.sansFamily
                                font.pixelSize: root.codeSize
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: root.slashIndex = slashRow.index
                            onClicked: root.completeSlash(slashRow.modelData.name)
                        }
                    }
                }
            }
        }

        Rectangle {
            id: composerBox
            width: Math.min(root.columnWidth, parent.width - 56)
            x: (parent.width - width) / 2
            y: 0
            height: chips.height + inputScroll.height + toolbar.height + 30
            radius: Math.round(root.fontSize * 0.9)
            color: root.raisedColor
            border.width: 1
            border.color: dropArea.containsDrag ? root.accentColor
                        : input.activeFocus ? root.tint(root.accentColor, 0.55) : root.borderColor

            // Files waiting to be sent: name, a thumbnail for an image, ×.
            Flow {
                id: chips
                x: 12; y: root.attachments.length > 0 ? 10 : 0
                width: parent.width - 24
                height: root.attachments.length > 0 ? implicitHeight + 6 : 0
                spacing: 6
                Repeater {
                    model: root.attachments
                    delegate: Rectangle {
                        id: chip
                        required property string modelData
                        readonly property bool image: root.model ? root.model.isImage(modelData) : false
                        height: Math.round(root.codeSize * 2.2)
                        width: chipRow.implicitWidth + 16
                        radius: 8
                        color: root.tint(root.textColor, 0.07)
                        border.color: root.borderColor
                        Row {
                            id: chipRow
                            x: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Image {
                                visible: chip.image
                                width: visible ? chip.height - 8 : 0
                                height: chip.height - 8
                                anchors.verticalCenter: parent.verticalCenter
                                source: chip.image ? "file://" + chip.modelData : ""
                                sourceSize.height: 64
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: chip.modelData.split("/").pop()
                                width: Math.min(implicitWidth, 220)
                                elide: Text.ElideMiddle
                                color: root.textColor
                                font.family: root.sansFamily
                                font.pixelSize: root.codeSize
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "×"
                                color: removeMouse.containsMouse ? root.textColor : root.mutedTextColor
                                font.pixelSize: root.stepSize
                                MouseArea {
                                    id: removeMouse
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.detach(chip.modelData)
                                }
                            }
                        }
                    }
                }
            }

            // lapis's guess, in full: it wraps, and the bar grows to hold it.
            Text {
                id: suggestionText
                x: inputScroll.x
                y: inputScroll.y
                width: inputScroll.width
                visible: input.text.length === 0 && input.preeditText.length === 0
                text: root.waiting ? qsTr("Waiting for your answer in the terminal")
                    : root.suggestion.length > 0 ? root.suggestion
                    : qsTr("Message the agent")
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: root.suggestion.length > 0 && !root.waiting
                       ? root.tint(root.textColor, root.suggestionConfident ? 0.62 : 0.42)
                       : root.mutedTextColor
                font: input.font
            }
            ScrollView {
                id: inputScroll
                x: 16; y: 14 + chips.height
                width: parent.width - 32
                // Two lines at least, so the bar never looks flat; it grows
                // with what is typed (or offered) up to eight.
                height: Math.min(Math.max(input.implicitHeight,
                                          suggestionText.visible ? suggestionText.implicitHeight : 0,
                                          Math.ceil(root.fontSize * 1.45 * 2)),
                                 root.fontSize * 1.45 * 8)
                TextArea {
                    id: input
                    objectName: "visualComposer"
                    focus: true
                    wrapMode: TextArea.Wrap
                    textFormat: TextEdit.PlainText
                    padding: 0
                    leftPadding: 0
                    rightPadding: 0
                    background: null
                    color: root.textColor
                    selectionColor: root.tint(root.accentColor, 0.4)
                    selectedTextColor: root.textColor
                    font.family: root.sansFamily
                    font.pixelSize: root.fontSize
                    onTextChanged: if (text.length === 0) root.filledFromSuggestion = false
                    Keys.onPressed: function(event) {
                        // AppKit marks arrows as keypad keys; lapis keeps
                        // Command as Meta and Control as Control.
                        const mods = event.modifiers & ~Qt.KeypadModifier
                        const plain = mods === Qt.NoModifier
                        const isReturn = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        // The slash list takes arrows, Tab, Return and Escape.
                        if (root.slashMatches.length > 0 && plain) {
                            const count = root.slashMatches.length
                            if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                                root.slashIndex = (root.slashIndex + (event.key === Qt.Key_Down ? 1 : count - 1)) % count
                                event.accepted = true
                                return
                            }
                            // As in the CLI's own list: Tab fills the
                            // command in to add to, Return runs it.
                            if (event.key === Qt.Key_Tab || isReturn) {
                                root.completeSlash(root.slashMatches[Math.min(root.slashIndex, count - 1)].name)
                                if (isReturn)
                                    root.send()
                                event.accepted = true
                                return
                            }
                            if (event.key === Qt.Key_Escape) {
                                root.slashDismissed = true
                                event.accepted = true
                                return
                            }
                        }
                        if (isReturn && (plain || mods === Qt.MetaModifier)) {
                            root.send()
                            event.accepted = true
                        } else if (isReturn && mods === Qt.ShiftModifier) {
                            // A plain newline: Qt's own Shift-Return puts a
                            // line separator (U+2028) that CLIs read oddly.
                            if (selectedText.length > 0)
                                remove(selectionStart, selectionEnd)
                            insert(cursorPosition, "\n")
                            event.accepted = true
                        } else if (root.editKey(input, event.key, mods)) {
                            event.accepted = true
                        } else if (event.key === Qt.Key_Tab && plain) {
                            if (root.suggestion.length > 0 && text.length === 0) {
                                text = root.suggestion
                                cursorPosition = text.length
                                root.filledFromSuggestion = true
                                root.suggestionFilled()
                            } else if (root.filledFromSuggestion && text === root.suggestion) {
                                root.send()
                            } else if (text.length === 0 && root.tabAway) {
                                root.tabAway()
                            }
                            event.accepted = true
                        } else if (event.matches(StandardKey.Copy) && selectedText.length > 0) {
                            root.copy(selectedText)
                            event.accepted = true
                        } else if (event.matches(StandardKey.Paste) && root.model) {
                            // Copied files or an image attach; text pastes as text.
                            const files = root.model.pastedFiles()
                            if (files.length > 0) {
                                files.forEach(p => root.attach(p))
                                event.accepted = true
                            }
                        } else if (event.key === Qt.Key_Backspace && text.length === 0
                                   && root.attachments.length > 0) {
                            root.detach(root.attachments[root.attachments.length - 1])
                            event.accepted = true
                        }
                    }
                }
            }

            Item {
                id: toolbar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
                anchors.leftMargin: 16
                height: sendButton.height
                // Left: attach, model, effort. Right: context use, Stop, Send.
                Row {
                    id: controls
                    anchors.left: parent.left
                    anchors.leftMargin: -6
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    BarButton {
                        objectName: "visualAttach"
                        label: "+"
                        labelSize: root.stepSize + 4
                        menuOpen: attachMenu.opened
                        onTriggered: attachMenu.popup(this, 0, -attachMenu.implicitHeight - 6)
                    }
                    BarButton {
                        objectName: "visualModel"
                        visible: root.model !== null && root.model.model.length > 0
                        label: root.modelLabel(root.model ? root.model.model : "")
                        menu: root.models.length > 0
                        menuOpen: modelMenu.opened
                        onTriggered: if (root.models.length > 0) modelMenu.popup(this, 0, -modelMenu.implicitHeight - 6)
                    }
                    BarButton {
                        objectName: "visualEffort"
                        visible: root.efforts.length > 0 || (root.model !== null && root.model.effort.length > 0)
                        label: (root.model && root.model.effort.length > 0
                                ? root.effortLabel(root.model.effort)
                                : qsTr("Effort"))
                        menu: root.efforts.length > 0
                        menuOpen: effortMenu.opened
                        onTriggered: if (root.efforts.length > 0) effortMenu.popup(this, 0, -effortMenu.implicitHeight - 6)
                    }
                }
                ChoiceMenu {
                    id: modelMenu
                    // The CLI's own default entry carries a sentence for a
                    // name; it is just "Default" here.
                    choices: root.models.map(m => ({ id: m.id, label: m.id === "default" ? qsTr("Default")
                                                                                         : (m.name || m.id) }))
                    // The model the agent reports, matched to an entry by its
                    // id or its family ("opus" in "claude-opus-5-5").
                    current: {
                        const running = root.model ? root.model.model : ""
                        const exact = root.models.find(m => m.id === running)
                        if (exact)
                            return exact.id
                        const family = root.models.find(m => m.id !== "default" && running.indexOf(m.id) >= 0)
                        return family ? family.id : ""
                    }
                    onChosen: id => root.modelChosen(id)
                }
                ChoiceMenu {
                    id: attachMenu
                    choices: [{ id: "files", label: qsTr("Files…") }, { id: "folder", label: qsTr("Folder…") }]
                    onChosen: id => id === "folder" ? folderPicker.open() : filePicker.open()
                }
                ChoiceMenu {
                    id: effortMenu
                    choices: root.efforts.map(e => ({ id: e, label: root.effortLabel(e) }))
                    current: root.model ? root.model.effort : ""
                    onChosen: id => root.effortChosen(id)
                }
                // How full the context window is: a ring, the share used and
                // the tokens against the window, always in view.
                Item {
                    id: contextMeter
                    objectName: "visualContext"
                    readonly property real used: root.model && root.model.contextWindow > 0
                                                 ? Math.min(1, root.model.contextTokens / root.model.contextWindow) : 0
                    visible: root.model !== null && root.model.contextTokens > 0
                    anchors.right: sendButton.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: visible ? ring.width + percent.implicitWidth + 6 : 0
                    height: sendButton.height
                    Canvas {
                        id: ring
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(root.codeSize * 1.3)
                        height: width
                        readonly property real fraction: contextMeter.used
                        readonly property color track: root.tint(root.textColor, 0.15)
                        readonly property color fill: contextMeter.used > 0.9 ? root.faultColor
                                                    : contextMeter.used > 0.75 ? Qt.tint(root.mutedTextColor, root.tint(root.faultColor, 0.5))
                                                    : root.mutedTextColor
                        onFractionChanged: requestPaint()
                        onFillChanged: requestPaint()
                        onPaint: {
                            const g = getContext("2d")
                            g.reset()
                            const r = width / 2 - 2
                            g.lineWidth = 2.5
                            g.strokeStyle = track
                            g.beginPath(); g.arc(width / 2, height / 2, r, 0, 2 * Math.PI); g.stroke()
                            g.strokeStyle = fill
                            g.beginPath()
                            g.arc(width / 2, height / 2, r, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * fraction)
                            g.stroke()
                        }
                    }
                    Text {
                        id: percent
                        anchors.left: ring.right
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.model
                              ? qsTr("%1% · %2 / %3").arg(Math.round(contextMeter.used * 100))
                                .arg(root.tokensLabel(root.model.contextTokens))
                                .arg(root.tokensLabel(root.model.contextWindow))
                              : ""
                        color: root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                        font.features: { "tnum": 1 }
                    }
                }
                Text {
                    anchors.left: controls.right
                    anchors.leftMargin: 8
                    anchors.right: contextMeter.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: root.notice.length > 0 ? root.notice
                        : root.waiting ? (root.waitingReason.length > 0 ? root.waitingReason
                                                                         : qsTr("The agent is waiting on a request"))
                        : root.suggestion.length > 0 && input.text.length === 0 ? qsTr("Tab to use the suggestion")
                        : root.filledFromSuggestion ? qsTr("Tab or Return to send")
                        : ""
                    color: root.notice.length > 0 || root.waiting ? root.faultColor : root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: Math.max(11, root.codeSize - 1)
                }
                // Send, or Stop while a turn runs: Return sends (a message
                // typed mid-turn queues for the agent), so the button stops.
                Rectangle {
                    id: sendButton
                    objectName: "visualSend"
                    readonly property bool stops: root.working && root.interrupt !== null
                    readonly property bool ready: stops
                                                  || ((input.text.trim().length > 0 || root.attachments.length > 0)
                                                      && root.canSend && !root.waiting)
                    anchors.right: parent.right
                    width: Math.round(root.fontSize * 1.75)
                    height: width
                    radius: width / 2
                    color: ready ? (sendMouse.containsMouse ? root.tint(root.textColor, 0.85) : root.textColor)
                                 : root.tint(root.textColor, 0.18)
                    Text {
                        anchors.centerIn: parent
                        visible: !sendButton.stops
                        text: "↑"
                        color: root.backgroundColor
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize
                        font.bold: true
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        visible: sendButton.stops
                        width: Math.round(parent.width * 0.32)
                        height: width
                        radius: 2
                        color: root.backgroundColor
                    }
                    MouseArea {
                        id: sendMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: sendButton.ready
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: sendButton.stops ? root.interrupt() : root.send()
                    }
                }
            }
        }

        Text {
            id: hints
            anchors.top: composerBox.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: composerBox.horizontalCenter
            text: qsTr("Return to send · Shift-Return for a new line · ⌘-click a path to open it · Esc for the terminal")
            color: root.tint(root.mutedTextColor, 0.8)
            font.family: root.sansFamily
            font.pixelSize: Math.max(11, root.codeSize - 3)
        }
    }

    Text {
        visible: root.model !== null && root.model.count === 0 && !root.model.loading
        anchors.centerIn: parent
        text: qsTr("Nothing saved in this conversation yet")
        color: root.mutedTextColor
        font.family: root.sansFamily
        font.pixelSize: root.stepSize
    }

    // Back to the newest item; the view follows from there.
    Rectangle {
        visible: !root.following && list.count > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: composerArea.top
        anchors.bottomMargin: 12
        width: root.fontSize * 2.2; height: width; radius: width / 2
        color: root.raisedColor
        border.color: root.borderColor
        Text { anchors.centerIn: parent; text: "↓"; color: root.textColor; font.pixelSize: root.stepSize }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.following = true; list.positionViewAtEnd() }
        }
    }
}
