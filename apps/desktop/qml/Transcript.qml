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
    // typeText(text) pastes into the agent's input box with no Return;
    // sendKeys(bytes) sends a control chord (never Control-C or -D, which
    // quit a CLI); typedScreen() is screen() without faint text, so a
    // placeholder in the input box does not read as text it holds.
    property var typeText: null
    property var sendKeys: null
    property var typedScreen: null
    // lapis's predicted next prompt (Tab), shown in the empty bar.
    property string suggestion: ""
    property string suggestionKey: ""   // the offer the suggestion belongs to
    property string suggestionAuthor: "" // who wrote it ("Fable 5.2 · high"); empty when unknown
    property bool suggestionConfident: false
    // The agent the bar types into (its lapis id). The view is one for every
    // agent, so what Tab filled, and the check on a sent prompt, belong to
    // the agent they were for and end when another shows.
    property string agent: ""
    onAgentChanged: {
        filledText = ""
        filledKey = ""
        sendCheck.stop()
        checkingText = ""
        checkingDraft = null
        retried = false
    }
    property bool working: false        // a turn is running: Send becomes Stop
    property bool waiting: false        // a request waits in the terminal
    property string waitingReason: ""
    property bool canSend: true
    // The agent's CLI has saved nothing yet: the thread is empty and shows
    // which CLI it is and where it works.
    property bool blank: false
    // Its CLI has not drawn its input box yet (starting, or a screen of its
    // own): what is typed stays in the bar, never sent to it.
    property bool starting: false
    // What Command-click on a file path opens it with (an editors() id).
    property string editor: "auto"
    // The agent's terminal rows as text (screenLines()), null where there
    // is no terminal. The thread never shows them: it reads them only to
    // see Claude Code compacting, a command going quiet, and whether a sent
    // prompt left the agent's input box.
    property var screen: null
    // A link clicked in the thread goes here first; true means it was handled.
    property var openLink: null
    // Height kept above the composer for the host's own strip (accessory).
    property real composerInset: 0
    readonly property alias accessory: accessorySlot
    // The terminal's rows above its input box, read a few times a second
    // while a command or turn runs, to see when the screen goes quiet.
    property var liveRows: []
    // The running turn's work shows as step rows under its live header, fed by the
    // transcript's own records as they land; the model knows it is running
    // from here.
    Binding {
        target: root.model
        property: "live"
        value: root.working
        when: root.model !== null
    }
    // A request waiting in the terminal keeps the last turn whole, even one
    // another agent's message started (which otherwise folds to a row).
    Binding {
        target: root.model
        property: "requestWaiting"
        value: root.waiting
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
    onWorkingChanged: if (working) {
        pendingCommand = ""
        sentAt = 0
        if (model)
            model.refreshQueueMode() // agy's setting, for this turn's sends
    }
    // The turn a sent prompt started has ended in the file.
    onTurnEndedChanged: if (turnEnded) sentAt = 0
    // The prompt last sent, checked once it should have been taken: an
    // input box still holding it gets one more Return (only then, so a
    // Return never lands in anything else). Still there, it comes back to
    // the bar (the box cleared, so the next send does not join it) with a
    // note; when the box cannot be cleared, its bubble and the bar say it
    // was not sent. The bubble's note goes once the box no longer holds it.
    property string checkingText: ""
    property bool retried: false
    property string unsentText: ""
    // What the bar held for the prompt being checked: {text, attachments}.
    property var checkingDraft: null
    function checkSent() {
        const holds = root.checkingText.length > 0 && root.screen && root.model && !root.waiting
                      && !root.model.dialogShowing(root.screen())
                      && root.model.composerHolds(root.screen(), root.checkingText)
        if (!holds) {
            root.checkingText = ""
            root.checkingDraft = null
            return
        }
        if (!root.retried && root.pressReturn) {
            root.retried = true
            root.pressReturn()
            sendCheck.restart()
            return
        }
        const text = root.checkingText
        const draft = root.checkingDraft
        root.unsentText = text
        root.checkingText = ""
        root.checkingDraft = null
        root.sentAt = 0
        unsentCheck.restart()
        if (draft && root.sendKeys)
            takeBackUnsent(text, draft, root.agent, 0)
        else
            say(qsTr("Not sent: %1 did not take it · it is in its input box, press Return in the terminal")
                .arg(root.cliName()))
    }
    // Clears the agent's input box of a prompt it did not take (Control-U,
    // never Control-C or -D) and, once the box reads clear, puts the prompt
    // back in the bar ahead of anything typed since.
    function takeBackUnsent(text, draft, agent, tries) {
        const unsent = qsTr("Not sent: %1 did not take it · it is in its input box, press Return in the terminal")
                       .arg(root.cliName())
        if (root.agent !== agent || !root.screen || !root.model)
            return
        const lines = Math.max(text.split("\n").length,
                               root.model.composerText(root.screen()).split("\n").length)
        if (!root.sendKeys("\u0015".repeat(Math.max(1, Math.min(16, 2 * lines - 1))))) {
            say(unsent)
            return
        }
        after(400, () => root.agent === agent && root.screen && root.model
                         && !root.model.composerHolds(root.screen(), text), () => {
            const typed = input.text
            input.text = typed.trim().length > 0 ? draft.text + "\n\n" + typed : draft.text
            input.cursorPosition = input.text.length
            root.attachments = draft.attachments.concat(
                        root.attachments.filter(p => draft.attachments.indexOf(p) < 0))
            root.model.forgetSent(text)
            root.unsentText = ""
            say(qsTr("Not sent: %1 did not take it · it is back in the bar").arg(root.cliName()))
        }, () => {
            if (root.agent !== agent)
                return // another agent shows; its bubble keeps the note
            if (tries + 1 < 3)
                takeBackUnsent(text, draft, agent, tries + 1)
            else
                say(unsent)
        })
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
            // A prompt sent now that the CLI recorded (or gave back) goes.
            const left = {}
            for (const key in root.urged)
                if (root.model.pendingPrompts.indexOf(key) >= 0)
                    left[key] = root.urged[key]
            root.urged = left
        }
    }

    // Mid-turn sending, per CLI (verified by hand against Claude Code
    // 2.1.322, Codex 0.157.1 and agy 1.5.7): Return queues (Claude Code),
    // steers (Codex) or queues as agy's `queuedMessages` says; "send now"
    // makes the model take it soonest without discarding work where the CLI
    // allows. Claude Code: the message, then Control-X Control-S. agy in its
    // Queue mode: the message with no Return, then Alt-M (Escape m); with no
    // text, Alt-M sends what it has queued. Codex has no such key: its send
    // now is "Interrupt and send", Escape once the steer shows waiting.
    readonly property string cli: model ? model.format : ""
    readonly property string queueMode: model ? model.queueMode : "queue"
    readonly property bool midTurn: working && !turnEnded
    readonly property bool sendsNow: midTurn && (cli === "claude" || cli === "codex"
                                                 || (cli === "antigravity" && queueMode === "turn-end"))
    readonly property string sendNowLabel: cli === "codex" ? qsTr("Interrupt and send") : qsTr("Send now")
    // Prompts sent mid-turn waiting for the CLI (not those sent to an idle
    // one, nor those already going as a new turn after an interrupt).
    readonly property bool messageWaiting: model !== null
                                           && model.pendingEntries.some(p => p.queued && urged[p.key] !== "interrupt")
    // What Stop (and Escape) does to a waiting message.
    readonly property string stopLabel: !messageWaiting ? qsTr("Stop")
                                      : cli === "antigravity" ? qsTr("Stop · queued text returns to the box")
                                      : qsTr("Interrupt and send")
    // Waiting prompts sent on at once, by key: "now" (the CLI's send-now
    // key) or "interrupt" (sent as a new turn after Escape).
    property var urged: ({})
    function markUrged(keys, how) {
        const next = Object.assign({}, urged)
        keys.forEach(k => next[k] = how)
        urged = next
    }
    function keyOf(text) { return text.replace(/\s+/g, " ").trim() }
    // The quiet line under a waiting prompt, as its CLI will treat it.
    function queuedLabel() {
        return queueMode === "steer" ? qsTr("Steering · read after the current step")
             : queueMode === "turn-end" ? qsTr("Queued · sent when this turn ends")
             : queueMode === "immediate" ? qsTr("Sent · read at the next step")
             : qsTr("Queued · read at the next step")
    }
    function pendingLabel(key) {
        const how = urged[key]
        return how === "interrupt" ? qsTr("Interrupting · sent as a new turn")
             : how === "now" ? (cli === "antigravity" ? qsTr("Sending now · read at the next step")
                                                      : qsTr("Sending now · read in a moment"))
             : queuedLabel()
    }
    // A note in the bar's toolbar; `quiet` for one that reports rather
    // than warns.
    property bool noticeQuiet: false
    function say(text, quiet) {
        notice = text
        noticeQuiet = quiet === true
        noticeTimer.restart()
    }

    // Steps to take once a condition holds on the agent's screen, or after
    // a delay: each {due, until, then, otherwise}, checked every 50 ms.
    property var followUps: []
    function after(ms, until, then, otherwise) {
        followUps = followUps.concat([{ due: Date.now() + ms, until: until, then: then,
                                        otherwise: otherwise || null }])
    }
    function runFollowUps() {
        const now = Date.now()
        const keep = [], ready = []
        for (const f of followUps) {
            if (f.until && f.until())
                ready.push(f.then)
            else if (now >= f.due)
                ready.push(f.until ? f.otherwise : f.then)
            else
                keep.push(f)
        }
        followUps = keep
        ready.forEach(fn => { if (fn) fn() })
    }
    Timer { interval: 50; repeat: true; running: root.followUps.length > 0; onTriggered: root.runFollowUps() }

    // What the agent's input box holds (Codex and agy, whose boxes keep or
    // get back text: a draft left there, queued messages Escape returned),
    // or "". Read from the screen's rows, so a heuristic.
    function heldText() {
        if (!model || (cli !== "codex" && cli !== "antigravity") || (!typedScreen && !screen))
            return ""
        return model.composerText(typedScreen ? typedScreen() : screen())
    }
    // Held text that could not be cleared is not pulled again.
    property string heldIgnored: ""
    // Puts what the agent's input box held into the bar, ahead of anything
    // typed, retires the bubbles it holds, and clears the box with
    // Control-U (never Control-C or -D) until it reads empty.
    function pullBack(held) {
        const typed = input.text
        input.text = typed.trim().length > 0 ? held + "\n\n" + typed : held
        input.cursorPosition = input.text.length
        if (model)
            model.forgetSent(held)
        clearAgentBox(held, 0)
    }
    // Control-U clears the cursor's line, and on an empty line joins it to
    // the one above: a draft of n lines takes 2n - 1 presses in Codex 0.157.1,
    // agy 1.5.7 and Claude Code 2.1.322 (a wrapped line is one), and extra
    // presses on an empty box do nothing. So each try sends what the text
    // left needs, at most 16 bytes (TerminalSurface::max_chord_bytes).
    function clearAgentBox(held, tries) {
        const left = tries === 0 ? held : heldText()
        const presses = Math.max(1, Math.min(16, 2 * left.split("\n").length - 1))
        if (!sendKeys || !sendKeys("\u0015".repeat(presses))) {
            heldIgnored = held
            return
        }
        after(400, () => heldText().length === 0, null, () => {
            if (tries + 1 < 4) {
                clearAgentBox(held, tries + 1)
            } else {
                heldIgnored = held
                say(qsTr("The agent's input box still holds text; clear it in the terminal"))
            }
        })
    }
    // Before sending: text in the agent's box the view did not put there
    // would join the message, so it comes into the bar instead. Returns
    // whether it did (and nothing was sent).
    function tookLeftover() {
        const held = heldText()
        if (held.length === 0 || held === heldIgnored)
            return false
        const rows = screen ? screen() : []
        for (const mine of [checkingText, unsentText])
            if (mine.length > 0 && model.composerHolds(rows, mine))
                return false
        pullBack(held)
        say(qsTr("The agent's input box held this; it is in the bar now"), true)
        return true
    }

    // Stop (the button, or Escape with nothing typed): Escape for the agent.
    // Claude Code and Codex then send a waiting message as a new turn; agy
    // puts its queued messages back in its box, which come into the bar.
    function stop() {
        if (!interrupt || !interrupt())
            return false
        if (messageWaiting && cli !== "antigravity")
            markUrged(model.pendingEntries.filter(p => p.queued).map(p => p.key), "interrupt")
        if (cli === "codex" || cli === "antigravity")
            after(2500, () => heldText().length > 0, () => {
                pullBack(heldText())
                say(qsTr("Back in the bar from the agent's input box"), true)
            }, null)
        return true
    }
    // Command-Return: the message sent now (sendMessage(true)); with
    // nothing typed, the messages already waiting.
    function sendNow() {
        if (message().trim().length === 0)
            sendQueuedNow()
        else
            sendMessage(true)
    }
    function sendQueuedNow() {
        if (!sendsNow || !messageWaiting || !model)
            return
        const keys = model.pendingEntries.filter(p => p.queued).map(p => p.key)
        if (cli === "codex") {
            stop()
            return
        }
        if (cli === "antigravity" && tookLeftover())
            return // Alt-M would send that text instead
        const chord = cli === "claude" ? "\u0018\u0013" : "\u001bm"
        if (sendKeys && sendKeys(chord))
            markUrged(keys, "now")
        else
            say(qsTr("Not sent now: the agent is not ready for input"))
    }
    // After a message sent mid-turn with send now: the CLI's key for it.
    function urge(text) {
        const key = keyOf(text)
        if (cli === "codex") {
            markUrged([key], "interrupt")
            after(4000, () => model && screen && model.steerWaiting(screen())
                              && !model.composerHolds(screen(), text),
                  () => { if (!interrupt || !interrupt()) markUrged([key], "") },
                  () => {
                      markUrged([key], "")
                      say(qsTr("Codex has not shown the message waiting; it stays a steer"))
                  })
            return
        }
        markUrged([key], "now")
        const chord = cli === "claude" ? "\u0018\u0013" : "\u001bm"
        // A moment after the paste, so the CLI has the text before the key;
        // longer for a message with an @ mention, whose Return the terminal
        // sends on its own a moment after the paste
        // (TerminalSurface::composer_return_ms).
        after(/(^|\s)@\S/.test(text) ? 900 : 300, null, () => {
            if (!sendKeys || !sendKeys(chord)) {
                markUrged([key], "")
                say(qsTr("Not sent now: the agent is not ready for input"))
            }
        })
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
    // The empty thread's line in place of Starting/Ready, when the host has
    // a better one (a remote agent's conversation still loading).
    property string blankStatus: ""

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
    // (workspace.pickerModels: {id, name, resolved, levels: [{effort, id}]}),
    // the fallback effort levels for a model its CLI lists none for, and
    // what it shows now (the model's shownModel and shownEffort). The window
    // turns a choice into that CLI's own command: `id` the model id to send
    // (a family's level id), `family` and `effort` what to show at once.
    property var models: []
    property var efforts: []
    signal modelChosen(string id, string family, string effort, string label)
    signal effortChosen(string level)
    // The model menu opened: the window asks the CLIs for their lists again.
    signal pickerOpened()
    // The picker entry for a model as the agent or the person named it: by
    // id, name, the API model an alias stands for, or a family's level id;
    // else the entry whose id it holds ("opus" in "claude-opus-5-5").
    function levelsOf(entry) { return entry && entry.levels ? entry.levels : [] }
    function names(entry, id) {
        return entry.id === id || entry.name === id || (entry.resolved && entry.resolved === id)
               || levelsOf(entry).some(l => l.id === id)
    }
    function modelEntry(id) {
        if (!id)
            return null
        return models.find(m => m.id !== "default" && names(m, id))
               || models.find(m => names(m, id))
               || models.find(m => m.id !== "default" && id.indexOf(m.id) >= 0) || null
    }
    readonly property var currentEntry: modelEntry(root.model ? root.model.shownModel : "")
    // The current model's own levels; the fallback only for a model its CLI
    // lists none for (no list yet, or a model the list does not have).
    readonly property var currentEfforts: levelsOf(currentEntry).length > 0
                                          ? levelsOf(currentEntry).map(l => l.effort)
                                          : currentEntry && listsLevels ? [] : efforts
    // Whether the CLI's list gives levels at all: then a model it lists with
    // none (agy's orchestrator) offers none, rather than the fallback.
    readonly property bool listsLevels: models.some(m => levelsOf(m).length > 0)
    function chooseModel(id) {
        const entry = models.find(m => m.id === id)
        if (!entry)
            return
        const levels = levelsOf(entry).filter(l => l.id && l.id.length > 0)
        if (levels.length === 0) {
            modelChosen(entry.id, entry.id, "", entry.name)
            return
        }
        // A family: the level shown now if it has it, else medium, else its first.
        const shown = root.model ? root.model.shownEffort : ""
        const level = levels.find(l => l.effort === shown) || levels.find(l => l.effort === "medium") || levels[0]
        modelChosen(level.id, entry.id, level.effort, entry.name)
    }
    // Files to send with the next message, as local paths.
    property var attachments: []
    function attach(path) {
        if (path.length === 0 || attachments.indexOf(path) >= 0)
            return
        if (root.upload && !root.startUpload(path))
            return
        attachments = attachments.concat([path])
    }
    function detach(path) {
        attachments = attachments.filter(p => p !== path)
        root.forgetUpload(path)
    }

    // An agent on another machine: each attachment is copied there first
    // (Workspace.uploadAttachment) and its entry in `attachments` then
    // becomes the copy's path there, so message() never names a path on this
    // Mac. Until every copy is there the message stays in the bar.
    // `upload(path)` returns {token} or {error}; the host calls
    // uploadProgress and uploadFinished as the copy goes.
    property var upload: null
    property string uploadMachine: ""
    property var uploads: ({}) // shown path -> {token, local, progress, error}
    function startUpload(path) {
        const started = root.upload(path)
        if (!started || !started.token) {
            root.say(qsTr("Not attached: %1").arg(started && started.error ? started.error : path))
            return false
        }
        const next = Object.assign({}, uploads)
        next[path] = { token: started.token, local: path, progress: 0, error: "",
                       agent: root.agent, machine: root.uploadMachine }
        uploads = next
        return true
    }
    function forgetUpload(path) {
        if (!(path in uploads))
            return
        const next = Object.assign({}, uploads)
        delete next[path]
        uploads = next
    }
    function uploadEntry(token) {
        for (const shown in uploads)
            if (uploads[shown].token === token)
                return shown
        return ""
    }
    function uploadProgress(token, fraction) {
        const shown = uploadEntry(token)
        if (shown.length === 0)
            return
        const next = Object.assign({}, uploads)
        next[shown] = Object.assign({}, next[shown], { progress: fraction })
        uploads = next
    }
    function uploadFinished(token, remote, error) {
        const shown = uploadEntry(token)
        if (shown.length === 0)
            return
        const next = Object.assign({}, uploads)
        if (error.length > 0) {
            next[shown] = Object.assign({}, next[shown], { error: error })
            uploads = next
            root.say(qsTr("Not attached: %1 · remove it or attach it again").arg(error))
            return
        }
        const entry = next[shown]
        delete next[shown]
        next[remote] = Object.assign({}, entry, { token: "", progress: 1 })
        uploads = next
        attachments = attachments.map(p => p === shown ? remote : p)
    }
    // False, with the reason said, while a copy is on its way or failed, or
    // when an attachment was made for another agent: the bar is shared, and
    // a file here must never reach a remote agent (nor a copy there another).
    function attachmentsSendable() {
        for (const shown of attachments) {
            const entry = uploads[shown]
            const name = (entry ? entry.local : shown).split("/").pop()
            if (root.upload && (!entry || entry.agent !== root.agent)) {
                root.say(qsTr("Not sent: %1 is not on %2 · remove it and attach it again")
                         .arg(name).arg(root.uploadMachine))
                return false
            }
            if (!root.upload && entry) {
                root.say(qsTr("Not sent: %1 was copied for an agent on %2 · remove it")
                         .arg(name).arg(entry.machine))
                return false
            }
            if (entry && entry.error.length > 0) {
                root.say(qsTr("Not sent: %1 is not on %2 · remove it or attach it again")
                         .arg(entry.local.split("/").pop()).arg(root.uploadMachine))
                return false
            }
            if (entry && entry.token.length > 0) {
                root.say(qsTr("Not sent yet: copying %1 to %2 · the message stays here")
                         .arg(entry.local.split("/").pop()).arg(root.uploadMachine), true)
                return false
            }
        }
        return true
    }
    // Short model names, as the apps show them: "Opus 5.5", "Fable 5.1". A
    // model the picker does not know shows as its CLI reported it.
    function modelLabel(id) {
        const entry = modelEntry(id)
        if (entry && entry.name && entry.id !== "default" && (names(entry, id)))
            return entry.name
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
    // Who sent another agent's message: its title, or the machine it ran
    // on; with its short id and category for the line over the message.
    function peerName(peer) {
        return !peer ? "" : peer.title ? peer.title
             : peer.host ? qsTr("an agent on %1").arg(peer.host) : qsTr("a peer agent")
    }
    function peerCard(peer) {
        return [peerName(peer), peer ? peer.id : "", peer ? peer.category : ""]
            .filter(s => s && s.length > 0).join(" · ")
    }
    // A time of day ("9:21 pm") for a row, or empty for 0.
    function clockLabel(ms) {
        return ms > 0 ? Qt.formatTime(new Date(ms), "h:mm ap") : ""
    }
    // The quiet line under a prompt: queued, or read mid-turn (steered).
    function promptMark(entry) {
        if (!entry)
            return ""
        if (entry.queued)
            return pendingLabel(keyOf(entry.body))
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
    // running tasks' rows), how long it ran or has run, its steps or how it ended.
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

    // Escape asks to go back to the terminal (escape() decides: with
    // nothing typed while the agent works, it stops the turn instead).
    signal dismissed()
    // The guess shows in the empty bar (an impression); Tab put it in the
    // bar; a second Tab sent it unchanged. `offerKey` is the offer Tab took.
    signal suggestionShown()
    signal suggestionFilled(string offerKey)
    signal suggestionSent(string offerKey)
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
                                         borderColor: borderColor, quoteColor: mutedTextColor,
                                         headerColor: tableHeaderColor })
    // A table's header: the body's text a step toward the muted tone, so
    // its weight, not a second color, sets it apart.
    readonly property color tableHeaderColor: Qt.tint(textColor,
                                                      Qt.rgba(mutedTextColor.r, mutedTextColor.g, mutedTextColor.b, 0.5))
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

    // Escape in the thread: with nothing typed or attached while the agent
    // works, Stop (as the button); otherwise back to the terminal.
    function escapePressed() {
        if (input.text.length === 0 && attachments.length === 0 && sendButton.stops)
            stop()
        else
            dismissed()
    }
    Keys.onEscapePressed: escapePressed()

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
    // Following the end keeps the reply to the person's last prompt in view:
    // when only other agents' folded exchanges (peer rows) come after it
    // and they would push its end above the view, its end sits at the
    // view's bottom instead (anchored), the peer rows a short scroll below.
    // A turn of the person's own follows the end as always. Scrolling while
    // anchored, or the ↓ button, goes to the real end until the person's
    // next turn or another thread.
    property bool landOnPerson: true
    property bool anchored: false
    function followEnd() {
        list.positionViewAtEnd()
        anchored = false
        if (!model)
            return
        const at = model.personAnchor()
        if (at < 0) {
            landOnPerson = true // the person's turn is the newest again
            return
        }
        // A prompt the bar sent waits at the end until the CLI records it.
        if (!landOnPerson || model.pendingEntries.length > 0 || sentAt > 0)
            return
        const item = list.itemAtIndex(at)
        if (item && item.y + item.height > list.contentY + 1)
            return
        list.positionViewAtIndex(at, ListView.End)
        anchored = true
    }
    function placeView() {
        const view = model.view()
        following = view.following
        landOnPerson = true
        // Placed while it has no height, then given its height back: the
        // rows the screen shows are made, and none it does not.
        const place = () => {
            if (following) {
                root.followEnd()
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
            Qt.callLater(root.followEnd) // once rows have their heights
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
        onCountChanged: if (root.following) Qt.callLater(root.followEnd)
        onContentHeightChanged: if (root.following) Qt.callLater(root.followEnd)
        // Always drawn while the thread is taller than the view, so there is
        // something to see and grab; it brightens under the pointer.
        ScrollBar.vertical: ScrollBar {
            id: threadBar
            objectName: "visualScrollBar"
            policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            width: 14
            padding: 3
            minimumSize: 0.06
            // Following would pull the view back to the end mid-drag.
            onPressedChanged: {
                if (pressed && root.anchored)
                    root.landOnPerson = false
                root.anchored = false
                root.following = !pressed && list.atYEnd
            }
            contentItem: Rectangle {
                objectName: "visualScrollThumb"
                implicitWidth: 8
                radius: width / 2
                color: root.textColor
                opacity: threadBar.pressed ? 0.6 : threadBar.hovered ? 0.45 : 0.22
            }
            background: Rectangle {
                color: root.textColor
                opacity: threadBar.hovered || threadBar.pressed ? 0.06 : 0
            }
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
            DelegateChoice { roleValue: "peer"; delegate: Entry { block: peerBlock } }
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
        required property bool opened
        required property var peer
        property Component block

        // A turn's steps sit one under another, each line carrying its own
        // breathing room; its interim words and notes keep a gap.
        readonly property bool stepRow: inWork && (kind === "tool" || kind === "thinking")
        readonly property int gapAbove: index === 0 ? 0
                                      : stepRow ? 0
                                      : afterStep ? 2
                                      : inWork ? Math.round(root.fontSize * 0.5)
                                      // Another agent's exchanges stack close.
                                      : kind === "peer" ? Math.round(root.fontSize * 0.6)
                                      : kind === "prompt" ? (steered || peer ? root.fontSize : root.fontSize * 2)
                                      : kind === "work" || kind === "live" ? Math.round(root.fontSize * 0.9)
                                      : kind === "turn" ? 10 : root.fontSize
        readonly property int column: Math.min(root.columnWidth, row.width - 56)
        width: ListView.view ? ListView.view.width : 0
        height: loader.height + gapAbove
        // A row the list let go of (index -1), or a live header with no
        // running turn behind it, never draws: Qt can leave a released row
        // where it was, over the rows that took its place.
        visible: index >= 0 && (kind !== "live" || root.liveShown)
        // A reused command row starts closed, whatever the last item opened.
        ListView.onReused: if (loader.item && loader.item.open !== undefined) loader.item.open = false
        Loader {
            id: loader
            y: row.gapAbove
            width: row.column
            x: (row.width - row.column) / 2
            sourceComponent: row.block
            property var entry: row
        }
    }

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) { root.scrollThread(event) }
    }
    // Scrolls the thread by a wheel or trackpad event's vertical part (a
    // table scrolling sideways passes its events on here).
    function scrollThread(event) {
        const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                              : event.angleDelta.y / 120 * root.fontSize * 4
        if (step === 0)
            return
        const top = list.originY
        const bottom = Math.max(top, list.originY + list.contentHeight - list.height)
        // Scrolled from the person's reply: the end is where it goes.
        if (root.anchored)
            root.landOnPerson = false
        root.anchored = false
        list.contentY = Math.max(top, Math.min(bottom, list.contentY - step))
        root.following = list.contentY >= bottom - 2
    }
    // The widest a table may be: the thread's width less its margins, past
    // the reading column.
    readonly property int tableRoom: list.width - 56

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
        // The host may take a link first (the factory view's agent links).
        onLinkActivated: link => {
            if (root.openLink && root.openLink(link))
                return
            // A remote thread's links to files name that machine's: web only.
            if (root.model && root.model.remote && !/^(https?|mailto):/i.test(link))
                return
            Qt.openUrlExternally(link)
        }
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
        // Taller lists scroll (the Basic style's list turns interactive).
        property real maxHeight: 100000
        height: Math.min(implicitHeight, maxHeight)
        signal chosen(string id)
        // Opens above `item`, its bottom kept there while its list lays out.
        function openAbove(item) {
            parent = item
            x = 0
            y = Qt.binding(() => -height - 6)
            open()
        }
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
            // Another agent's message (lapis-peers), not the person's words:
            // its sender over a quieter, outlined bubble.
            readonly property var peer: entry && entry.peer ? entry.peer : null
            spacing: 4
            HoverHandler { id: promptHover }
            Text {
                objectName: "peerMessageFrom"
                anchors.right: parent.right
                anchors.rightMargin: 6
                visible: prompt.peer !== null
                text: prompt.peer ? qsTr("From %1").arg(root.peerCard(prompt.peer)) : ""
                color: root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.codeSize - 1
            }
            Rectangle {
                id: bubble
                anchors.right: parent.right
                width: Math.min(measure.implicitWidth + 36, parent.width * 0.8)
                height: promptText.height + 22
                radius: Math.round(root.fontSize * 1.1)
                color: prompt.peer ? "transparent" : root.raisedColor
                border.color: root.borderColor
                border.width: prompt.peer ? 1 : 0
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
                    color: prompt.peer ? root.mutedTextColor : root.textColor
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
                    sourceComponent: modelData.media ? mediaPiece : modelData.code ? codePiece
                                     : modelData.table ? tablePiece : prosePiece
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

    // A reply's table: columns fit to their text by the model (styleTable),
    // as wide as the text asks up to the thread's width, so a wide table
    // leaves the reading column centred on it; wider still, it scrolls
    // sideways in its frame and never squeezes a column below its minimum.
    // The header's tint and the rules between rows are drawn behind the
    // text, which stays one selectable document.
    Component {
        id: tablePiece
        Item {
            id: tableBox
            objectName: "replyTable"
            readonly property string source: parent && parent.piece ? parent.piece.text : ""
            readonly property real room: Math.max(width, root.tableRoom)
            property var fit: ({ width: 0, rows: [], header: false })
            readonly property bool fitted: fit.width > 0
            readonly property var rows: fitted ? fit.rows : []
            readonly property bool overflow: fitted && fit.width > room - 2
            readonly property real maxScroll: Math.max(0, fit.width - (frame.width - 2))
            property real scrollX: 0
            implicitHeight: frame.height
            function refit() {
                if (!root.model)
                    return
                fit = root.model.styleTable(cells.textDocument, source, root.proseStyle, room - 2)
                scrollX = Math.max(0, Math.min(scrollX, maxScroll))            }
            onSourceChanged: refit()
            onRoomChanged: refit()
            Component.onCompleted: refit()
            Connections {
                target: root
                function onStyleKeyChanged() { tableBox.refit() }
            }

            Item {
                id: frame
                objectName: "replyTableFrame"
                width: tableBox.fitted ? Math.min(tableBox.fit.width + 2, tableBox.room) : tableBox.width
                // In the reading column when it fits there, else centred on it.
                x: width <= tableBox.width ? 0 : Math.round((tableBox.width - width) / 2)
                height: cells.height + 2 + (tableBox.overflow ? sideBar.height : 0)
                readonly property int radius: 6

                // The header row: a faint tint of the text's color.
                Rectangle {
                    visible: tableBox.fit.header === true && tableBox.rows.length > 1
                    x: 1
                    y: 1 + (visible ? tableBox.rows[0] : 0)
                    width: frame.width - 2
                    height: visible ? tableBox.rows[1] - tableBox.rows[0] : 0
                    topLeftRadius: frame.radius - 1
                    topRightRadius: frame.radius - 1
                    color: root.tint(root.textColor, root.lightPage ? 0.035 : 0.05)
                }
                // A hairline between rows; under the header, a firmer one.
                Repeater {
                    model: Math.max(0, tableBox.rows.length - 2)
                    Rectangle {
                        required property int index
                        x: 1
                        y: 1 + Math.round(tableBox.rows[index + 1])
                        width: frame.width - 2
                        height: 1
                        color: index === 0 && tableBox.fit.header
                               ? Qt.tint(root.borderColor, root.tint(root.textColor, 0.22))
                               : root.tint(root.borderColor, 0.75)
                    }
                }
                Item {
                    x: 1
                    y: 1
                    width: frame.width - 2
                    height: cells.height
                    clip: tableBox.overflow
                    Selectable {
                        id: cells
                        objectName: "replyTableText"
                        x: -tableBox.scrollX
                        // The room, or the table when wider: the width the
                        // model laid the document out at, so it is not laid
                        // out again for a new width.
                        width: tableBox.fitted || tableBox.fit.rows !== undefined
                               ? Math.max(tableBox.fit.width, tableBox.room - 2) : frame.width - 2
                        textFormat: TextEdit.RichText
                    }
                }
                Rectangle {
                    visible: tableBox.fitted
                    anchors.fill: parent
                    radius: frame.radius
                    color: "transparent"
                    border.width: 1
                    border.color: root.borderColor
                }
                ScrollBar {
                    id: sideBar
                    objectName: "replyTableScrollBar"
                    visible: tableBox.overflow
                    orientation: Qt.Horizontal
                    x: 1
                    width: frame.width - 2
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 1
                    height: 12
                    padding: 3
                    policy: ScrollBar.AlwaysOn
                    size: tableBox.fitted ? (frame.width - 2) / tableBox.fit.width : 1
                    position: tableBox.fitted ? tableBox.scrollX / tableBox.fit.width : 0
                    onPositionChanged: if (pressed) tableBox.scrollX = Math.max(0, Math.min(tableBox.maxScroll, position * tableBox.fit.width))
                    contentItem: Rectangle {
                        implicitHeight: 6
                        radius: height / 2
                        color: root.textColor
                        opacity: sideBar.pressed ? 0.6 : sideBar.hovered ? 0.45 : 0.22
                    }
                }
                // Sideways gestures scroll the table; the up-and-down part
                // still scrolls the thread.
                WheelHandler {
                    target: null
                    enabled: tableBox.overflow
                    orientation: Qt.Horizontal
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: function(event) {
                        const step = event.pixelDelta.x !== 0 ? event.pixelDelta.x
                                                              : event.angleDelta.x / 120 * root.fontSize * 4
                        tableBox.scrollX = Math.max(0, Math.min(tableBox.maxScroll, tableBox.scrollX - step))
                        root.scrollThread(event)
                    }
                }
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

    // A tool call or a stretch of thinking: one quiet line that opens in
    // place. Whether it is opened is the model's (by item), so it stays
    // opened while the row is scrolled out and reused, and across switches.
    Component {
        id: stepBlock
        Column {
            id: step
            objectName: "step"
            readonly property var entry: parent ? parent.entry : null
            readonly property bool stepOpened: entry !== null && entry.opened

            StepRow {
                objectName: "stepRow"
                width: parent.width
                entry: step.entry
                opened: step.stepOpened
                onClicked: if (step.entry) root.toggleStep(step.entry.index)
            }

            // What opens on click is made on click: a collapsed step is one
            // line, an edit included (its diff shows only once opened).
            Loader {
                width: parent.width
                active: step.stepOpened
                visible: active
                sourceComponent: Component {
                    StepDetail { entry: step.entry }
                }
            }
        }
    }
    // Opens or closes a step in place. The view stops following the end, so
    // the row under the pointer stays where it is while its detail opens
    // under it; scrolling back to the end follows again.
    function toggleStep(row) {
        if (!model)
            return
        following = false
        model.toggleStep(row)
    }
    function togglePeer(row) {
        if (!model)
            return
        following = false
        model.togglePeer(row)
        Qt.callLater(() => {
            const bottom = Math.max(list.originY, list.originY + list.contentHeight - list.height)
            if (list.contentY > bottom)
                list.contentY = bottom
        })
    }
    function toggleWork(row) {
        if (!model)
            return
        following = false
        model.toggleWork(row)
        // Folded near the end, the thread can end above the view's bottom:
        // bring the end back down to the view's.
        Qt.callLater(() => {
            const bottom = Math.max(list.originY, list.originY + list.contentHeight - list.height)
            if (list.contentY > bottom)
                list.contentY = bottom
        })
    }

    // A step's one line: a small mark for its kind (a ring for thinking, a
    // dot for a call, a square for an edit, a diamond for a subagent or
    // background task, a gently pulsing accent dot while it runs), its verb
    // and target ("Ran ctest", "Edited greet.cpp", a thought's first
    // sentence), its counts or run, how long it took, and a chevron when it
    // opens. Used for the thread's steps, the running tasks' rows, and a
    // subagent's own steps.
    readonly property int stepLineHeight: Math.round(stepSize * 1.6)
    component StepRow: Item {
        id: stepRow
        property var entry: null
        property bool opened: false
        property bool withKind: false // a subagent's type and model too
        signal clicked()
        readonly property string kind: entry ? entry.kind : ""
        readonly property bool thinking: kind === "thinking"
        // A thought folds to its first sentence, dimmed, in place of "Thought".
        readonly property string thought: thinking ? root.thoughtLine(entry.body) : ""
        readonly property var task: entry && entry.task ? entry.task : null
        readonly property bool edit: entry !== null && ((entry.diff && entry.diff.length > 0)
                                                        || entry.added > 0 || entry.removed > 0)
        readonly property bool running: kind === "tool" && (task ? task.running : !entry.answered)
        readonly property bool failed: entry !== null && entry.failed
        readonly property bool expandable: entry !== null
                                           && (thinking ? entry.body.length > 0
                                               : kind !== "tool" || task !== null || entry.result.length > 0
                                                 || entry.detail.length > 0 || edit || entry.title === "Ran"
                                                 || entry.body.indexOf("\n") >= 0)
        // A thought, a reply, a subagent's description and what a monitor
        // watches read as prose; commands, files and patterns as code.
        readonly property bool prose: kind === "reply" || thought.length > 0
                                      || (task !== null && task.type !== "background")
        implicitHeight: root.stepLineHeight
        height: implicitHeight
        MouseArea {
            id: stepHover
            anchors.fill: parent
            hoverEnabled: true
            enabled: stepRow.expandable
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: stepRow.clicked()
        }
        Row {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            spacing: Math.round(root.stepSize * 0.4)
            KindMark {
                id: mark
                anchors.verticalCenter: parent.verticalCenter
                kind: stepRow.thinking ? "thinking"
                    : stepRow.task ? "task"
                    : stepRow.edit ? "edit"
                    : stepRow.kind === "reply" ? "reply" : "tool"
                running: stepRow.running
                failed: stepRow.failed
            }
            Text {
                id: stepVerb
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                // "Ran", "Subagent ·", "Started in background ·"
                text: !stepRow.entry || stepRow.kind === "reply" || stepRow.thought.length > 0 ? ""
                      : stepRow.entry.title + (stepRow.task ? " ·" : "")
                color: stepRow.failed ? root.faultColor
                     : stepHover.containsMouse ? root.textColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 1
            }
            Text {
                id: stepTarget
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - mark.width - stepVerb.width - stepCounts.width
                                               - stepInfo.width - stepChevron.width - 5 * parent.spacing)
                text: stepRow.thought.length > 0 ? stepRow.thought : root.stepTarget(stepRow.entry)
                elide: Text.ElideRight
                textFormat: Text.PlainText
                color: stepRow.thought.length > 0
                       ? (stepHover.containsMouse ? root.textColor : root.tint(root.mutedTextColor, 0.9))
                     : stepRow.failed ? root.tint(root.faultColor, 0.85)
                     : root.tint(root.textColor, stepHover.containsMouse ? 0.95 : stepRow.prose ? 0.78 : 0.82)
                font.family: stepRow.prose ? root.sansFamily : root.monoFamily
                font.pixelSize: stepRow.prose ? root.stepSize - 1 : root.codeSize - 1
            }
            Row {
                id: stepCounts
                visible: stepRow.entry !== null && (stepRow.entry.added > 0 || stepRow.entry.removed > 0)
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Text { text: stepRow.entry ? "+" + stepRow.entry.added : ""; visible: stepRow.entry !== null && stepRow.entry.added > 0; color: root.addedColor; font.pixelSize: root.codeSize - 1; font.family: root.monoFamily }
                Text { text: stepRow.entry ? "−" + stepRow.entry.removed : ""; visible: stepRow.entry !== null && stepRow.entry.removed > 0; color: root.removedColor; font.pixelSize: root.codeSize - 1; font.family: root.monoFamily }
            }
            // A task's run ("4m 10s · 23 steps"), else how long the step
            // took once it took a second or more.
            Text {
                id: stepInfo
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: !stepRow.entry ? ""
                      : stepRow.task ? root.taskLine(stepRow.task, stepRow.withKind).slice(1)
                      : (stepRow.kind === "tool" || stepRow.thought.length > 0) && stepRow.entry.duration >= 1000
                      ? root.durationLabel(stepRow.entry.duration) : ""
                color: root.tint(root.mutedTextColor, 0.75)
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 2
                font.features: { "tnum": 1 }
            }
            Text {
                id: stepChevron
                objectName: "stepChevron"
                visible: stepRow.expandable
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                rotation: stepRow.opened ? 90 : 0
                Behavior on rotation { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                color: stepHover.containsMouse || stepRow.opened ? root.textColor
                                                                 : root.tint(root.mutedTextColor, 0.6)
                font.family: root.sansFamily
                font.pixelSize: root.stepSize
            }
        }
    }

    // A step's kind as a small shape in the mark column.
    component KindMark: Item {
        id: kindMark
        property string kind: "tool"
        property bool running: false
        property bool failed: false
        readonly property color tone: failed ? root.faultColor : root.tint(root.mutedTextColor, 0.75)
        width: Math.round(root.stepSize * 0.7)
        height: width
        Rectangle {
            readonly property int size: kindMark.running ? 7
                                      : kindMark.kind === "thinking" ? 8
                                      : kindMark.kind === "edit" ? 6
                                      : kindMark.kind === "task" ? 7
                                      : kindMark.kind === "reply" ? 6 : 5
            anchors.centerIn: parent
            width: kindMark.kind === "reply" && !kindMark.running ? size + 3 : size
            height: kindMark.kind === "reply" && !kindMark.running ? 2 : size
            radius: kindMark.running || kindMark.kind === "thinking" || kindMark.kind === "tool" ? size / 2
                  : kindMark.kind === "reply" ? 1 : 1.5
            rotation: kindMark.kind === "task" && !kindMark.running ? 45 : 0
            color: kindMark.running ? root.activityColor
                 : kindMark.kind === "thinking" || kindMark.kind === "task" ? "transparent" : kindMark.tone
            border.width: !kindMark.running && (kindMark.kind === "thinking" || kindMark.kind === "task") ? 1.3 : 0
            border.color: kindMark.tone
            SequentialAnimation on opacity {
                running: kindMark.running && kindMark.visible
                loops: Animation.Infinite
                alwaysRunToEnd: true // stops at full opacity
                NumberAnimation { to: 0.35; duration: 800; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1; duration: 800; easing.type: Easing.InOutSine }
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
        // The rule runs down from the step's mark; the detail lines up with
        // its verb.
        Rectangle {
            objectName: "detailRule"
            x: Math.round(root.stepSize * 0.35)
            y: stepDetail.gapAbove
            width: 1
            height: detail.height
            color: root.borderColor
        }
        Column {
        id: detail
        x: Math.round(root.stepSize * 0.7) + Math.round(root.stepSize * 0.4)
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

    // What an opened subagent or background task shows: how it stands, what
    // it was asked or runs, then a subagent's own steps as step rows (each
    // opens in place under it; its own subagents stay lines) and its final
    // report, or a background command's last lines of output. Read from its
    // files when opened, and again every two seconds while it runs.
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
        // Its newest steps only: the rows are remade each time it is read
        // again while it runs.
        readonly property int shownFrom: Math.max(0, steps.length - 150)
        Text {
            visible: taskDetail.subagent && taskDetail.shownFrom > 0
            text: taskDetail.shownFrom === 1 ? qsTr("1 earlier step") : qsTr("%1 earlier steps").arg(taskDetail.shownFrom)
            color: root.tint(root.mutedTextColor, 0.7)
            font.family: root.sansFamily
            font.pixelSize: root.stepSize - 2
        }
        Column {
            objectName: "subagentSteps"
            visible: taskDetail.subagent && taskDetail.steps.length > 0
            width: parent.width
            Repeater {
                model: taskDetail.subagent ? taskDetail.steps.slice(taskDetail.shownFrom) : []
                delegate: Column {
                    id: subStep
                    required property var modelData
                    required property int index
                    readonly property bool opened: taskDetail.openStep === taskDetail.shownFrom + index
                    width: parent ? parent.width : 0
                    StepRow {
                        objectName: "subagentStep"
                        width: parent.width
                        entry: subStep.modelData
                        opened: subStep.opened
                        onClicked: {
                            root.following = false
                            taskDetail.openStep = subStep.opened ? -1 : taskDetail.shownFrom + subStep.index
                        }
                    }
                    Loader {
                        width: parent.width
                        active: subStep.opened
                        visible: active
                        property var subEntry: subStep.modelData
                        // By id, so StepDetail and TaskDetail do not hold each other.
                        sourceComponent: nestedStepDetail
                    }
                }
            }
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

    // Subagents and background tasks still running, one step row each
    // ("Subagent · review the parser · general-purpose · 3m 12s · 14
    // steps", "Background · npm test · 1m 05s") at the thread's end. More
    // than four collapse to one line ("3 subagents and 2 background tasks
    // running") that shows the rows. A row opens its task in place under
    // it; finished ones leave.
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
        readonly property bool many: tasks.length > 4
        visible: tasks.length > 0
        Item {
            objectName: "runningSummary"
            visible: strip.many
            width: parent.width
            height: root.stepLineHeight
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: strip.expanded = !strip.expanded
            }
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(root.stepSize * 0.4)
                KindMark {
                    anchors.verticalCenter: parent.verticalCenter
                    running: true
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
        Column {
            objectName: "runningStrip"
            visible: !strip.many || strip.expanded
            width: parent.width
            Repeater {
                model: strip.tasks
                delegate: Column {
                    id: stripTask
                    required property var modelData
                    readonly property bool opened: root.stripOpenItem === modelData.item
                    width: parent ? parent.width : 0
                    StepRow {
                        objectName: "runningTask"
                        width: parent.width
                        withKind: true
                        entry: ({ kind: "tool", title: stripTask.modelData.type === "subagent" ? qsTr("Subagent") : qsTr("Background"),
                                  body: stripTask.modelData.description, task: stripTask.modelData, answered: true,
                                  failed: false, added: 0, removed: 0, duration: 0, result: "", detail: "" })
                        opened: stripTask.opened
                        onClicked: {
                            root.following = false
                            root.stripOpenItem = stripTask.opened ? -1 : stripTask.modelData.item
                        }
                    }
                    Loader {
                        width: parent.width
                        active: stripTask.opened && root.stripDetail !== null
                        visible: active
                        sourceComponent: Component {
                            StepDetail { entry: root.stripDetail }
                        }
                    }
                }
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
        implicitWidth: busyRow.implicitWidth
        Row {
            id: busyRow
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

    // The running turn's header, while the agent works: its state line
    // ("Working · 2m 10s · 14 steps") over its steps, which are thread rows
    // like a finished turn's, added as the CLI records them, the newest at
    // the bottom. Clicking it folds or shows those rows. It is the
    // transcript's own record, never the terminal's screen.
    Component {
        id: liveBlock
        Item {
            id: live
            objectName: "liveHeader"
            readonly property var entry: parent ? parent.entry : null
            readonly property bool expanded: entry !== null && entry.expanded
            height: liveLine.height
            MouseArea {
                id: liveHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (live.entry) root.toggleWork(live.entry.index)
            }
            Row {
                spacing: 8
                BusyLine {
                    id: liveLine
                    width: implicitWidth
                    steps: live.entry ? live.entry.steps : -1
                }
                Chevron {
                    anchors.verticalCenter: parent.verticalCenter
                    turned: live.expanded
                    color: liveHover.containsMouse ? root.textColor : root.mutedTextColor
                }
            }
        }
    }
    // The running turn's newest step is done (or is not a tool call).
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
        id: pendingPrompt
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
        // How the CLI will take it, and for a waiting one not yet sent on,
        // the CLI's way to send it now.
        Row {
            id: pendingNote
            anchors.right: parent.right
            anchors.rightMargin: 6
            spacing: 10
            readonly property bool waiting: !pendingPrompt.unsent && pendingPrompt.modelData.queued
            Text {
                objectName: "pendingSendNow"
                anchors.verticalCenter: parent.verticalCenter
                visible: pendingNote.waiting && root.sendsNow && !root.urged[pendingPrompt.modelData.key]
                text: root.sendNowLabel
                color: sendNowMouse.containsMouse ? root.textColor : root.accentColor
                font.family: root.sansFamily
                font.pixelSize: root.codeSize - 1
                MouseArea {
                    id: sendNowMouse
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.sendQueuedNow()
                }
            }
            Text {
                objectName: "pendingLabel"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                text: pendingPrompt.unsent ? qsTr("Not sent yet · press Return in the terminal")
                    : pendingPrompt.modelData.queued ? root.pendingLabel(pendingPrompt.modelData.key) : ""
                color: pendingPrompt.unsent ? root.faultColor : root.mutedTextColor
                font.family: root.sansFamily
                font.pixelSize: root.codeSize - 1
            }
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
                // With no running turn's header: the agent busy, or idle
                // while its subagents and background tasks run ("Waiting on ...").
                BusyLine {
                    visible: (root.busy || root.waitingOnWork) && !root.liveShown
                    width: parent.width
                }
                // Under a running turn's steps: Claude Code writes a step as
                // it is called and its result when done, so after a finished
                // step the model is thinking; other CLIs write steps once done.
                Row {
                    objectName: "liveHint"
                    readonly property bool thinking: root.model !== null && root.model.format === "claude"
                                                     && root.liveLastAnswered && !root.liveLastThought
                    readonly property bool none: root.liveSteps !== null && root.liveSteps.count === 0
                    visible: root.liveShown && !root.compacting && (thinking || none)
                    height: root.stepLineHeight
                    spacing: Math.round(root.stepSize * 0.4)
                    KindMark {
                        anchors.verticalCenter: parent.verticalCenter
                        running: true
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: parent.none ? qsTr("Waiting for the first step…") : qsTr("Thinking…")
                        color: root.tint(root.mutedTextColor, 0.7)
                        font.family: root.sansFamily
                        font.pixelSize: root.stepSize - 1
                    }
                }
                RunningStrip {
                    width: parent.width
                }
                Repeater {
                    model: root.model ? root.model.pendingEntries.filter(p => p.queued) : []
                    delegate: PendingPrompt {}
                }
            }
        }
    }

    // A finished turn's header: "Worked for 3m 12s", one plain line over its
    // steps. Clicking it folds or shows them as a group; folded, it says
    // what the work did, as the Codex app sums it up ("ran 4 commands,
    // edited 2 files"; a turn of only thinking counts its steps).
    Component {
        id: workBlock
        Item {
            id: work
            objectName: "workHeader"
            readonly property var entry: parent ? parent.entry : null
            readonly property bool expanded: entry !== null && entry.expanded
            height: Math.max(workText.implicitHeight, root.stepSize + 10)
            MouseArea {
                id: workHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (work.entry) root.toggleWork(work.entry.index)
            }
            Row {
                id: workRow
                anchors.verticalCenter: parent.verticalCenter
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
                    turned: work.expanded
                    color: workHover.containsMouse ? root.textColor : root.mutedTextColor
                }
            }
            Text {
                id: workSummary
                objectName: "workSummary"
                visible: !work.expanded && text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                x: workRow.width + 12
                width: Math.max(0, parent.width - x)
                elide: Text.ElideRight
                text: !work.entry || work.entry.steps <= 0 ? ""
                      : work.entry.detail !== "" ? work.entry.detail
                      : work.entry.steps === 1 ? qsTr("1 step") : qsTr("%1 steps").arg(work.entry.steps)
                color: root.tint(root.mutedTextColor, 0.7)
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 2
            }
            // Shown, a hairline runs on from the header.
            Rectangle {
                visible: work.expanded
                anchors.verticalCenter: parent.verticalCenter
                x: workRow.width + 14
                width: Math.max(0, parent.width - x)
                height: 1
                color: root.borderColor
            }
        }
    }

    // Another agent's exchange (a lapis-peers message, the work it caused
    // and the agent's answer) folded to one quiet line: who sent it, when,
    // and the answer's first line ("working…" while the turn runs). It sits
    // under the person's own thread as a work header does; clicking it shows
    // the whole exchange in place under it, and again folds it.
    Component {
        id: peerBlock
        Item {
            id: peerRow
            objectName: "peerRow"
            readonly property var entry: parent ? parent.entry : null
            readonly property var peer: entry && entry.peer ? entry.peer : null
            readonly property bool expanded: entry !== null && entry.expanded
            height: Math.max(peerFrom.implicitHeight, root.stepSize + 10)
            MouseArea {
                id: peerHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (peerRow.entry) root.togglePeer(peerRow.entry.index)
            }
            Row {
                id: peerLead
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Text {
                    id: peerFrom
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("From %1").arg(root.peerName(peerRow.peer))
                    color: peerHover.containsMouse ? root.textColor : root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize - 1
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: text.length > 0
                    text: peerRow.entry ? root.clockLabel(peerRow.entry.at) : ""
                    color: root.tint(root.mutedTextColor, 0.7)
                    font.family: root.sansFamily
                    font.pixelSize: root.stepSize - 2
                    font.features: { "tnum": 1 }
                }
                Chevron {
                    anchors.verticalCenter: parent.verticalCenter
                    turned: peerRow.expanded
                    color: peerHover.containsMouse ? root.textColor : root.mutedTextColor
                }
            }
            Text {
                objectName: "peerReply"
                visible: !peerRow.expanded
                anchors.verticalCenter: parent.verticalCenter
                x: peerLead.width + 12
                width: Math.max(0, parent.width - x)
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: !peerRow.peer || !peerRow.entry ? ""
                      : peerRow.peer.working ? qsTr("working…")
                      : peerRow.entry.body.length > 0 ? peerRow.entry.body : qsTr("no reply")
                color: root.tint(root.mutedTextColor, 0.7)
                font.family: root.sansFamily
                font.pixelSize: root.stepSize - 2
            }
            // Shown, a hairline runs on from the line, as under a work header.
            Rectangle {
                visible: peerRow.expanded
                anchors.verticalCenter: parent.verticalCenter
                x: peerLead.width + 14
                width: Math.max(0, parent.width - x)
                height: 1
                color: root.borderColor
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
    // What Tab put in the bar and the offer it came from, until it is sent,
    // edited at all, or another agent shows. Taking the guess retires its
    // offer (NextPrompt::used), so `suggestion` is empty by the second Tab:
    // that Tab compares the bar with this, never with the suggestion.
    property string filledText: ""
    property string filledKey: ""
    readonly property bool filledFromSuggestion: filledText.length > 0 && input.text === filledText
                                                 && attachments.length === 0
    // The guess is on screen in the empty bar of the active window.
    readonly property bool offerShown: suggestion.length > 0 && !waiting && input.text.length === 0
                                       && input.preeditText.length === 0 && visible && Window.active
    onOfferShownChanged: if (offerShown) suggestionShown()
    onSuggestionKeyChanged: if (offerShown) suggestionShown()
    // Tab, as one key for the whole round: into an empty bar it puts the
    // guess (to edit); on that guess, unchanged, it sends it; on an empty bar
    // with nothing offered it moves to the next agent that needs the person,
    // whose own guess is then offered, so Tab goes on from there. Text the
    // person edited waits for Return. A held key's repeats do nothing, and a
    // composition (IME) in progress keeps Tab. A request is never answered:
    // the bar offers no guess over one and sends nothing to it.
    function tabStep(repeat) {
        if (repeat || input.preeditText.length > 0)
            return
        if (filledFromSuggestion) {
            if (waiting) {
                say(qsTr("Answer the request in the terminal first (Esc)"))
                return
            }
            const key = filledKey
            if (sendMessage(false))
                suggestionSent(key)
            return
        }
        if (input.text.length > 0 || attachments.length > 0)
            return
        if (suggestion.length > 0 && !waiting) {
            const key = suggestionKey
            suggestionShown() // what Tab took was on screen
            input.text = suggestion
            input.cursorPosition = input.text.length
            filledText = input.text
            filledKey = key
            suggestionFilled(key)
            return
        }
        if (tabAway && !tabAway())
            say(qsTr("Nothing else is waiting"), true)
    }
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
    function send() { sendMessage(false) }
    // `now`: Command-Return, the message taken soonest (see urge); an
    // ordinary send when the agent is idle or for a slash command. Returns
    // whether the message went to the agent.
    function sendMessage(now) {
        if (!root.attachmentsSendable()) return false // remote copies (startUpload)
        const text = message()
        if (text.trim().length === 0 || !root.submit)
            return false
        if (root.starting) {
            root.say(qsTr("Not sent: %1 is still starting · the message stays here").arg(root.cliName()))
            return false
        }
        // A dialog in the terminal (a picker, settings) would take these
        // keys as its own, so show it instead and keep the message here.
        if (!root.waiting && root.slashCommands && root.screen && root.model
                && root.model.dialogShowing(root.screen())) {
            root.dialogFound()
            return false
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
        if (!root.waiting && root.tookLeftover())
            return false
        if (now === true && working && root.cli === "antigravity" && root.model)
            root.model.refreshQueueMode()
        const urgent = now === true && working && command.length === 0 && root.sendsNow
        // agy sends now by Alt-M on text in its box, with no Return.
        const typed = urgent && root.cli === "antigravity"
        if (typed ? (root.typeText !== null && root.typeText(text)) : root.submit(text)) {
            // The prompt shows at once, at the thread's end, until the CLI
            // records it (queued when the agent was working), and is then
            // checked for having been taken.
            if (command.length === 0 && root.model) {
                root.model.noteSent(text, working)
                if (!working)
                    root.sentAt = Date.now()
                root.checkingText = text
                root.checkingDraft = { text: input.text, attachments: root.attachments.slice() }
                root.retried = false
                root.unsentText = ""
                sendCheck.restart()
                if (urgent)
                    root.urge(text)
            }
            if (uncovers)
                root.terminalNeeded()
            if (command.length > 0 && !uncovers) {
                root.pendingCommand = command
                root.pendingSince = Date.now()
                root.quietTicks = 0
                root.commandSent(text.trim())
            }
            root.filledText = ""
            root.filledKey = ""
            root.attachments = []
            input.clear()
            root.following = true
            root.landOnPerson = true
            root.followEnd()
            root.notice = ""
            return true
        }
        root.say(root.waiting ? qsTr("Answer the request in the terminal first (Esc)")
                              : qsTr("Not sent: the agent is not ready for input"))
        return false
    }
    Timer { id: noticeTimer; interval: 4000; onTriggered: root.notice = "" }

    Item {
        id: composerArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.composerInset + composerBox.height + Math.round(root.fontSize * 0.8) + hints.height + 6

        // Room the host keeps above the composer (the factory view's gate
        // strip); what it puts in `accessory` is laid over it.
        Item {
            id: accessorySlot
            objectName: "visualComposerAccessory"
            x: composerBox.x
            width: composerBox.width
            height: Math.max(0, root.composerInset - 6)
        }

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
            y: root.composerInset
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
                        // A copy on a remote agent's machine: the file here
                        // gives its picture and name.
                        readonly property var copy: root.uploads[modelData] || null
                        readonly property string local: copy ? copy.local : modelData
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
                                source: chip.image ? "file://" + chip.local : ""
                                sourceSize.height: 64
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: chip.local.split("/").pop()
                                width: Math.min(implicitWidth, 220)
                                elide: Text.ElideMiddle
                                color: root.textColor
                                font.family: root.sansFamily
                                font.pixelSize: root.codeSize
                            }
                            // Copying to the agent's machine: how far, or that it failed.
                            Text {
                                objectName: "attachmentCopy"
                                anchors.verticalCenter: parent.verticalCenter
                                visible: chip.copy !== null && (chip.copy.token.length > 0
                                                                || chip.copy.error.length > 0)
                                text: !chip.copy ? ""
                                      : chip.copy.error.length > 0 ? qsTr("not copied")
                                      : qsTr("copying %1%").arg(Math.round(chip.copy.progress * 100))
                                color: chip.copy && chip.copy.error.length > 0 ? root.faultColor
                                                                               : root.mutedTextColor
                                font.family: root.sansFamily
                                font.pixelSize: root.codeSize - 1
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
                    // Any edit, even one undone, makes it the person's text.
                    onTextChanged: if (text !== root.filledText) { root.filledText = ""; root.filledKey = "" }
                    Keys.onPressed: function(event) {
                        // AppKit marks arrows as keypad keys; lapis keeps
                        // Command as Meta and Control as Control.
                        const mods = event.modifiers & ~Qt.KeypadModifier
                        const plain = mods === Qt.NoModifier
                        const isReturn = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        // Tab's round (tabStep); a guess that is a slash
                        // command still sends on the second Tab.
                        if (event.key === Qt.Key_Tab && plain
                                && (root.slashMatches.length === 0 || root.filledFromSuggestion)) {
                            root.tabStep(event.isAutoRepeat)
                            event.accepted = true
                            return
                        }
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
                        if (isReturn && plain) {
                            root.send()
                            event.accepted = true
                        } else if (isReturn && mods === Qt.MetaModifier) {
                            root.sendNow()
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
                    // A choice shows at once, with "…" until the CLI
                    // confirms it (TranscriptModel::choicePending).
                    BarButton {
                        objectName: "visualModel"
                        visible: root.model !== null && (root.model.shownModel.length > 0 || root.models.length > 0)
                        label: root.modelLabel(root.model ? root.model.shownModel : "")
                               + (root.model && root.model.choicePending ? "…" : "")
                        menu: root.models.length > 0
                        menuOpen: modelMenu.opened
                        onTriggered: if (root.models.length > 0) {
                            root.pickerOpened()
                            modelMenu.openAbove(this)
                        }
                    }
                    BarButton {
                        objectName: "visualEffort"
                        visible: root.currentEfforts.length > 0 || (root.model !== null && root.model.shownEffort.length > 0)
                        label: (root.model && root.model.shownEffort.length > 0
                                ? root.effortLabel(root.model.shownEffort) + (root.model.choicePending ? "…" : "")
                                : qsTr("Effort"))
                        menu: root.currentEfforts.length > 0
                        menuOpen: effortMenu.opened
                        onTriggered: if (root.currentEfforts.length > 0) effortMenu.openAbove(this)
                    }
                    // Why the CLI refused the last choice, in its own words.
                    Text {
                        objectName: "visualChoiceRefusal"
                        visible: root.model !== null && root.model.choiceRefusal.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(80, toolbar.width * 0.35))
                        elide: Text.ElideRight
                        text: root.model ? root.model.choiceRefusal : ""
                        color: root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: root.codeSize
                    }
                }
                ChoiceMenu {
                    id: modelMenu
                    objectName: "visualModelMenu"
                    // Every entry; a long list scrolls.
                    maxHeight: Math.max(160, root.height * 0.6)
                    // The CLI's own default entry carries a sentence for a
                    // name; it is just "Default" here.
                    choices: root.models.map(m => ({ id: m.id, label: m.id === "default" ? qsTr("Default")
                                                                                         : (m.name || m.id) }))
                    current: root.currentEntry ? root.currentEntry.id : ""
                    onChosen: id => root.chooseModel(id)
                }
                ChoiceMenu {
                    id: attachMenu
                    choices: [{ id: "files", label: qsTr("Files…") }, { id: "folder", label: qsTr("Folder…") }]
                    onChosen: id => id === "folder" ? folderPicker.open() : filePicker.open()
                }
                ChoiceMenu {
                    id: effortMenu
                    objectName: "visualEffortMenu"
                    choices: root.currentEfforts.map(e => ({ id: e, label: root.effortLabel(e) }))
                    current: root.model ? root.model.shownEffort : ""
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
                    anchors.right: midTurnRow.left
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
                // Who wrote the offered guess, in the guess's own tint, before
                // the keys that take it; nothing when that is not known.
                Text {
                    id: suggestionAuthorText
                    objectName: "visualSuggestionAuthor"
                    anchors.left: controls.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: visible ? Math.min(implicitWidth, parent.width / 3) : 0
                    visible: root.suggestionAuthor.length > 0 && root.suggestion.length > 0
                             && root.notice.length === 0 && !root.waiting && input.text.length === 0
                    text: qsTr("from %1").arg(root.suggestionAuthor)
                    elide: Text.ElideRight
                    color: root.tint(root.textColor, 0.62)
                    font.family: root.sansFamily
                    font.pixelSize: Math.max(11, root.codeSize - 1)
                }
                Text {
                    objectName: "visualHint"
                    anchors.left: suggestionAuthorText.visible ? suggestionAuthorText.right : controls.right
                    anchors.leftMargin: suggestionAuthorText.visible ? 14 : 8
                    anchors.right: contextMeter.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: root.notice.length > 0 ? root.notice
                        : root.waiting ? (root.waitingReason.length > 0 ? root.waitingReason
                                                                         : qsTr("The agent is waiting on a request"))
                        : root.suggestion.length > 0 && input.text.length === 0
                          ? qsTr("Tab to use · again to send · then the next agent")
                        : root.filledFromSuggestion ? qsTr("Tab or Return to send")
                        : ""
                    color: (root.notice.length > 0 && !root.noticeQuiet) || root.waiting
                           ? root.faultColor : root.mutedTextColor
                    font.family: root.sansFamily
                    font.pixelSize: Math.max(11, root.codeSize - 1)
                }
                // Mid-turn, beside Stop: "Send now" for what is typed, and
                // what Stop does to a message already waiting.
                Row {
                    id: midTurnRow
                    readonly property bool offersSendNow: root.sendsNow && root.canSend && !root.waiting
                                                          && (input.text.trim().length > 0
                                                              || root.attachments.length > 0)
                    readonly property bool notesStop: sendButton.stops && root.messageWaiting && !offersSendNow
                    anchors.right: sendButton.left
                    anchors.rightMargin: visible ? 6 : 0
                    anchors.verticalCenter: parent.verticalCenter
                    visible: notesStop || offersSendNow
                    width: visible ? implicitWidth : 0
                    spacing: 6
                    Text {
                        id: stopNote
                        objectName: "stopNote"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: midTurnRow.notesStop
                        text: root.stopLabel
                        color: root.mutedTextColor
                        font.family: root.sansFamily
                        font.pixelSize: Math.max(11, root.codeSize - 1)
                    }
                    BarButton {
                        id: sendNowButton
                        objectName: "visualSendNow"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: midTurnRow.offersSendNow
                        label: root.sendNowLabel
                        onTriggered: root.sendNow()
                    }
                }
                // Send, or Stop while a turn runs: Return sends (a message
                // typed mid-turn queues for the agent), so the button stops.
                Rectangle {
                    id: sendButton
                    objectName: "visualSend"
                    readonly property bool stops: root.working && root.interrupt !== null
                    Accessible.role: Accessible.Button
                    Accessible.name: stops ? root.stopLabel : qsTr("Send")
                    readonly property bool ready: stops
                                                  || ((input.text.trim().length > 0 || root.attachments.length > 0)
                                                      && root.canSend && !root.waiting && !root.starting)
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
                        onClicked: sendButton.stops ? root.stop() : root.send()
                    }
                }
            }
        }

        Text {
            id: hints
            anchors.top: composerBox.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: composerBox.horizontalCenter
            objectName: "visualHints"
            // Mid-turn: what Return, Command-Return and Escape do for this CLI.
            text: !root.midTurn || root.cli === ""
                  ? qsTr("Return to send · Shift-Return for a new line · ⌘-click a path to open it · Esc for the terminal")
                  : [root.queueMode === "steer" ? qsTr("Return to steer")
                     : root.queueMode === "immediate" ? qsTr("Return to send") : qsTr("Return to queue"),
                     root.sendsNow ? qsTr("⌘-Return to %1").arg(root.sendNowLabel.toLowerCase()) : "",
                     input.text.length > 0 || root.attachments.length > 0 ? qsTr("Esc for the terminal")
                     : root.messageWaiting ? qsTr("Esc: %1").arg(root.stopLabel.charAt(0).toLowerCase() + root.stopLabel.slice(1))
                     : qsTr("Esc to stop")].filter(s => s.length > 0).join(" · ")
            color: root.tint(root.mutedTextColor, 0.8)
            font.family: root.sansFamily
            font.pixelSize: Math.max(11, root.codeSize - 3)
        }
    }

    // The CLI's name for a person: what the empty thread and notes call it.
    function cliName() {
        return harness === "codex" || cli === "codex" ? qsTr("Codex")
             : harness === "agy" || cli === "antigravity" ? qsTr("Antigravity")
             : qsTr("Claude Code")
    }
    Text {
        visible: root.model !== null && root.model.count === 0 && !root.model.loading && !root.blank
        anchors.centerIn: parent
        text: qsTr("Nothing saved in this conversation yet")
        color: root.mutedTextColor
        font.family: root.sansFamily
        font.pixelSize: root.stepSize
    }
    // A new session: which CLI, where, and whether it is ready, until the
    // first prompt shows.
    Column {
        objectName: "visualBlankState"
        visible: root.blank && root.model !== null && root.model.count === 0
                 && root.model.pendingEntries.length === 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(0, (composerArea.y - height) / 2)
        width: Math.min(root.columnWidth, parent.width - 56)
        spacing: Math.round(root.fontSize * 0.4)
        Text {
            objectName: "visualBlankTitle"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.cliName()
            color: root.textColor
            font.family: root.sansFamily
            font.pixelSize: Math.round(root.fontSize * 1.15)
            font.weight: Font.DemiBold
        }
        Text {
            objectName: "visualBlankFolder"
            width: parent.width
            visible: root.folder.length > 0
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideMiddle
            text: root.folder
            color: root.mutedTextColor
            font.family: root.monoFamily
            font.pixelSize: root.codeSize
        }
        Text {
            objectName: "visualBlankStatus"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.blankStatus.length > 0 ? root.blankStatus
                  : root.starting ? qsTr("Starting…") : qsTr("Ready for your first message")
            color: root.tint(root.mutedTextColor, 0.75)
            font.family: root.sansFamily
            font.pixelSize: root.stepSize - 1
        }
    }

    // Back to the newest item; the view follows from there.
    Rectangle {
        objectName: "jumpToLatest"
        visible: (!root.following || root.anchored) && list.count > 0
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
            // To the real end, past other agents' exchanges too.
            onClicked: {
                root.landOnPerson = false
                root.anchored = false
                root.following = true
                list.positionViewAtEnd()
            }
        }
    }
}
