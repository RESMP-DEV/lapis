pragma ComponentBehavior: Bound

import QtQuick
import Qt.labs.folderlistmodel
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as NativeDialogs
import Lapis 1.0

ApplicationWindow {
    id: window

    width: 1400
    height: 960
    minimumWidth: 640
    minimumHeight: 480
    objectName: "workspaceWindow"
    color: backgroundColor
    title: windowTitle

    // Theme colors come from the keymap. Without one, the stage uses a dark
    // command-deck palette. Selection is a thin accent edge, not a second surface.
    // focusedBorderColor only marks where keyboard input goes; activity,
    // attention (pending requests) and fault (lost connection, errors) are
    // separate meanings and are always paired with a shape or text cue.
    readonly property var appearance: (typeof keymap !== "undefined" && keymap !== null) ?
                                          keymap.themes.find(function(theme) { return theme.name === keymap.themeName }) : null
    readonly property color backgroundColor: appearance ? appearance.background : "#070b10"
    readonly property color surfaceColor: appearance ? appearance.surface : "#101820"
    readonly property color cardColor: appearance ? appearance.card : "#15202b"
    readonly property color hoveredCardColor: appearance ? appearance.hoveredCard : "#1c2a38"
    readonly property color focusedColor: appearance ? appearance.focused : "#1a3040"
    readonly property color borderColor: appearance ? appearance.border : "#243140"
    readonly property color focusedBorderColor: appearance ? appearance.focusedBorder : "#7ddec8"
    readonly property color textColor: appearance ? appearance.text : "#e7eef4"
    readonly property color mutedTextColor: appearance ? appearance.mutedText : "#8ea0b3"
    readonly property color attentionColor: appearance ? appearance.attention : "#ff6b5a"
    readonly property color attentionTextColor: appearance ? appearance.attentionText : "#1a0d0b"
    readonly property color activityColor: appearance ? appearance.activity : "#74a8ff"
    readonly property color faultColor: appearance ? appearance.fault : "#ee7a8a"
    readonly property color plentyColor: appearance && appearance.plenty ? appearance.plenty : "#6fdc8c"
    readonly property color paneColor: backgroundColor
    readonly property int chromeRadius: appearance ? appearance.cornerRadius : 2
    readonly property int motionDuration: appearance ? appearance.motionDuration : 100
    // One fixed-width family for the terminal and every machine readout (paths,
    // shortcut hints, states, counts). Human names and prose use the UI face.
    readonly property string monoFamily: liveTerminal.resolvedFontFamily
    readonly property int terminalFontSize: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSize : 14
    // Composition and paste own the keyboard until they finish.
    readonly property bool terminalBusy: liveTerminal.composing || liveTerminal.pasting ||
                                        sideSurface.composing || sideSurface.pasting

    readonly property int uiFont: {
        // Use the window font so a larger system size still fits. Do not assign
        // font.pixelSize here; that would bind the size to itself.
        if (font.pixelSize > 0)
            return Math.max(12, font.pixelSize)
        if (font.pointSize > 0)
            return Math.max(12, Math.round(font.pointSize * 96 / 72))
        return 13
    }
    readonly property string densityMode: (typeof keymap !== "undefined" && keymap !== null) ?
                                              keymap.densityName : "comfortable"
    readonly property int chromeFont: densityMode === "minimal" ? Math.max(12, uiFont - 1) : uiFont
    readonly property int readoutFont: Math.max(11, chromeFont - 1)
    readonly property int railWidth: densityMode === "minimal" ? 148 : densityMode === "compact" ? 168 : 196
    readonly property int tabHeight: Math.max(32, chromeFont + (densityMode === "minimal" ? 16 : densityMode === "compact" ? 20 : 24))
    property bool transientSidebarVisible: true
    property bool transientPreviewsVisible: true
    readonly property bool previewsEnabled: typeof keymap !== "undefined" && keymap !== null ? keymap.previewsVisible : transientPreviewsVisible
    // The agent strip under the stage is the category's navigation: live
    // previews in tab order. Short windows get shorter cards, not fewer.
    readonly property int stripHeight: height < 640 ? 96 :
                                       densityMode === "minimal" ? 112 : densityMode === "compact" ? 128 : 144
    readonly property bool stripShown: previewsEnabled && workspace.categorySessions.length > 0
    readonly property bool sidebarExpanded: typeof keymap !== "undefined" && keymap !== null ? keymap.sidebarVisible : transientSidebarVisible
    readonly property bool narrow: width < 860
    // Zero-duration themes and reduced motion change state instantly.
    readonly property bool motionEnabled: !preview.reducedMotion && active && visible && motionDuration > 0
    readonly property bool inputBlocked: transitionLock
            || commandsDialog.visible
            || resumeDialog.visible
            || terminalPicker.visible
            || toolsDialog.visible
            || settingsDialog.visible
            || attentionDialog.visible
            || agentDialog.visible
            || closeAgentDialog.visible
            || categoryDialog.visible
            || renameAgentDialog.visible
            || agentMenu.visible
            || categoryMenu.visible
            || (categorySelector.popup !== null && categorySelector.popup.visible)
    // Dialogs block input. Composition and paste do not open a dialog, but they
    // still own the keyboard, so shortcuts and chrome clicks stay inert.
    readonly property bool interactionArmed: !inputBlocked && !terminalBusy
    readonly property bool shortcutsArmed: interactionArmed
    readonly property string activeCategoryName: {
        const id = workspace.activeCategoryId
        const list = workspace.categories
        for (let i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return list[i].name
        }
        return ""
    }
    readonly property string windowTitle: {
        const session = workspace.focusedSession
        if (session && activeCategoryName.length > 0)
            return "lapis · " + activeCategoryName + " · " + window.agentTabTitle(session)
        if (activeCategoryName.length > 0)
            return "lapis · " + activeCategoryName
        return "lapis"
    }

    property bool transitionLock: false
    property var pendingDialog: null

    palette.window: surfaceColor
    palette.windowText: textColor
    palette.text: textColor
    palette.button: cardColor
    palette.buttonText: textColor
    palette.base: backgroundColor
    palette.mid: borderColor
    palette.highlight: focusedColor
    palette.highlightedText: textColor
    palette.placeholderText: mutedTextColor
    palette.toolTipBase: cardColor
    palette.toolTipText: textColor

    signal keyboardOwnershipReady()

    function bindings(action) {
        if (typeof keymap !== "undefined" && keymap !== null) {
            const configured = keymap.shortcutBindings[action]
            if (configured && configured.length > 0)
                return configured
        }
        return fallbackSequences(action)
    }

    function fallbackSequences(action) {
        const mac = Qt.platform.os === "osx"
        const mod = mac ? "Meta+" : "Ctrl+Shift+"
        if (action === "quit")
            return [mod + "Q"]
        if (action === "closeAgent")
            return [mod + "W"]
        if (action === "closeWindow")
            return mac ? ["Meta+Shift+W"] : []
        if (action === "minimizeWindow")
            return mac ? ["Meta+M"] : []
        if (action === "nextCategory")
            return [mod + "Alt+Right", mod + (mac ? "Shift+Down" : "Down")]
        if (action === "previousCategory")
            return [mod + "Alt+Left", mod + (mac ? "Shift+Up" : "Up")]
        const numbered = /^category([1-9])$/.exec(action)
        if (numbered)
            return [mod + numbered[1]]
        if (action === "openSettings")
            return preview.settingsShortcuts
        if (action === "toggleSidebar")
            return [mod + "B"]
        if (action === "openCommands")
            return [mac ? "Meta+Shift+P" : "Ctrl+Shift+P"]
        if (action === "newAgent")
            return [mod + "T"]
        if (action === "reloadConfig")
            return [mod + "R"]
        if (action === "nextWindow")
            return [mac ? "Meta+Shift+]" : "Ctrl+Shift+]"]
        if (action === "previousWindow")
            return [mac ? "Meta+Shift+[" : "Ctrl+Shift+["]
        if (action === "newCategory")
            return [mod + "N"]
        if (action === "textBigger")
            return [mod + "=", mod + "+"]
        if (action === "textSmaller")
            return [mod + "-"]
        if (action === "textReset")
            return [mod + "0"]
        if (action === "reopenAgent")
            return [mac ? "Meta+Shift+T" : "Ctrl+Alt+Shift+T"]
        if (action === "resumeConversation")
            return [mod + "O"]
        if (action === "toggleTerminal")
            return mac ? ["Meta+`", "Ctrl+`"] : ["Ctrl+`"]
        if (action === "chooseTerminal")
            return mac ? ["Meta+Shift+`", "Meta+~"] : ["Ctrl+Shift+~", "Ctrl+~"]
        return []
    }

    function shortcutText(action) {
        return bindings(action).map(sequence => Qt.platform.os === "osx" ?
            sequence.replace(/Meta\+/g, "⌘").replace(/Ctrl\+/g, "⌃").replace(/Alt\+/g, "⌥").replace(/Shift\+/g, "⇧")
                    .replace(/Left/g, "←").replace(/Right/g, "→").replace(/Up/g, "↑").replace(/Down/g, "↓")
                    .replace(/Return/g, "↩") : sequence).join(" / ")
    }
    function projectName(path) {
        return path.replace(/\/+$/, "").split("/").pop() || "/"
    }
    function projectTitle(path) {
        const name = projectName(path)
        if (name.length <= 80)
            return name
        // Names use UTF-16 limits; keep both halves of an astral character.
        let end = 79
        if (name.charCodeAt(end - 1) >= 0xD800 && name.charCodeAt(end - 1) <= 0xDBFF
                && name.charCodeAt(end) >= 0xDC00 && name.charCodeAt(end) <= 0xDFFF)
            --end
        return name.slice(0, end) + "…"
    }
    function agentBaseTitle(session) {
        return workspace.previewMode || session.title !== projectTitle(session.directory) ?
                    session.title : workspace.displayPath(session.directory)
    }
    // Agents started in the same folder share a default title; number the
    // later ones so each card and menu entry names a distinct agent.
    function agentTabTitle(session) {
        const base = agentBaseTitle(session)
        if (workspace.previewMode) return base
        const list = workspace.categorySessions
        let ordinal = 0
        let total = 0
        for (let i = 0; i < list.length; ++i) {
            if (agentBaseTitle(list[i]) !== base) continue
            ++total
            if (list[i] === session) ordinal = total
        }
        return ordinal > 1 ? base + " · " + ordinal : base
    }
    function toggleSidebar() {
        if (!interactionArmed) return
        if (typeof keymap !== "undefined" && keymap !== null)
            keymap.setSidebarVisible(!sidebarExpanded)
        else
            transientSidebarVisible = !transientSidebarVisible
    }
    function togglePreviews() {
        if (!interactionArmed) return
        if (typeof keymap !== "undefined" && keymap !== null)
            keymap.setPreviewsVisible(!previewsEnabled)
        else
            transientPreviewsVisible = !transientPreviewsVisible
    }
    function openCommandsDialog() {
        if (!interactionArmed) return
        commandsDialog.open()
    }
    readonly property bool conversationsAvailable: typeof conversations !== "undefined" && conversations !== null
    function openResumeDialog() {
        if (!interactionArmed || dialogsVisible() || !conversationsAvailable) return
        resumeDialog.open()
    }
    // The home list shown when nothing is open: actions, then recent
    // conversations, then the other categories that have agents.
    ListModel { id: homeModel }
    Component.onCompleted: {
        homeRebuild.restart()
        recallWindowMemory()
    }
    property var homeRuns: []
    function rebuildHome() {
        if (workspace.focusedSession !== null && homeModel.count > 0)
            return
        const runs = []
        homeModel.clear()
        function add(group, kind, label, detail, hint, harness, run) {
            homeModel.append({group: group, kind: kind, label: label, detail: detail, hint: hint, harness: harness})
            runs.push(run)
        }
        add("", "action", qsTr("New agent"), "", shortcutText("newAgent"), "", () => window.openNewAgentDialog())
        if (conversationsAvailable)
            add("", "action", qsTr("Resume a conversation"), "", shortcutText("resumeConversation"), "", () => window.openResumeDialog())
        if (terminalsAvailable)
            add("", "action", qsTr("Terminal"), "", shortcutText("toggleTerminal").split(" / ")[0], "", () => window.toggleTerminal())
        if (workspace.canReopenAgent)
            add("", "action", qsTr("Reopen closed agent"), "", shortcutText("reopenAgent"), "", () => window.reopenAgent())
        if (conversationsAvailable) {
            for (const conversation of conversations.recent("", 5))
                add(qsTr("Recent conversations"), "conversation", conversation.title,
                    conversation.place + "  " + conversation.when, "", conversation.harness,
                    () => window.resumeConversation(conversation))
        }
        for (const category of workspace.categories) {
            if (category.id !== workspace.activeCategoryId && category.agentCount > 0)
                add(qsTr("Categories"), "category", category.name,
                    category.agentCount === 1 ? qsTr("1 agent") : qsTr("%1 agents").arg(category.agentCount),
                    "", "", () => workspace.selectCategory(category.id))
        }
        homeRuns = runs
        homeList.currentIndex = Math.min(Math.max(0, homeList.currentIndex), homeModel.count - 1)
    }
    function runHomeEntry(index) {
        if (!interactionArmed || index < 0 || index >= homeRuns.length) return
        homeRuns[index]()
    }
    Timer {
        id: homeRebuild
        interval: 0
        onTriggered: window.rebuildHome()
    }
    Connections {
        target: workspace
        function onCategoriesChanged() { homeRebuild.restart() }
        function onFocusChanged() { homeRebuild.restart() }
        function onClosedChanged() { homeRebuild.restart() }
        function onSessionsChanged() { homeRebuild.restart() }
    }
    Connections {
        target: window.conversationsAvailable ? conversations : null
        function onChanged() { homeRebuild.restart() }
    }
    // What gets the keyboard when no dialog does: the side terminal while it
    // shows, else the selected agent's terminal, or the home list when
    // nothing is open.
    readonly property string focusTarget: sideTerminalOpen && terminalsAvailable && terminals.current !== null ?
                                              "sideTerminalSurface" :
                                          workspace.focusedSession === null ? "homeList" : "liveTerminal"
    // Command-` (or Control-`): a plain shell on this Mac or an ssh host,
    // beside the agents, for a quick command; never an agent. Command-~
    // picks the machine.
    readonly property bool terminalsAvailable: typeof terminals !== "undefined" && terminals !== null
    property bool sideTerminalOpen: false
    property string lastTerminalMachine: ""
    function toggleTerminal() {
        if (!terminalsAvailable || terminalBusy)
            return
        if (sideTerminalOpen) {
            closeSideTerminal()
            return
        }
        if (!inputBlocked)
            openTerminalOn(lastTerminalMachine)
    }
    function openTerminalOn(machine) {
        if (!terminalsAvailable)
            return
        const started = terminals.show(machine)
        if (started)
            lastTerminalMachine = machine
        sideTerminalOpen = true
        preview.deferTerminalFocus()
    }
    function closeSideTerminal() {
        sideTerminalOpen = false
        preview.deferTerminalFocus()
    }
    function chooseTerminal() {
        if (!terminalsAvailable || terminalBusy || dialogsVisible())
            return
        terminalPicker.open()
    }
    // Typing exit closes the panel along with the shell.
    Connections {
        target: window.terminalsAvailable ? terminals : null
        function onCurrentChanged() {
            if (window.sideTerminalOpen && terminals.current === null && terminals.error.length === 0)
                window.closeSideTerminal()
        }
    }
    // A past conversation comes back as a new agent in this category, in its
    // folder, with the approval mode last chosen for a new agent.
    function resumeConversation(conversation) {
        const defaults = workspace.agentDefaults()
        const mode = window.lastMode.length > 0 ? window.lastMode :
                     defaults.mode && defaults.mode.length > 0 ? defaults.mode : "full"
        workspace.resumeAgent(conversation.directory, projectTitle(conversation.directory),
                              conversation.harness, conversation.id, mode)
    }
    function openToolsDialog() {
        if (!interactionArmed || dialogsVisible() || !toolsAvailable) return
        toolsDialog.open()
    }
    // A named window property prevents the pane's own `tools` model from
    // resolving as the context object and forming an engine binding loop.
    readonly property var toolStatus: typeof tools !== "undefined" ? tools : null
    readonly property bool toolsAvailable: toolStatus !== null
    // Built when a dialog that lists them is open: as a property it would be
    // rebuilt, and the closed lists' delegates with it, on every switch.
    function commandEntries() {
        const agent = workspace.focusedSession
        const hasAgent = agent !== null
        const needAgent = qsTr("Select an agent first")
        const entries = []
        function add(id, label, action, enabled, reason, run) {
            entries.push({id: id, label: label, shortcut: action ? window.shortcutText(action) : "",
                          enabled: enabled, reason: reason || "", run: run})
        }
        add("newAgent", qsTr("New agent"), "newAgent", true, "", () => window.openNewAgentDialog())
        add("newCategory", qsTr("New category"), "newCategory", true, "", () => window.openCategoryDialog("add"))
        add("openTools", qsTr("Tools"), "", toolsAvailable, qsTr("The tools pane is not available here"), () => window.openToolsDialog())
        add("toggleSidebar", sidebarExpanded ? qsTr("Hide sidebar") : qsTr("Show sidebar"), "toggleSidebar", true, "", () => window.toggleSidebar())
        add("togglePreviews", previewsEnabled ? qsTr("Hide agent previews") : qsTr("Show agent previews"), "togglePreviews", true, "", () => window.togglePreviews())
        add("nextWindow", qsTr("Next agent in category"), "nextWindow", workspace.categorySessions.length > 1, qsTr("This category needs another agent"), () => workspace.nextSession())
        add("previousWindow", qsTr("Previous agent in category"), "previousWindow", workspace.categorySessions.length > 1, qsTr("This category needs another agent"), () => workspace.nextSession(-1))
        add("closeAgent", qsTr("Close agent"), "closeAgent", hasAgent, needAgent, () => window.closeFocusedAgent())
        add("reopenAgent", qsTr("Reopen closed agent"), "reopenAgent", workspace.canReopenAgent, qsTr("No agent was closed since lapis opened"), () => window.reopenAgent())
        add("resumeConversation", qsTr("Resume a conversation"), "resumeConversation", conversationsAvailable, qsTr("Not available here"), () => window.openResumeDialog())
        add("toggleTerminal", sideTerminalOpen ? qsTr("Hide terminal") : qsTr("Terminal"), "toggleTerminal", terminalsAvailable, qsTr("Not available here"), () => window.toggleTerminal())
        add("chooseTerminal", qsTr("Terminal on another machine"), "chooseTerminal", terminalsAvailable, qsTr("Not available here"), () => window.chooseTerminal())
        const conversationFile = hasAgent && !window.visualActive ? workspace.conversationFile(agent.sessionId) : ""
        add("toggleVisual", window.visualActive ? qsTr("Show terminal") : qsTr("Show visual view"), "",
            window.visualActive || conversationFile.length > 0,
            hasAgent ? qsTr("Only for a Claude Code agent on this Mac, once its conversation is saved") : needAgent,
            () => window.toggleVisual())
        add("toggleVisualAll", window.visualAll ? qsTr("Show terminals for all agents") : qsTr("Show visual view for all agents"), "",
            typeof keymap !== "undefined" && keymap !== null, qsTr("Not available here"),
            () => window.toggleVisualAll())
        add("textBigger", qsTr("Bigger text"), "textBigger", true, "", () => window.changeTextSize(1))
        add("textSmaller", qsTr("Smaller text"), "textSmaller", true, "", () => window.changeTextSize(-1))
        add("textReset", qsTr("Default text size"), "textReset", true, "", () => window.changeTextSize(0))
        const stopped = hasAgent && agent.live && (agent.connectionState === "ended" || agent.connectionState === "disconnected")
        add("restartAgent", qsTr("Restart agent"), "", stopped, qsTr("Only an ended or unreachable agent restarts"), () => workspace.restartAgent(agent.sessionId))
        // A CLI rereads its settings only when it starts: these end it and
        // start it again, resuming its conversation.
        add("reloadTab", qsTr("Reload tab"), "", hasAgent && agent.live, needAgent, () => workspace.reloadAgent(agent.sessionId))
        add("reloadCategory", qsTr("Reload category"), "", workspace.categorySessions.length > 0, qsTr("This category has no agents"), () => workspace.reloadCategory())
        add("reloadWindow", qsTr("Reload window"), "", workspace.sessions.length > 0, qsTr("No agents are open"), () => workspace.reloadAll())
        add("nextCategory", qsTr("Next category"), "nextCategory", workspace.categories.length > 1, qsTr("Add another category first"), () => workspace.nextCategory())
        add("previousCategory", qsTr("Previous category"), "previousCategory", workspace.categories.length > 1, qsTr("Add another category first"), () => workspace.nextCategory(-1))
        for (let i = 0; i < Math.min(9, workspace.categories.length); ++i) {
            const category = workspace.categories[i]
            add("category" + (i + 1), qsTr("Switch category: %1").arg(category.name), "category" + (i + 1), true, "", () => workspace.selectCategory(category.id))
        }
        add("renameCategory", qsTr("Rename category"), "", true, "", () => window.openCategoryDialog("rename"))
        add("removeCategory", qsTr("Remove category"), "", window.canRemoveActiveCategory(), qsTr("Move its agents first; keep at least one category"), () => window.removeActiveCategory())
        add("renameAgent", qsTr("Rename agent"), "", hasAgent, needAgent, () => window.openRenameDialog())
        add("earlier", qsTr("Move agent earlier"), "", hasAgent && window.focusedTabIndex() > 0, qsTr("No earlier tab position"), () => window.moveFocused(-1))
        add("later", qsTr("Move agent later"), "", hasAgent && window.focusedTabIndex() < workspace.categorySessions.length - 1, qsTr("No later tab position"), () => window.moveFocused(1))
        for (const category of workspace.categories) {
            if (category.id !== workspace.activeCategoryId)
                add("move:" + category.id, qsTr("Move agent to %1").arg(category.name), "", hasAgent, needAgent, () => workspace.moveSession(agent.sessionId, category.id))
        }
        add("requests", qsTr("Review requests"), "", hasAgent && agent.attentionCount > 0, qsTr("No pending requests"), () => window.openAttentionDialog())
        add("older", qsTr("Browse older history"), "", hasAgent && agent.live && agent.connectionState === "ready" && !agent.historyRequestPending, qsTr("Connect the agent and wait for pending history"), () => agent.olderHistory())
        add("newer", qsTr("Browse newer history"), "", hasAgent && agent.historyActive && !agent.historyRequestPending, qsTr("Open archived history first"), () => agent.newerHistory())
        add("live", qsTr("Return to live output"), "", hasAgent && agent.historyActive, qsTr("Already showing live output"), () => agent.returnToLive())
        // Takes the agent back from another view, such as the phone holding a
        // session that cannot be joined; nothing else reclaims a replaced view.
        add("reconnect", qsTr("Reconnect agent"), "", window.recoveryAvailable(), qsTr("The selected agent is already connected or opening"), () => agent.reconnect())
        add("appearance", qsTr("Appearance"), "openSettings", true, "", () => window.openSettingsDialog())
        add("reloadConfig", qsTr("Reload configuration"), "reloadConfig", typeof keymap !== "undefined" && keymap !== null, qsTr("No configuration in this fixture"), () => keymap.reload())
        const mac = Qt.platform.os === "osx"
        add("closeWindow", qsTr("Close window (lapis keeps running)"), "closeWindow", mac, qsTr("Only on the Mac"), () => window.close())
        add("minimizeWindow", qsTr("Minimize window"), "minimizeWindow", true, "", () => window.showMinimized())
        add("quit", qsTr("Quit lapis"), "quit", true, "", () => Qt.quit())
        return entries
    }
    function runCommand(id) {
        if (!interactionArmed) return
        const command = commandEntries().find(entry => entry.id === id)
        if (command && command.enabled) command.run()
    }

    // Every status class has its own shape so no state depends on color alone:
    // dot working, ring ready (an agent waiting on an answer reads as ready: it
    // pinged as a finished turn does), square lost connection, dash ended,
    // hollow box opening or unknown.
    function statusShape(kind) {
        if (kind === "working")
            return "dot"
        if (kind === "idle" || kind === "finished" || kind === "waiting")
            return "ring"
        if (kind === "disconnected")
            return "square"
        if (kind === "ended")
            return "dash"
        return "box"
    }
    function statusColor(kind) {
        if (kind === "working")
            return activityColor
        if (kind === "disconnected")
            return faultColor
        if (kind === "idle" || kind === "finished" || kind === "waiting")
            return textColor
        return mutedTextColor
    }
    // Mono recall number for categories reachable by a category1-9 shortcut.
    function categoryRecall(index) {
        return index < 9 && bindings("category" + (index + 1)).length > 0 ? String(index + 1) : ""
    }
    function categoryAttentionElsewhere() {
        let total = 0
        for (const category of workspace.categories) {
            if (category.id !== workspace.activeCategoryId)
                total += category.unseenCount
        }
        return total
    }

    function categoryIndex(id) {
        const list = workspace.categories
        for (let i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return i
        }
        return -1
    }

    function selectCategoryByIndex(index) {
        const list = workspace.categories
        if (index >= 0 && index < list.length && list[index].id)
            workspace.selectCategory(list[index].id)
    }

    function focusedTabIndex() {
        const session = workspace.focusedSession
        const list = workspace.categorySessions
        if (!session)
            return -1
        for (let i = 0; i < list.length; ++i) {
            if (list[i].sessionId === session.sessionId)
                return i
        }
        return -1
    }

    function nameProblem(value) {
        const name = value.trim()
        if (name.length < 1 || name.length > 80)
            return qsTr("Use a name of 1–80 characters.")
        for (let i = 0; i < name.length; ++i) {
            const code = name.charCodeAt(i)
            if (code < 32 || code === 127)
                return qsTr("That name cannot be used.")
        }
        return ""
    }

    function directoryProblem(value) {
        const path = value.trim()
        if (path.length < 1)
            return qsTr("Enter the project folder.")
        if (path.indexOf("\u0000") >= 0)
            return qsTr("That folder path cannot be used.")
        return ""
    }

    function dialogsVisible() {
        return commandsDialog.visible || resumeDialog.visible || terminalPicker.visible || toolsDialog.visible || settingsDialog.visible || attentionDialog.visible || agentDialog.visible
                || closeAgentDialog.visible || categoryDialog.visible || renameAgentDialog.visible
    }

    function menusVisible() {
        return agentMenu.visible || categoryMenu.visible
    }

    function closeMenus() {
        if (moveMenu.visible)
            moveMenu.close()
        if (agentMenu.visible)
            agentMenu.close()
        if (categoryMenu.visible)
            categoryMenu.close()
    }

    function openFresh(dialog) {
        if (terminalBusy)
            return
        if (dialogsVisible())
            return
        if (menusVisible()) {
            transitionLock = true
            pendingDialog = dialog
            closeMenus()
            return
        }
        dialog.open()
    }

    function onMenuClosed() {
        if (menusVisible())
            return
        const next = pendingDialog
        pendingDialog = null
        if (next) {
            next.open()
            transitionLock = false
            return
        }
        transitionLock = false
        if (!dialogsVisible())
            preview.deferTerminalFocus()
    }

    function popupAt(menu, anchor) {
        if (terminalBusy)
            return
        const point = anchor.mapToItem(window.contentItem, 0, anchor.height + 4)
        const menuWidth = Math.min(300, Math.max(180, window.width - 16))
        menu.width = menuWidth
        menu.x = Math.max(8, Math.min(point.x, window.width - menuWidth - 8))
        const estimated = 340
        let y = point.y
        if (y + estimated > window.height - 8)
            y = Math.max(8, anchor.mapToItem(window.contentItem, 0, -4).y - estimated)
        menu.y = y
        menu.open()
    }

    function openSettingsDialog() {
        if (terminalBusy)
            return
        if (attentionDialog.visible || agentDialog.visible || categoryDialog.visible
                || renameAgentDialog.visible || settingsDialog.visible || closeAgentDialog.visible)
            return
        if (menusVisible() || (categorySelector.popup !== null && categorySelector.popup.visible)) {
            if (categorySelector.popup !== null && categorySelector.popup.visible)
                categorySelector.popup.close()
            openFresh(settingsDialog)
            return
        }
        settingsDialog.open()
        settingsDialog.forceActiveFocus()
    }

    function openAttentionDialog() {
        if (terminalBusy)
            return
        if (inputBlocked)
            return
        if (workspace.focusedSession && workspace.focusedSession.hasAttentionSource)
            attentionDialog.showSession(workspace.focusedSession)
    }

    // What the last new agent was started with, kept for the next one: the
    // CLI, the approval mode across CLIs, and each CLI's model.
    property string lastHarness: ""
    // The machine the last new agent started on: "" for this Mac, else an ssh host.
    property string lastMachine: ""
    property string lastMode: ""
    property var lastModels: ({})
    // The window's own choices outlive a restart of the window (guiState;
    // absent in fixtures): the new-agent form's and the side terminal's.
    readonly property var windowMemory: typeof guiState !== "undefined" ? guiState : null
    // QML initializes its properties after the state is loaded; remember only
    // after recall has read it, or those writes would erase loaded choices.
    property bool windowMemoryReady: false
    function remember(key, value) {
        if (windowMemory !== null && windowMemoryReady)
            windowMemory.setValue(key, value)
    }
    onLastHarnessChanged: remember("lastHarness", lastHarness)
    onLastMachineChanged: remember("lastMachine", lastMachine)
    onLastModeChanged: remember("lastMode", lastMode)
    onLastModelsChanged: remember("lastModels", lastModels)
    onLastTerminalMachineChanged: remember("lastTerminalMachine", lastTerminalMachine)
    onSideTerminalOpenChanged: remember("sideTerminalOpen", sideTerminalOpen)
    function recallWindowMemory() {
        if (windowMemory === null)
            return
        const catalog = workspace.availableHarnesses()
        const text = key => {
            const value = windowMemory.value(key)
            return typeof value === "string" ? value : ""
        }
        const harnesses = catalog
        const harness = harnesses.find(candidate => candidate.id === text("lastHarness"))
        lastHarness = harness ? harness.id : ""
        const machines = [""].concat(workspace.sshMachines())
        lastMachine = machines.includes(text("lastMachine")) ? text("lastMachine") : ""
        const mode = text("lastMode")
        lastMode =
            harness && harness.modes.some(candidate => candidate.id === mode) ? mode : ""
        const models = windowMemory.value("lastModels")
        if (models !== null && typeof models === "object" && !Array.isArray(models)) {
            const validModels = {}
            for (const candidate of harnesses)
                if (typeof models[candidate.id] === "string")
                    validModels[candidate.id] = models[candidate.id]
            lastModels = validModels
        }
        lastTerminalMachine = machines.includes(text("lastTerminalMachine"))
                                 ? text("lastTerminalMachine") : ""
        if (windowMemory.value("sideTerminalOpen") === true)
            Qt.callLater(() => openTerminalOn(lastTerminalMachine))
        windowMemoryReady = true
    }
    function openNewAgentDialog() {
        if (terminalBusy)
            return
        if (dialogsVisible())
            return
        agentDialog.localError = ""
        agentDialog.harnesses = workspace.availableHarnesses()
        agentDialog.machines = [""].concat(workspace.sshMachines())
        agentDialog.selectedMachine = agentDialog.machines.indexOf(window.lastMachine) >= 0 ? window.lastMachine : ""
        agentDialog.folderMachine = ""
        agentDialog.phase = 0
        // The config's newAgent defaults: the CLI, then the folder to start in.
        const defaults = workspace.agentDefaults()
        const wanted = window.lastHarness.length > 0 ? window.lastHarness : defaults.harness
        const preferred = agentDialog.harnesses.findIndex(h => h.id === wanted && h.installed)
        harnessChoices.currentIndex = preferred >= 0 ? preferred : Math.max(0, agentDialog.harnesses.findIndex(h => h.installed))
        const folder = defaults.folder === "~" ? workspace.homeDirectory :
                       defaults.folder.startsWith("~/") ? workspace.homeDirectory + defaults.folder.slice(1) :
                       defaults.folder
        agentDirectoryField.text = (folder.length > 0 ? folder.replace(/\/+$/, "") : workspace.homeDirectory) + "/"
        agentDialog.preferredMode = window.lastMode.length > 0 ? window.lastMode :
                                    defaults.mode && defaults.mode.length > 0 ? defaults.mode : "full"
        openFresh(agentDialog)
    }
    // Command-W, as in a browser: the side terminal's panel if it shows (its
    // shell keeps running), else the focused agent, and only when the category
    // has no agent left the window (on the Mac, where lapis keeps running).
    function closeInFront() {
        if (terminalBusy || inputBlocked)
            return
        if (sideTerminalOpen) {
            closeSideTerminal()
            return
        }
        if (workspace.focusedSession !== null) {
            closeFocusedAgent()
            return
        }
        if (Qt.platform.os === "osx")
            window.close()
    }
    // Close the focused agent. A running agent is confirmed first, the way
    // iTerm2 asks before closing a session with a running job.
    function closeFocusedAgent() {
        if (terminalBusy)
            return
        const session = workspace.focusedSession
        if (!session || session.closing)
            return
        if (!session.live || session.connectionState === "ended") {
            workspace.closeSession(session.sessionId, false)
            return
        }
        closeAgentDialog.sessionId = session.sessionId
        closeAgentDialog.harnessId = session.harnessId
        closeAgentDialog.agentName = session.agentName
        closeAgentDialog.place = workspace.displayPath(session.directory)
        closeAgentDialog.reachable = session.reachable
        openFresh(closeAgentDialog)
    }

    function openCategoryDialog(mode) {
        if (terminalBusy)
            return
        if (dialogsVisible())
            return
        categoryDialog.mode = mode
        categoryDialog.initialName = mode === "rename" ? activeCategoryName : ""
        categoryDialog.localError = ""
        openFresh(categoryDialog)
    }

    function openRenameDialog() {
        if (terminalBusy)
            return
        const session = workspace.focusedSession
        if (!session || dialogsVisible())
            return
        renameAgentDialog.sessionId = session.sessionId
        renameAgentDialog.initialTitle = session.title
        renameAgentDialog.localError = ""
        openFresh(renameAgentDialog)
    }

    function commitNewAgent() {
        const folder = agentDirectoryField.text.trim()
        const title = projectTitle(folder)
        const folderIssue = directoryProblem(agentDirectoryField.text)
        if (folderIssue.length > 0) {
            agentDialog.localError = folderIssue
            return
        }
        const model = agentDialog.chosenModel && !agentDialog.chosenModel.default ? agentDialog.chosenModel.id : ""
        if (!workspace.createAgent(agentDirectoryField.text.trim(), title, agentDialog.selectedHarness,
                                   model, agentDialog.selectedMode, agentDialog.selectedMachine, false)) {
            agentDialog.localError = workspace.workspaceError.length > 0 ? workspace.workspaceError :
                                                                          qsTr("Could not start %1.").arg(agentDialog.harnessName)
            return
        }
        agentDialog.localError = ""
        agentDirectoryField.text = ""
        agentDialog.close()
    }

    function commitCategory() {
        const issue = nameProblem(categoryNameField.text)
        if (issue.length > 0) {
            categoryDialog.localError = issue
            return
        }
        const name = categoryNameField.text.trim()
        const ok = categoryDialog.mode === "rename" ?
                    workspace.renameCategory(workspace.activeCategoryId, name) :
                    workspace.addCategory(name)
        if (!ok) {
            categoryDialog.localError = workspace.workspaceError.length > 0 ? workspace.workspaceError :
                                                                             qsTr("Could not save the category.")
            return
        }
        // Agents dropped on the rail's + come along into the new category.
        if (categoryDialog.mode !== "rename" && pendingCategoryAgents.length > 0)
            workspace.placeSessions(pendingCategoryAgents, workspace.activeCategoryId, 1 << 20)
        pendingCategoryAgents = []
        categoryDialog.localError = ""
        categoryDialog.close()
    }

    function commitRenameAgent() {
        const issue = nameProblem(renameAgentField.text)
        if (issue.length > 0) {
            renameAgentDialog.localError = issue
            return
        }
        if (!workspace.renameSession(renameAgentDialog.sessionId, renameAgentField.text.trim())) {
            renameAgentDialog.localError = workspace.workspaceError.length > 0 ? workspace.workspaceError :
                                                                              qsTr("Could not rename the agent.")
            return
        }
        renameAgentDialog.localError = ""
        renameAgentDialog.close()
    }

    function canRemoveActiveCategory() {
        return workspace.categories.length > 1 && workspace.categorySessions.length === 0
    }

    function removeActiveCategory() {
        if (!canRemoveActiveCategory())
            return
        workspace.removeCategory(workspace.activeCategoryId)
    }

    function moveFocused(delta) {
        const session = workspace.focusedSession
        if (!session)
            return
        workspace.moveSessionBy(session.sessionId, delta)
    }

    // Several agents picked with Command-click or Shift-click, to drag together.
    property var selectedAgents: []
    // What a drag carries: agent ids from the strip, or a category.
    property var draggedAgents: []
    property string dragSource: ""
    property string draggedCategory: ""
    // Agents waiting for the category being created from a drop on its +.
    property var pendingCategoryAgents: []
    readonly property int toggleModifier: Qt.platform.os === "osx" ? Qt.MetaModifier : Qt.ControlModifier
    // Agents shown as a readable thread (the visual view) instead of their
    // terminal, by id. The terminal keeps running under it, takes no input
    // while it is covered, and gets the keyboard back on Escape.
    // "All agents" (saved in lapis.json) shows every Claude Code agent on
    // this Mac as a thread; visualAgents holds this run's per-agent choices,
    // which win over it until lapis restarts or "all" is switched again.
    property var visualAgents: ({})
    readonly property var visualSettings: (typeof keymap !== "undefined" && keymap !== null) ? keymap.visual : ({})
    readonly property bool visualAll: visualSettings.all === true
    readonly property bool visualWanted: {
        const agent = workspace.focusedSession
        if (agent === null)
            return false
        const choice = visualAgents[agent.sessionId]
        return choice === undefined ? visualAll : choice
    }
    // The file appears once the CLI saves its first message; until then the
    // check repeats while the thread is wanted.
    property int visualProbe: 0
    readonly property string visualFile: visualProbe >= 0 && visualWanted
                                         ? workspace.conversationFile(workspace.focusedSession.sessionId) : ""
    readonly property bool visualActive: visualWanted && visualFile.length > 0
    Timer {
        interval: 2000
        repeat: true
        running: window.visualWanted && window.visualFile.length === 0
        onTriggered: window.visualProbe += 1
    }
    // Some slash commands open a dialog in the CLI's terminal (/model's
    // picker, /config, /resume). The thread cannot show one, so after the
    // bar sends a command the terminal is watched for a few seconds and, if a
    // dialog replaces its input box, shown with the keyboard until the dialog
    // closes. Only the person's own command starts this: a request an agent
    // raises never uncovers the terminal or takes the keys.
    property bool visualDialog: false
    property int visualDialogTicks: -1 // -1: not watching
    readonly property bool visualCovers: visualActive && !visualDialog
    property string visualDialogAgent: ""
    function watchVisualDialog(found) {
        if (!visualActive || liveTerminal.document === null)
            return
        visualDialogAgent = liveTerminal.document.sessionId
        visualDialogTicks = 0
        if (found)
            visualDialog = true
    }
    function endVisualDialog() {
        visualDialogTicks = -1
        visualDialog = false
    }
    Timer {
        interval: 200
        repeat: true
        running: window.visualDialogTicks >= 0
        onTriggered: {
            const agent = liveTerminal.document
            if (!window.visualActive || agent === null || agent.sessionId !== window.visualDialogAgent) {
                window.endVisualDialog() // another agent is selected now
            } else if (workspace.transcript.dialogShowing(liveTerminal.screenLines())) {
                // A turn's own request is the agent's, not this command's.
                if (window.visualDialog || agent.statusKind !== "working")
                    window.visualDialog = true
                else
                    window.endVisualDialog()
            } else if (window.visualDialog || ++window.visualDialogTicks > 25) {
                window.endVisualDialog() // closed, or none came in five seconds
            }
        }
    }
    // Selecting another agent can show or hide the thread on its own (All
    // agents); the keyboard follows whichever is shown.
    onVisualCoversChanged: if (!inputBlocked && !dialogsVisible()) visualFocus()
    function visualFocus() {
        Qt.callLater(() => visualCovers ? visualView.composer.forceActiveFocus() : liveTerminal.forceActiveFocus())
    }
    function toggleVisual() {
        const agent = workspace.focusedSession
        if (!agent)
            return
        const next = Object.assign({}, visualAgents)
        next[agent.sessionId] = !visualActive
        visualAgents = next
        endVisualDialog()
        visualFocus()
    }
    function toggleVisualAll() {
        if (typeof keymap === "undefined" || keymap === null)
            return
        visualAgents = ({})
        keymap.setVisual("all", !visualAll)
        visualFocus()
    }
    // Each CLI's commands for the visual view's model and effort pickers. A
    // CLI without an entry shows what it reports but offers no choices.
    readonly property var visualCommands: ({
        claude: {
            model: id => "/model " + id,
            effort: level => "/effort " + level,
            efforts: ["low", "medium", "high", "xhigh", "max"]
        }
    })
    function visualModels(agent) {
        if (!agent || !visualCommands[agent.harnessId])
            return []
        const harness = workspace.availableHarnesses().find(h => h.id === agent.harnessId)
        return harness && harness.models ? harness.models.map(m => ({id: m.id, name: m.name})) : []
    }
    function visualEfforts(agent) {
        const commands = agent ? visualCommands[agent.harnessId] : null
        return commands ? commands.efforts : []
    }
    function visualCommand(kind, value) {
        const agent = liveTerminal.document
        const commands = agent ? visualCommands[agent.harnessId] : null
        if (commands && commands[kind] && liveTerminal.submitText(commands[kind](value)))
            watchVisualDialog(false)
    }
    // A new agent from this conversation: up to a reply, or just before a
    // prompt with that prompt waiting in its input.
    function forkVisual(record, before, prefill) {
        const agent = workspace.focusedSession
        if (!agent)
            return
        const fork = workspace.forkConversation(agent.sessionId, record, before)
        if (fork.length === 0)
            return
        const defaults = workspace.agentDefaults()
        const mode = window.lastMode.length > 0 ? window.lastMode :
                     defaults.mode && defaults.mode.length > 0 ? defaults.mode : "full"
        const title = (agent.title.length > 0 ? agent.title : "claude").slice(0, 73) + " (fork)"
        workspace.resumeAgent(workspace.agentFolder(agent.sessionId), title, "claude", fork, mode, prefill)
    }

    function stripIds() {
        return workspace.categorySessions.map(session => session.sessionId)
    }
    function isAgentSelected(id) {
        return selectedAgents.indexOf(id) >= 0
    }
    function clearAgentSelection() {
        if (selectedAgents.length > 0)
            selectedAgents = []
    }
    // A plain click shows the agent; Command-click adds or removes it from a
    // selection, and Shift-click selects the run from the shown agent to it.
    function clickAgent(id, modifiers) {
        const focused = workspace.focusedSession ? workspace.focusedSession.sessionId : ""
        if (modifiers & toggleModifier) {
            let chosen = selectedAgents.length > 0 || focused === "" ? selectedAgents.slice() : [focused]
            const at = chosen.indexOf(id)
            if (at >= 0)
                chosen.splice(at, 1)
            else
                chosen.push(id)
            selectedAgents = chosen
            return
        }
        if (modifiers & Qt.ShiftModifier) {
            const ids = stripIds()
            const from = Math.max(0, ids.indexOf(focused))
            const to = ids.indexOf(id)
            if (to >= 0) {
                selectedAgents = ids.slice(Math.min(from, to), Math.max(from, to) + 1)
                return
            }
        }
        clearAgentSelection()
        if (workspace.selectSession(id))
            preview.deferTerminalFocus()
    }
    // Dragging a selected card carries the whole selection, in strip order.
    function agentsToDrag(id) {
        if (!isAgentSelected(id) || selectedAgents.length < 2)
            return [id]
        return stripIds().filter(candidate => isAgentSelected(candidate))
    }
    function beginDrag(kind, label, scenePosition) {
        dragGhost.label = label
        dragGhost.Drag.keys = [kind]
        moveDragGhost(scenePosition)
        dragGhost.Drag.active = true
    }
    function beginAgentDrag(ids, source, scenePosition) {
        if (ids.length === 0)
            return
        draggedAgents = ids
        dragSource = source
        const first = workspace.categorySessions.find(session => session.sessionId === ids[0])
        const label = ids.length > 1 ? qsTr("%1 agents").arg(ids.length) : first ? agentTabTitle(first) : ""
        beginDrag("lapis-agents", label, scenePosition)
    }
    function beginCategoryDrag(id, name, scenePosition) {
        draggedCategory = id
        beginDrag("lapis-category", name, scenePosition)
    }
    function moveDragGhost(scenePosition) {
        dragGhost.x = scenePosition.x - dragGhost.Drag.hotSpot.x
        dragGhost.y = scenePosition.y - dragGhost.Drag.hotSpot.y
    }
    function endDrag() {
        if (dragGhost.Drag.active)
            dragGhost.Drag.drop()
        dragGhost.Drag.active = false
        // The drop's changes wait for the drag to finish (queueDrop).
        Qt.callLater(() => {
            draggedAgents = []
            dragSource = ""
            draggedCategory = ""
        })
    }
    // Apply a drop after the dragging card's handler returns: the change can
    // replace the very card being dragged.
    function queueDrop(apply) {
        Qt.callLater(apply)
    }
    function dropOnStrip(visualIndex) {
        const ids = draggedAgents
        if (ids.length === 0)
            return
        // Positions count the agents that stay where they are.
        const strip = stripIds()
        let index = 0
        for (let i = 0; i < Math.min(visualIndex, strip.length); ++i)
            if (ids.indexOf(strip[i]) < 0)
                ++index
        workspace.placeSessions(ids, workspace.activeCategoryId, index)
        clearAgentSelection()
    }
    function dropOnCategory(categoryId) {
        if (draggedAgents.length > 0 && categoryId !== workspace.activeCategoryId)
            workspace.placeSessions(draggedAgents, categoryId, 1 << 20)
        clearAgentSelection()
    }
    function dropOnNewCategory() {
        if (draggedAgents.length === 0)
            return
        pendingCategoryAgents = draggedAgents
        clearAgentSelection()
        openCategoryDialog("add")
    }
    function dropCategory(targetIndex) {
        const from = categoryIndex(draggedCategory)
        if (from < 0)
            return
        workspace.placeCategory(draggedCategory, from < targetIndex ? targetIndex - 1 : targetIndex)
    }
    readonly property bool desktopAvailable: typeof desktop !== "undefined" && desktop !== null
    // Command-plus, minus and zero: the terminal text size, saved to the config.
    function changeTextSize(delta) {
        if (typeof keymap === "undefined" || keymap === null)
            return
        const next = delta === 0 ? keymap.terminalFontSizeDefault : keymap.terminalFontSize + delta
        keymap.setTerminalFontSize(Math.max(keymap.terminalFontSizeMinimum,
                                            Math.min(keymap.terminalFontSizeMaximum, next)))
    }
    function reopenAgent() {
        if (interactionArmed && workspace.reopenAgent())
            preview.deferTerminalFocus()
    }
    // Dropped files become their paths, quoted for a shell, as in Terminal.
    function quoteDroppedPaths(urls) {
        const quoted = []
        for (const url of urls) {
            const path = liveTerminal.localFilePath(url.toString())
            if (path.length === 0)
                continue
            quoted.push("'" + path.replace(/'/g, "'\\''") + "'")
        }
        return quoted.length > 0 ? quoted.join(" ") + " " : ""
    }
    function pastePaths(text, destinationId) {
        if (!window.interactionArmed || text.length === 0)
            return false
        const destination = workspace.focusedSession
        if (!destination || liveTerminal.document !== destination)
            return false
        if (destination.sessionId !== destinationId)
            return false
        return liveTerminal.pasteText(text)
    }

    function recoveryAvailable() {
        const session = workspace.focusedSession
        return session && session.live && !session.inputReady
                && session.connectionState !== "connecting"
                && session.connectionState !== "synchronizing"
    }

    onActiveChanged: if (active)
                         keyboardOwnershipReady()
    onInputBlockedChanged: if (!inputBlocked) {
                               keyboardOwnershipReady()
                               preview.deferTerminalFocus()
                           }
    onNarrowChanged: if (!narrow && categorySelector.popup !== null && categorySelector.popup.visible)
                         categorySelector.popup.close()

    Shortcut {
        objectName: "commandsShortcut"
        sequences: window.bindings("openCommands")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.openCommandsDialog()
    }
    Shortcut {
        objectName: "previewsShortcut"
        sequences: window.bindings("togglePreviews")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.togglePreviews()
    }
    Shortcut {
        objectName: "sidebarShortcut"
        sequences: window.bindings("toggleSidebar")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.toggleSidebar()
    }
    Resume {
        id: resumeDialog
        engine: window.conversationsAvailable ? conversations : null
        surfaceColor: window.surfaceColor
        textColor: window.textColor
        mutedColor: window.mutedTextColor
        accentColor: window.focusedBorderColor
        selectionColor: window.focusedColor
        hoverColor: window.hoveredCardColor
        borderColor: window.borderColor
        monoFamily: window.monoFamily
        uiFont: window.chromeFont
        readoutFont: window.readoutFont
        chromeRadius: window.chromeRadius
        motionDuration: window.motionDuration
        motionEnabled: window.motionEnabled
        onChosen: function(conversation) { Qt.callLater(function() { window.resumeConversation(conversation) }) }
        onClosed: preview.deferTerminalFocus()
    }
    Tools {
        id: toolsDialog
        engine: window.toolStatus
        surfaceColor: window.surfaceColor
        cardColor: window.cardColor
        textColor: window.textColor
        mutedColor: window.mutedTextColor
        accentColor: window.focusedBorderColor
        faultColor: window.faultColor
        plentyColor: window.plentyColor
        attentionColor: window.attentionColor
        borderColor: window.borderColor
        selectionColor: window.focusedColor
        monoFamily: window.monoFamily
        uiFont: window.chromeFont
        readoutFont: window.readoutFont
        chromeRadius: window.chromeRadius
        motionDuration: window.motionDuration
        motionEnabled: window.motionEnabled
        onClosed: preview.deferTerminalFocus()
    }

    Commands {
        id: commandsDialog
        commands: visible ? window.commandEntries() : []
        surfaceColor: window.surfaceColor
        textColor: window.textColor
        mutedColor: window.mutedTextColor
        accentColor: window.focusedBorderColor
        selectionColor: window.focusedColor
        hoverColor: window.hoveredCardColor
        borderColor: window.borderColor
        monoFamily: window.monoFamily
        uiFont: window.chromeFont
        readoutFont: window.readoutFont
        chromeRadius: window.chromeRadius
        motionDuration: window.motionDuration
        motionEnabled: window.motionEnabled
        onRequested: function(commandId) { Qt.callLater(function() { window.runCommand(commandId) }) }
        onClosed: preview.deferTerminalFocus()
    }

    component PlainLabel: Label { textFormat: Text.PlainText }
    component PlainText: Text { textFormat: Text.PlainText }

    component IndexShortcut: Shortcut {
        required property int targetIndex
        required property string action
        sequences: window.bindings(action)
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.selectCategoryByIndex(targetIndex)
    }

    // Command-Left/Right stay with the terminal. These chords are the workspace
    // bindings from the keymap; they are off while a dialog, menu, or composition
    // owns the keyboard. On the Mac a closed window only hides: lapis keeps
    // running and the Dock icon brings it back.
    Shortcut {
        sequences: window.bindings("quit")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: Qt.quit()
    }
    ActionShortcut { action: "closeWindow"; onActivated: window.close() }
    ActionShortcut { action: "minimizeWindow"; onActivated: window.showMinimized() }
    Shortcut {
        // Unbound by default; kept for configurations that name it.
        sequences: window.bindings("detachWindow")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.close()
    }
    Shortcut {
        objectName: "closeAgentShortcut"
        sequences: window.bindings("closeAgent")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.closeInFront()
    }
    // A press that clicks, or once it moves a few pixels, drags. One mouse
    // area rather than a TapHandler and DragHandler pair, which inside the
    // strip's list lost clicks.
    component DragOrClick: MouseArea {
        id: area
        signal tapped(int modifiers)
        signal dragStarted(point scenePosition)
        signal dragMoved(point scenePosition)
        signal dragEnded()
        property point pressedAt
        property bool dragging: false
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        onPressed: function(mouse) {
            pressedAt = Qt.point(mouse.x, mouse.y)
            dragging = false
        }
        onPositionChanged: function(mouse) {
            const scene = area.mapToItem(null, mouse.x, mouse.y)
            if (!dragging && Math.hypot(mouse.x - pressedAt.x, mouse.y - pressedAt.y) >= 8) {
                dragging = true
                dragStarted(scene)
            }
            if (dragging)
                dragMoved(scene)
        }
        onReleased: function(mouse) {
            if (dragging) {
                dragging = false
                dragEnded()
            } else {
                tapped(mouse.modifiers)
            }
        }
        onCanceled: if (dragging) {
                        dragging = false
                        dragEnded()
                    }
    }
    component ActionShortcut: Shortcut {
        required property string action
        objectName: action + "Shortcut"
        sequences: window.bindings(action)
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
    }
    ActionShortcut { action: "textBigger"; onActivated: window.changeTextSize(1) }
    ActionShortcut { action: "textSmaller"; onActivated: window.changeTextSize(-1) }
    ActionShortcut { action: "textReset"; onActivated: window.changeTextSize(0) }
    ActionShortcut { action: "reopenAgent"; onActivated: window.reopenAgent() }
    ActionShortcut { action: "resumeConversation"; onActivated: window.openResumeDialog() }
    ActionShortcut { action: "toggleTerminal"; onActivated: window.toggleTerminal() }
    ActionShortcut { action: "chooseTerminal"; onActivated: window.chooseTerminal() }
    // What a drag carries, under the pointer. Drop targets read its keys:
    // agents for the strip and the rail; a category for the rail.
    Rectangle {
        id: dragGhost
        objectName: "dragGhost"
        property string label: ""
        // Above the window's content; not on Qt's popup overlay, which a child
        // would make visible and able to take clicks.
        z: 1000
        visible: Drag.active
        width: Math.min(260, ghostText.implicitWidth + 24)
        height: Math.max(28, window.readoutFont + 14)
        radius: window.chromeRadius
        color: window.cardColor
        border.width: 1
        border.color: window.focusedBorderColor
        opacity: 0.92
        Drag.hotSpot.x: 14
        Drag.hotSpot.y: height / 2
        PlainText {
            id: ghostText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, 236)
            text: dragGhost.label
            color: window.textColor
            elide: Text.ElideRight
            font.family: window.monoFamily
            font.pixelSize: window.readoutFont
        }
    }
    Connections {
        target: workspace
        function onCategoryChanged() {
            window.clearAgentSelection()
        }
    }
    Shortcut {
        objectName: "nextCategoryShortcut"
        sequences: window.bindings("nextCategory")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        onActivated: workspace.nextCategory(1)
    }
    Shortcut {
        objectName: "previousCategoryShortcut"
        sequences: window.bindings("previousCategory")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        onActivated: workspace.nextCategory(-1)
    }
    IndexShortcut {
        objectName: "categoryShortcut1"
        targetIndex: 0
        action: "category1"
    }
    IndexShortcut {
        objectName: "categoryShortcut2"
        targetIndex: 1
        action: "category2"
    }
    IndexShortcut {
        objectName: "categoryShortcut3"
        targetIndex: 2
        action: "category3"
    }
    IndexShortcut {
        objectName: "categoryShortcut4"
        targetIndex: 3
        action: "category4"
    }
    IndexShortcut {
        objectName: "categoryShortcut5"
        targetIndex: 4
        action: "category5"
    }
    IndexShortcut {
        objectName: "categoryShortcut6"
        targetIndex: 5
        action: "category6"
    }
    IndexShortcut {
        objectName: "categoryShortcut7"
        targetIndex: 6
        action: "category7"
    }
    IndexShortcut {
        objectName: "categoryShortcut8"
        targetIndex: 7
        action: "category8"
    }
    IndexShortcut {
        objectName: "categoryShortcut9"
        targetIndex: 8
        action: "category9"
    }
    Shortcut {
        objectName: "nextSessionShortcut"
        sequences: window.bindings("nextWindow")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        onActivated: workspace.nextSession(1)
    }
    Shortcut {
        objectName: "previousSessionShortcut"
        sequences: window.bindings("previousWindow")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        onActivated: workspace.nextSession(-1)
    }
    Shortcut {
        objectName: "newAgentShortcut"
        sequences: window.bindings("newAgent")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.openNewAgentDialog()
    }
    Shortcut {
        objectName: "newCategoryShortcut"
        sequences: window.bindings("newCategory")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.openCategoryDialog("add")
    }
    Shortcut {
        objectName: "reloadConfigShortcut"
        sequences: window.bindings("reloadConfig")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.reload()
        }
    }

    component CommandButton: Button {
        id: control

        property bool selected: false
        // Optional machine readout, such as the configured shortcut.
        property string hint: ""
        // Drop the readout rather than overflow a button with a fixed width.
        property bool hintWhenRoom: false
        property color frameColor: selected ? window.focusedBorderColor : window.borderColor

        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        font.pixelSize: window.chromeFont
        padding: 6
        leftPadding: 10
        rightPadding: 10
        implicitHeight: Math.max(window.tabHeight, window.chromeFont + 16)

        contentItem: Item {
            implicitWidth: buttonRow.implicitWidth
            implicitHeight: buttonRow.implicitHeight
            Row {
                id: buttonRow
                anchors.centerIn: parent
                spacing: 8
                PlainText {
                    id: buttonLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: control.text
                    font: control.font
                    color: control.enabled ? window.textColor : window.mutedTextColor
                }
                PlainText {
                    id: buttonHint
                    anchors.verticalCenter: parent.verticalCenter
                    visible: control.hint.length > 0 && (!control.hintWhenRoom
                        || buttonLabel.implicitWidth + buttonRow.spacing + buttonHint.implicitWidth
                           <= control.availableWidth)
                    text: control.hint
                    font.family: window.monoFamily
                    font.pixelSize: window.readoutFont
                    color: window.mutedTextColor
                }
            }
        }

        background: Rectangle {
            radius: window.chromeRadius
            color: control.down ? window.focusedColor :
                   control.hovered ? window.hoveredCardColor : window.cardColor
            border.width: control.selected ? 2 : 1
            border.color: control.frameColor
            Behavior on color {
                enabled: window.motionEnabled
                ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
            }
            Behavior on border.color {
                enabled: window.motionEnabled
                ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
            }
        }
    }

    // Pending requests for the selected agent stay one click away.
    component CommandsButton: CommandButton {
        text: qsTr("Commands")
        hint: window.shortcutText("openCommands")
        enabled: window.interactionArmed
        onClicked: window.openCommandsDialog()
    }

    component StatusMark: Item {
        id: mark

        required property string kind
        readonly property string shape: window.statusShape(kind)
        readonly property color tone: window.statusColor(kind)

        implicitWidth: 8
        implicitHeight: 8
        Rectangle {
            anchors.centerIn: parent
            visible: mark.shape === "dot" || mark.shape === "ring" || mark.shape === "square"
                     || mark.shape === "box"
            width: 8
            height: 8
            radius: mark.shape === "dot" || mark.shape === "ring" ? 4 : 0
            color: mark.shape === "dot" || mark.shape === "square" ? mark.tone : "transparent"
            border.width: mark.shape === "ring" || mark.shape === "box" ? 1.5 : 0
            border.color: mark.tone
        }
        Rectangle {
            anchors.centerIn: parent
            visible: mark.shape === "diamond"
            width: 7
            height: 7
            rotation: 45
            antialiasing: true
            color: mark.tone
        }
        Rectangle {
            anchors.centerIn: parent
            visible: mark.shape === "dash"
            width: 8
            height: 2
            color: mark.tone
        }
    }

    // Pending-request count. Outlined "+N" counts requests in other groups,
    // so it cannot be read as the badge of the group it sits beside.
    component CountBadge: Rectangle {
        id: badge

        required property int count
        property bool elsewhere: false
        visible: count > 0
        radius: window.chromeRadius
        color: elsewhere ? "transparent" : window.attentionColor
        border.width: elsewhere ? 1 : 0
        border.color: window.attentionColor
        implicitWidth: badgeText.implicitWidth + 8
        implicitHeight: Math.max(16, window.readoutFont + 4)
        PlainText {
            id: badgeText
            anchors.centerIn: parent
            text: badge.elsewhere ? "+" + badge.count : badge.count
            color: badge.elsewhere ? window.attentionColor : window.attentionTextColor
            font.family: window.monoFamily
            font.pixelSize: Math.max(10, window.readoutFont - 1)
        }
    }

    component FormField: TextField {
        id: field

        Layout.fillWidth: true
        selectByMouse: true
        color: window.textColor
        placeholderTextColor: window.mutedTextColor
        font.pixelSize: window.chromeFont
        implicitHeight: Math.max(36, window.chromeFont + 20)
        background: Rectangle {
            color: window.backgroundColor
            radius: window.chromeRadius
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? window.focusedBorderColor : window.borderColor
        }
    }

    component FormError: PlainLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: window.faultColor
        font.pixelSize: window.chromeFont
        visible: text.length > 0
    }

    component StageDialog: Dialog {
        id: dialog

        modal: true
        focus: true
        padding: 16
        closePolicy: Popup.CloseOnEscape
        anchors.centerIn: parent
        width: Math.min(460, Math.max(288, (parent ? parent.width : 640) - 28))
        height: Math.min(380, Math.max(240, (parent ? parent.height : 480) - 28))
        standardButtons: Dialog.NoButton
        header: Item { implicitHeight: 0 }
        footer: Item { implicitHeight: 0 }

        background: Rectangle {
            color: window.surfaceColor
            border.color: window.borderColor
            border.width: 1
            radius: window.chromeRadius
        }
        Overlay.modal: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.46)
        }
    }

    component ActionItem: MenuItem {
        font.pixelSize: window.chromeFont
        implicitHeight: Math.max(32, window.chromeFont + 16)
    }

    component MenuSurface: Rectangle {
        color: window.cardColor
        border.color: window.borderColor
        border.width: 1
        radius: window.chromeRadius
    }

    AttentionDialog {
        id: attentionDialog
        palette.window: window.surfaceColor
        palette.base: window.backgroundColor
        palette.button: window.cardColor
        palette.text: window.textColor
        palette.windowText: window.textColor
        palette.buttonText: window.textColor
        palette.mid: window.borderColor
        palette.highlight: window.focusedColor
        palette.highlightedText: window.textColor
        monoFamily: window.monoFamily
        onClosed: preview.deferTerminalFocus()
    }

    NativeDialogs.FolderDialog {
        id: folderPicker
        objectName: "projectFolderPicker"
        title: qsTr("Choose a project folder")
        onAccepted: {
            const url = selectedFolder.toString()
            if (!url.startsWith("file:///")) {
                agentDialog.localError = qsTr("Choose a local project folder.")
                return
            }
            agentDirectoryField.text = decodeURIComponent(url.slice(7))
            agentDialog.localError = ""
            agentDirectoryField.forceActiveFocus()
        }
        onRejected: agentDirectoryField.forceActiveFocus()
    }

    Settings {
        id: settingsDialog
        themeModel: (typeof keymap !== "undefined" && keymap !== null) ? keymap.themes : []
        densityModel: (typeof keymap !== "undefined" && keymap !== null) ? keymap.densities :
                                                                            ["comfortable", "compact", "minimal"]
        currentTheme: (typeof keymap !== "undefined" && keymap !== null) ? keymap.themeName : "lapis"
        currentDensity: window.densityMode
        configPath: (typeof keymap !== "undefined" && keymap !== null) ? keymap.sourcePath : ""
        configDiagnostic: (typeof keymap !== "undefined" && keymap !== null) ? keymap.diagnostic : ""
        shortcutHint: window.shortcutText("openSettings")
        fontFamilies: preview.monospaceFamilies
        fontFamily: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontFamily : ""
        resolvedFontFamily: window.monoFamily
        fontSize: liveTerminal.fontPixelSize
        fontSizeMinimum: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSizeMinimum : 14
        fontSizeMaximum: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSizeMaximum : 14
        fontSizeDefault: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSizeDefault : 14
        motionDuration: window.motionDuration
        motionEnabled: window.motionEnabled
        finishSound: (typeof keymap !== "undefined" && keymap !== null) ? keymap.finishSound : true
        keepAwake: (typeof keymap !== "undefined" && keymap !== null) ? keymap.keepAwake : true
        loginAvailable: window.desktopAvailable && desktop.launchAtLoginAvailable
        launchAtLogin: window.desktopAvailable && desktop.launchAtLogin
        updatesAvailable: window.desktopAvailable && desktop.updatesAvailable
        shortcutRows: visible ? window.commandEntries().filter(entry => entry.shortcut.length > 0)
                                         .map(entry => ({label: entry.label, keys: entry.shortcut})) : []
        visualThemes: visualView.themes.map(theme => visualView.colorsOf(theme))
        visualFonts: visualView.readingFonts
        visualCodeFonts: visualView.codeFonts
        visualEditors: workspace.transcript
                       ? workspace.transcript.editors().map(e => ({ name: e.id, label: e.label, family: "" }))
                       : []
        visual: window.visualSettings
        visualSize: visualView.fontSize
        onVisualChosen: function(key, value) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setVisual(key, value)
        }
        onLaunchAtLoginChosen: function(on) { if (window.desktopAvailable) desktop.setLaunchAtLogin(on) }
        onCheckUpdates: if (window.desktopAvailable) desktop.checkForUpdates()
        onFinishSoundChosen: function(on) { if (typeof keymap !== "undefined" && keymap !== null) keymap.setFinishSound(on) }
        onKeepAwakeChosen: function(on) { if (typeof keymap !== "undefined" && keymap !== null) keymap.setKeepAwake(on) }
        onChimePlayed: function(needsYou) { if (typeof alerts !== "undefined" && alerts !== null) alerts.preview(needsYou) }
        onClosed: preview.deferTerminalFocus()
        onFontFamilyChosen: function(name) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setTerminalFontFamily(name)
        }
        onFontSizeChosen: function(pixels) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setTerminalFontSize(pixels)
        }
        onThemeChosen: function(name) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setTheme(name)
        }
        onDensityChosen: function(name) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setDensity(name)
        }
    }

    StageDialog {
        id: agentDialog
        objectName: "agentDialog"
        title: qsTr("New agent")
        anchors.centerIn: null
        x: (window.width - width) / 2
        y: Math.max(16, (window.height - 360) / 2)
        height: Math.min(400, window.height - 32, agentForm.implicitHeight + topPadding + bottomPadding)
        property string localError: ""
        property int phase: 0
        property var harnesses: []
        readonly property var choices: harnesses
        property string selectedHarness: "claude"
        // "" for this Mac, else an ssh host from the ssh config. Left and right
        // change it while up and down choose the CLI.
        property var machines: [""]
        property string selectedMachine: ""
        readonly property bool remote: selectedMachine.length > 0
        // The machine whose folder the field holds.
        property string folderMachine: ""
        function chooseMachine(machine) {
            selectedMachine = machine
            window.lastMachine = machine
        }
        function stepMachine(delta) {
            if (machines.length < 2) return
            const at = Math.max(0, machines.indexOf(selectedMachine))
            chooseMachine(machines[(at + delta + machines.length) % machines.length])
        }
        // The config's newAgent folder for that machine, else its home.
        function machineFolder() {
            const defaults = workspace.agentDefaults()
            if (remote)
                return (defaults.machines && defaults.machines[selectedMachine]) || "~"
            const folder = defaults.folder === "~" ? workspace.homeDirectory :
                           defaults.folder && defaults.folder.startsWith("~/") ? workspace.homeDirectory + defaults.folder.slice(1) :
                           defaults.folder || ""
            return (folder.length > 0 ? folder.replace(/\/+$/, "") : workspace.homeDirectory) + "/"
        }
        // This agent's model (one the CLI lists) and approval mode. The mode
        // stays across CLIs; a CLI without it uses its nearest, less access
        // first, and the preference returns on a CLI that has it.
        property string selectedModel: ""
        property string preferredMode: "full"
        readonly property var selectedItem: harnesses.find(h => h.id === selectedHarness)
        readonly property string harnessName: selectedItem ? selectedItem.name : selectedHarness
        readonly property var modelChoices: selectedItem && selectedItem.models ? selectedItem.models : []
        readonly property var chosenModel: modelChoices.find(m => m.id === selectedModel)
                                           || modelChoices.find(m => m.default) || modelChoices[0] || null
        readonly property var modeOptions: [{id: "edits", name: qsTr("Accept edits")},
                                            {id: "auto", name: qsTr("Auto")},
                                            {id: "full", name: qsTr("Full access")}]
        function offers(mode) {
            return !!(selectedItem && selectedItem.modes && selectedItem.modes.some(m => m.id === mode))
        }
        readonly property string selectedMode: {
            const order = modeOptions.map(m => m.id)
            const at = Math.max(0, order.indexOf(preferredMode))
            for (const index of [at, at - 1, at - 2, at + 1, at + 2])
                if (index >= 0 && index < order.length && offers(order[index]))
                    return order[index]
            return ""
        }
        function chooseModel(id) {
            selectedModel = id
            const models = Object.assign({}, window.lastModels)
            models[selectedHarness] = id
            window.lastModels = models
        }
        function chooseMode(id) {
            preferredMode = id
            window.lastMode = id
        }
        function chooseHarness(index) {
            const item = choices[index]
            // Another machine's CLIs are its own; this Mac cannot see them.
            if (!item || (!item.installed && !remote)) return
            selectedHarness = item.id
            selectedModel = window.lastModels[item.id] || ""
            window.lastHarness = item.id
            if (folderMachine !== selectedMachine) {
                agentDirectoryField.text = machineFolder()
                folderMachine = selectedMachine
            }
            phase = 1
            Qt.callLater(function() {
                agentDirectoryField.forceActiveFocus()
                agentDirectoryField.cursorPosition = agentDirectoryField.text.length
                folderResults.model = []
                if (!agentDialog.remote)
                    folderSearch.restart()
            })
        }
        onOpened: Qt.callLater(function() { harnessChoices.forceActiveFocus() })
        onClosed: {
            agentDialog.localError = ""
            folderSearch.stop()
            directoryModel.folder = ""
        }

        function completeFolder() {
            const index = folderResults.currentIndex >= 0 ? folderResults.currentIndex : 0
            if (index < folderResults.count) {
                agentDirectoryField.text = folderResults.model[index].path
                agentDirectoryField.cursorPosition = agentDirectoryField.text.length
            }
            agentDirectoryField.forceActiveFocus()
        }
        property string folderParent: ""
        property string folderPrefix: ""
        // The folders with the most recent and frequent agent work come
        // first, then the rest by name with _folders last.
        function refreshFolders() {
            if (!folderResults) return
            const names = []
            if (directoryModel.status === FolderListModel.Ready) {
                for (let i = 0; i < Math.min(directoryModel.count, 4096); ++i) {
                    const name = directoryModel.get(i, "fileName")
                    const path = directoryModel.get(i, "filePath")
                    if (path.slice(0, path.lastIndexOf("/") + 1) === folderParent
                            && name.toLowerCase().startsWith(folderPrefix.toLowerCase()))
                        names.push(name)
                }
            }
            const ordered = window.conversationsAvailable ? conversations.orderFolders(folderParent, names) : names
            folderResults.model = ordered.slice(0, 100).map(name => ({name: name, path: folderParent + name + "/"}))
            folderResults.currentIndex = -1
        }
        FolderListModel {
            id: directoryModel
            showFiles: false
            showDirs: true
            showDotAndDotDot: false
            showHidden: agentDialog.folderPrefix.startsWith(".")
            showOnlyReadable: true
            sortCaseSensitive: false
            folder: ""
            onCountChanged: agentDialog.refreshFolders()
            onStatusChanged: agentDialog.refreshFolders()
        }
        Timer {
            id: folderSearch
            interval: 100
            onTriggered: {
                if (!agentDialog.visible || agentDialog.phase !== 1 || agentDialog.remote) return
                let path = agentDirectoryField.text
                if (path === "~" || path.startsWith("~/"))
                    path = workspace.homeDirectory + path.slice(1)
                agentDialog.folderParent = path.startsWith("/") ? path.slice(0, path.lastIndexOf("/") + 1) : ""
                agentDialog.folderPrefix = path.slice(path.lastIndexOf("/") + 1)
                directoryModel.folder = agentDialog.folderParent.length ?
                            "file://" + agentDialog.folderParent.split("/").map(encodeURIComponent).join("/") : ""
                agentDialog.refreshFolders()
            }
        }
        contentItem: ScrollView {
            id: agentScroll
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: agentForm
                width: agentScroll.availableWidth
                spacing: 8

                RowLayout {
                    objectName: "machineChoices"
                    visible: agentDialog.phase === 0 && agentDialog.machines.length > 1
                    Layout.fillWidth: true
                    spacing: 6
                    PlainLabel {
                        text: qsTr("Machine")
                        color: window.mutedTextColor
                        Layout.preferredWidth: 60
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: agentDialog.machines
                            delegate: CommandButton {
                                required property var modelData
                                objectName: "machine_" + (modelData.length > 0 ? modelData : "mac")
                                text: modelData.length > 0 ? modelData : qsTr("This Mac")
                                selected: modelData === agentDialog.selectedMachine
                                onClicked: agentDialog.chooseMachine(modelData)
                            }
                        }
                    }
                    PlainLabel {
                        text: "\u2190 \u2192"
                        color: window.mutedTextColor
                        font.family: window.monoFamily
                    }
                }
                PlainLabel {
                    visible: agentDialog.phase === 0
                    text: qsTr("Choose agent")
                    color: window.textColor
                    font.bold: true
                }
                ListView {
                    id: harnessChoices
                    objectName: "harnessChoices"
                    visible: agentDialog.phase === 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(6, count) * 44
                    model: agentDialog.choices
                    clip: true
                    keyNavigationEnabled: true
                    Keys.onReturnPressed: agentDialog.chooseHarness(currentIndex)
                    Keys.onEnterPressed: agentDialog.chooseHarness(currentIndex)
                    Keys.onLeftPressed: agentDialog.stepMachine(-1)
                    Keys.onRightPressed: agentDialog.stepMachine(1)
                    onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: ItemDelegate {
                        id: harnessChoice
                        required property var modelData
                        required property int index
                        objectName: "harness_" + modelData.id
                        width: harnessChoices.width
                        height: 44
                        focusPolicy: Qt.NoFocus
                        Accessible.name: modelData.name + (modelData.installed ? "" : qsTr(", not installed"))
                        onClicked: { harnessChoices.currentIndex = index; agentDialog.chooseHarness(index) }
                        background: Rectangle {
                            color: harnessChoice.index === harnessChoices.currentIndex ? window.focusedColor :
                                   harnessChoice.hovered ? window.hoveredCardColor : window.surfaceColor
                        }
                        contentItem: RowLayout {
                            spacing: 10
                            AgentMark {
                                harnessId: harnessChoice.modelData.id
                                ink: harnessChoice.modelData.installed || agentDialog.remote ? window.textColor : window.mutedTextColor
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                            }
                            PlainLabel {
                                text: harnessChoice.modelData.name
                                color: harnessChoice.modelData.installed || agentDialog.remote ? window.textColor : window.mutedTextColor
                                Layout.fillWidth: true
                            }
                            PlainLabel {
                                visible: !harnessChoice.modelData.installed && !agentDialog.remote
                                text: qsTr("Not installed")
                                color: window.mutedTextColor
                            }
                        }
                    }
                }
                RowLayout {
                    visible: agentDialog.phase === 1
                    CommandButton {
                        text: "‹"
                        Accessible.name: qsTr("Choose another agent")
                        onClicked: { agentDialog.phase = 0; harnessChoices.forceActiveFocus() }
                    }
                    AgentMark { harnessId: agentDialog.selectedHarness; ink: window.textColor; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                    PlainLabel { text: agentDialog.harnessName; color: window.textColor; font.pixelSize: window.chromeFont + 2 }
                    PlainLabel {
                        objectName: "agentMachine"
                        visible: agentDialog.remote
                        text: qsTr("on %1").arg(agentDialog.selectedMachine)
                        color: window.mutedTextColor
                        font.pixelSize: window.chromeFont + 2
                    }
                }
                // Model and approval mode for this agent, as the CLI's own flags.
                Flow {
                    visible: agentDialog.phase === 1 && agentDialog.modelChoices.length > 0
                    Layout.fillWidth: true
                    spacing: 6
                    PlainLabel { text: qsTr("Model"); color: window.mutedTextColor; height: window.tabHeight; verticalAlignment: Text.AlignVCenter; width: 52 }
                    Repeater {
                        model: agentDialog.modelChoices
                        delegate: CommandButton {
                            required property var modelData
                            objectName: "model_" + modelData.id
                            text: modelData.name
                            selected: agentDialog.chosenModel !== null && modelData.id === agentDialog.chosenModel.id
                            onClicked: agentDialog.chooseModel(modelData.id)
                        }
                    }
                }
                // Always one of three; a CLI without one shows it unavailable.
                RowLayout {
                    visible: agentDialog.phase === 1 && agentDialog.selectedMode.length > 0
                    Layout.fillWidth: true
                    spacing: 6
                    PlainLabel { text: qsTr("Mode"); color: window.mutedTextColor; Layout.preferredHeight: window.tabHeight; verticalAlignment: Text.AlignVCenter; Layout.preferredWidth: 52 }
                    Repeater {
                        model: agentDialog.modeOptions
                        delegate: CommandButton {
                            required property var modelData
                            objectName: "mode_" + modelData.id
                            text: modelData.name
                            enabled: agentDialog.offers(modelData.id)
                            selected: modelData.id === agentDialog.selectedMode
                            Layout.fillWidth: true
                            Accessible.description: enabled ? "" : qsTr("%1 has no such mode").arg(agentDialog.harnessName)
                            onClicked: agentDialog.chooseMode(modelData.id)
                        }
                    }
                }
                RowLayout {
                    visible: agentDialog.phase === 1
                    Layout.fillWidth: true
                    FormField {
                        id: agentDirectoryField
                        objectName: "agentDirectoryField"
                        placeholderText: agentDialog.remote ? qsTr("Folder on %1 (~ is its home)").arg(agentDialog.selectedMachine)
                                                            : workspace.homeDirectory + "/"
                        font.family: window.monoFamily
                        Accessible.name: qsTr("Project folder")
                        maximumLength: 4096
                        onAccepted: {
                            if (folderResults.currentIndex >= 0) agentDialog.completeFolder()
                            else window.commitNewAgent()
                        }
                        onTextChanged: { folderResults.model = []; folderResults.currentIndex = -1; folderSearch.restart() }
                        onTextEdited: agentDialog.localError = ""
                        Keys.onDownPressed: folderResults.currentIndex = Math.min(folderResults.count - 1, folderResults.currentIndex + 1)
                        Keys.onUpPressed: folderResults.currentIndex = Math.max(-1, folderResults.currentIndex - 1)
                        Keys.onTabPressed: agentDialog.completeFolder()
                        Keys.onEscapePressed: { agentDialog.phase = 0; harnessChoices.forceActiveFocus() }

                    }
                    CommandButton {
                        objectName: "browseProjectFolder"
                        visible: !agentDialog.remote
                        text: "…"
                        Accessible.name: qsTr("Browse project folders")
                        onClicked: folderPicker.open()
                    }
                }
                ListView {
                    id: folderResults
                    objectName: "folderResults"
                    visible: agentDialog.phase === 1 && !agentDialog.remote
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(5, count) * 34
                    clip: true
                    currentIndex: -1
                    model: []
                    onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        width: folderResults.width
                        height: 34
                        focusPolicy: Qt.NoFocus
                        text: modelData.name + "/"
                        font.family: window.monoFamily
                        contentItem: PlainText {
                            text: parent.text
                            color: window.textColor
                            font: parent.font
                            elide: Text.ElideMiddle
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: parent.index === folderResults.currentIndex ? window.focusedColor :
                                   parent.hovered ? window.hoveredCardColor : window.surfaceColor
                        }
                        onClicked: { folderResults.currentIndex = index; agentDialog.completeFolder() }
                    }
                }
                FormError {
                    objectName: "agentFormError"
                    visible: text.length > 0
                    text: agentDialog.localError.length > 0 ? agentDialog.localError : workspace.workspaceError
                }
                RowLayout {
                    visible: agentDialog.phase === 1
                    Layout.alignment: Qt.AlignRight
                    spacing: 8
                    CommandButton {
                        text: qsTr("Cancel")
                        onClicked: agentDialog.close()
                    }
                    CommandButton {
                        objectName: "createAgentConfirm"
                        text: qsTr("Start")
                        selected: true
                        onClicked: window.commitNewAgent()
                    }
                }
            }
        }
    }

    StageDialog {
        id: closeAgentDialog
        objectName: "closeAgentDialog"
        title: reachable ? qsTr("End agent") : qsTr("Close tab")
        height: Math.min(window.height - 32, closeForm.implicitHeight + topPadding + bottomPadding)
        property string sessionId: ""
        property string harnessId: ""
        property string agentName: ""
        property string place: ""
        property bool reachable: true
        function confirm() {
            const id = sessionId
            const abandon = !reachable
            close()
            if (!workspace.closeSession(id, abandon))
                preview.deferTerminalFocus()
        }
        onOpened: Qt.callLater(function() { closeConfirm.forceActiveFocus() })

        contentItem: ColumnLayout {
            id: closeForm
            spacing: 12
            RowLayout {
                spacing: 10
                AgentMark {
                    harnessId: closeAgentDialog.harnessId
                    ink: window.textColor
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                }
                PlainLabel {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: closeAgentDialog.reachable ?
                              qsTr("End %1 in %2?").arg(closeAgentDialog.agentName).arg(closeAgentDialog.place) :
                              qsTr("Close the %1 tab for %2?").arg(closeAgentDialog.agentName).arg(closeAgentDialog.place)
                    color: window.textColor
                    font.pixelSize: window.chromeFont + 1
                    font.bold: true
                    wrapMode: Text.Wrap
                }
            }
            PlainLabel {
                Layout.fillWidth: true
                text: closeAgentDialog.reachable ?
                          qsTr("Its process stops and the tab closes.") :
                          qsTr("lapis can't reach this agent. If it is still running, it keeps running without a tab.")
                color: window.mutedTextColor
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                CommandButton {
                    objectName: "closeAgentCancel"
                    text: qsTr("Cancel")
                    hint: "Esc"
                    onClicked: closeAgentDialog.close()
                }
                CommandButton {
                    id: closeConfirm
                    objectName: "closeAgentConfirm"
                    text: closeAgentDialog.reachable ? qsTr("End agent") : qsTr("Close tab")
                    hint: "↩"
                    selected: true
                    focusPolicy: Qt.StrongFocus
                    frameColor: window.faultColor
                    Keys.onReturnPressed: closeAgentDialog.confirm()
                    Keys.onEnterPressed: closeAgentDialog.confirm()
                    onClicked: closeAgentDialog.confirm()
                }
            }
        }
    }

    StageDialog {
        id: categoryDialog
        objectName: "categoryDialog"
        title: mode === "rename" ? qsTr("Rename category") : qsTr("New category")
        property string mode: "add"
        property string initialName: ""
        property string localError: ""
        onOpened: Qt.callLater(function() {
            categoryNameField.text = categoryDialog.initialName
            categoryNameField.forceActiveFocus()
            categoryNameField.selectAll()
        })
        onClosed: {
            categoryDialog.localError = ""
            window.pendingCategoryAgents = []
        }

        contentItem: ScrollView {
            id: categoryScroll
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                width: categoryScroll.availableWidth
                spacing: 8

                PlainLabel {
                    Layout.fillWidth: true
                    text: categoryDialog.mode === "rename" ? qsTr("Rename category") : qsTr("New category")
                    color: window.textColor
                    font.pixelSize: window.chromeFont + 2
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                PlainLabel {
                    Layout.fillWidth: true
                    text: categoryDialog.mode === "rename" ?
                              qsTr("Agents in this category stay where they are.") :
                              qsTr("A category keeps its own agents and the one you last opened.")
                    color: window.mutedTextColor
                    wrapMode: Text.WordWrap
                }
                FormField {
                    id: categoryNameField
                    objectName: "categoryNameField"
                    placeholderText: qsTr("Category name")
                    maximumLength: 80
                    onAccepted: window.commitCategory()
                    onTextEdited: categoryDialog.localError = ""
                }
                FormError {
                    objectName: "categoryFormError"
                    text: categoryDialog.localError.length > 0 ? categoryDialog.localError : workspace.workspaceError
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 8
                    CommandButton {
                        text: qsTr("Cancel")
                        onClicked: categoryDialog.close()
                    }
                    CommandButton {
                        objectName: "categoryConfirm"
                        text: categoryDialog.mode === "rename" ? qsTr("Rename") : qsTr("Add category")
                        selected: true
                        onClicked: window.commitCategory()
                    }
                }
            }
        }
    }

    StageDialog {
        id: renameAgentDialog
        objectName: "renameAgentDialog"
        title: qsTr("Rename agent")
        property string sessionId: ""
        property string initialTitle: ""
        property string localError: ""
        onOpened: Qt.callLater(function() {
            renameAgentField.text = renameAgentDialog.initialTitle
            renameAgentField.forceActiveFocus()
            renameAgentField.selectAll()
        })
        onClosed: renameAgentDialog.localError = ""

        contentItem: ScrollView {
            id: renameScroll
            clip: true
            contentWidth: availableWidth
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                width: renameScroll.availableWidth
                spacing: 8

                PlainLabel {
                    Layout.fillWidth: true
                    text: qsTr("Rename agent")
                    color: window.textColor
                    font.pixelSize: window.chromeFont + 2
                    font.bold: true
                }
                FormField {
                    id: renameAgentField
                    objectName: "renameAgentField"
                    placeholderText: qsTr("Agent name")
                    maximumLength: 80
                    onAccepted: window.commitRenameAgent()
                    onTextEdited: renameAgentDialog.localError = ""
                }
                FormError {
                    objectName: "renameAgentError"
                    text: renameAgentDialog.localError.length > 0 ? renameAgentDialog.localError :
                                                                   workspace.workspaceError
                }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 8
                    CommandButton {
                        text: qsTr("Cancel")
                        onClicked: renameAgentDialog.close()
                    }
                    CommandButton {
                        objectName: "renameAgentConfirm"
                        text: qsTr("Rename")
                        selected: true
                        onClicked: window.commitRenameAgent()
                    }
                }
            }
        }
    }

    Menu {
        id: categoryMenu
        objectName: "categoryMenu"
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        focus: true
        onClosed: window.onMenuClosed()
        background: MenuSurface {}

        ActionItem {
            objectName: "renameCategoryAction"
            text: qsTr("Rename")
            enabled: workspace.activeCategoryId.length > 0
            onTriggered: window.openCategoryDialog("rename")
        }
        ActionItem {
            objectName: "removeCategoryAction"
            enabled: window.canRemoveActiveCategory()
            text: workspace.categories.length <= 1 ? qsTr("Keep at least one category") :
                  workspace.categorySessions.length > 0 ? qsTr("Move agents out before removing") :
                  qsTr("Remove category")
            onTriggered: window.removeActiveCategory()
        }
        MenuSeparator {}
        ActionItem {
            objectName: "appearanceAction"
            text: qsTr("Appearance")
            onTriggered: window.openSettingsDialog()
        }
    }

    Menu {
        id: agentMenu
        objectName: "agentMenu"
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        focus: true
        onClosed: window.onMenuClosed()
        background: MenuSurface {}

        ActionItem {
            enabled: false
            text: workspace.focusedSession ? workspace.focusedSession.title : ""
            visible: text.length > 0
        }
        ActionItem {
            objectName: "sessionDiagnostic"
            enabled: false
            text: workspace.focusedSession ? (workspace.focusedSession.attentionDiagnostic || workspace.focusedSession.activity) : ""
            visible: text.length > 0
            contentItem: PlainLabel {
                text: parent.text
                color: window.mutedTextColor
                wrapMode: Text.WrapAnywhere
                font.pixelSize: window.chromeFont
            }
            implicitHeight: Math.max(40, contentItem.implicitHeight + 16)
        }
        ActionItem {
            objectName: "renameAgentAction"
            text: qsTr("Rename")
            enabled: workspace.focusedSession !== null
            onTriggered: window.openRenameDialog()
        }
        ActionItem {
            objectName: "moveAgentEarlier"
            text: qsTr("Move earlier")
            enabled: window.focusedTabIndex() > 0
            onTriggered: window.moveFocused(-1)
        }
        ActionItem {
            objectName: "moveAgentLater"
            text: qsTr("Move later")
            enabled: {
                const index = window.focusedTabIndex()
                return index >= 0 && index < workspace.categorySessions.length - 1
            }
            onTriggered: window.moveFocused(1)
        }
        Menu {
            id: moveMenu
            objectName: "moveAgentMenu"
            title: qsTr("Move to category")
            onClosed: window.onMenuClosed()
            background: MenuSurface {}

            ActionItem {
                visible: workspace.categories.length < 2
                enabled: false
                text: qsTr("Add another category first")
            }
            Repeater {
                model: workspace.categories
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    font.pixelSize: window.chromeFont
                    implicitHeight: Math.max(32, window.chromeFont + 16)
                    enabled: modelData.id !== workspace.activeCategoryId
                    onTriggered: {
                        const session = workspace.focusedSession
                        if (session)
                            workspace.moveSession(session.sessionId, modelData.id)
                    }
                }
            }
        }
        MenuSeparator {}
        ActionItem {
            objectName: "closeAgentAction"
            text: qsTr("Close")
            enabled: workspace.focusedSession !== null
            onTriggered: window.closeFocusedAgent()
        }
        MenuSeparator {
            visible: workspace.focusedSession !== null && workspace.focusedSession.live
        }
        ActionItem {
            objectName: "historyOlderAction"
            text: qsTr("Older history")
            visible: workspace.focusedSession !== null && workspace.focusedSession.live
            enabled: workspace.focusedSession !== null && !workspace.focusedSession.historyRequestPending
            onTriggered: if (workspace.focusedSession)
                             workspace.focusedSession.olderHistory()
        }
        ActionItem {
            objectName: "historyNewerAction"
            text: qsTr("Newer history")
            visible: workspace.focusedSession !== null && workspace.focusedSession.live
            enabled: workspace.focusedSession !== null && workspace.focusedSession.historyActive
                     && !workspace.focusedSession.historyRequestPending
            onTriggered: if (workspace.focusedSession)
                             workspace.focusedSession.newerHistory()
        }
        ActionItem {
            objectName: "historyLiveAction"
            text: qsTr("Back to live")
            visible: workspace.focusedSession !== null && workspace.focusedSession.live
            enabled: workspace.focusedSession !== null && workspace.focusedSession.historyActive
            onTriggered: if (workspace.focusedSession)
                             workspace.focusedSession.returnToLive()
        }
    }

    Connections {
        target: workspace
        function onFocusChanged() {
            window.keyboardOwnershipReady()
            agentTabs.revealFocused()

            if (!window.inputBlocked)
                preview.deferTerminalFocus()
        }
        function onSessionsChanged() {
            agentTabs.revealFocused()
        }
        function onCategoryChanged() {
            categoryList.revealActive()
            agentTabs.revealFocused()
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        Rectangle {
            id: categoryRail
            objectName: "categoryRail"
            visible: !window.narrow && window.sidebarExpanded
            Layout.preferredWidth: window.railWidth
            Layout.maximumWidth: window.railWidth
            Layout.fillHeight: true
            color: window.surfaceColor
            border.color: window.borderColor
            border.width: 1
            radius: window.chromeRadius

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                anchors.topMargin: 0
                spacing: 8

                ListView {
                    id: categoryList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    cacheBuffer: 8000
                    spacing: 4
                    model: workspace.categories
                    highlight: null
                    highlightFollowsCurrentItem: false
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    function revealActive() {
                        const index = window.categoryIndex(workspace.activeCategoryId)
                        if (index >= 0)
                            positionViewAtIndex(index, ListView.Contain)
                    }

                    // Categories are quiet group labels: no resting fill, a leading
                    // edge and full ink for the active group, and a mono recall number
                    // for the first four. Agent tabs remain the filled session cells.
                    delegate: Button {
                        id: categoryButton
                        required property var modelData
                        required property int index
                        objectName: "category_" + modelData.id
                        readonly property bool marked: modelData.id === workspace.activeCategoryId
                        readonly property string recall: window.categoryRecall(index)
                        width: categoryList.width
                        height: window.tabHeight
                        padding: 0
                        focusPolicy: Qt.NoFocus
                        hoverEnabled: true
                        enabled: window.interactionArmed
                        onClicked: workspace.selectCategory(modelData.id)
                        Accessible.name: modelData.unseenCount > 0 ?
                                             qsTr("%1, %2 waiting").arg(modelData.name).arg(modelData.unseenCount) :
                                             modelData.name
                        ToolTip.visible: hovered && recall.length > 0
                        ToolTip.delay: 600
                        ToolTip.text: window.shortcutText("category" + (index + 1))

                        background: Rectangle {
                            radius: window.chromeRadius
                            color: categoryDrop.containsDrag && categoryDrop.acceptsAgents ?
                                       Qt.alpha(window.focusedBorderColor, 0.18) :
                                   categoryButton.hovered && !categoryButton.marked ? window.hoveredCardColor :
                                                                                       window.surfaceColor
                            border.width: categoryDrop.containsDrag && categoryDrop.acceptsAgents ? 1 : 0
                            border.color: window.focusedBorderColor
                            Behavior on color {
                                enabled: window.motionEnabled
                                ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                            }
                            Rectangle {
                                objectName: "categoryMarker"
                                x: 0
                                width: 2
                                height: parent.height - 10
                                anchors.verticalCenter: parent.verticalCenter
                                color: categoryButton.marked ? window.focusedBorderColor : "transparent"
                                Behavior on color {
                                    enabled: window.motionEnabled
                                    ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                                }
                            }
                        }

                        contentItem: Item {
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 6
                                spacing: 8
                                PlainText {
                                    text: categoryButton.recall
                                    visible: text.length > 0
                                    color: categoryButton.marked ? window.textColor : window.mutedTextColor
                                    font.family: window.monoFamily
                                    font.pixelSize: window.readoutFont
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                PlainText {
                                    text: categoryButton.modelData.name
                                    color: categoryButton.marked || categoryButton.hovered ? window.textColor :
                                                                                             window.mutedTextColor
                                    font.pixelSize: window.chromeFont
                                    font.weight: categoryButton.marked ? Font.DemiBold : Font.Normal
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    verticalAlignment: Text.AlignVCenter
                                }
                                // An agent here finished or needs you and has not been
                                // looked at; it pulses with the agent's card.
                                Rectangle {
                                    id: unseenDot
                                    objectName: "categoryUnseen_" + categoryButton.modelData.id
                                    visible: categoryButton.modelData.unseenCount > 0 && !categoryButton.marked
                                    Layout.preferredWidth: 6
                                    Layout.preferredHeight: 6
                                    Layout.alignment: Qt.AlignVCenter
                                    radius: 3
                                    color: window.textColor
                                    SequentialAnimation on opacity {
                                        running: unseenDot.visible && window.motionEnabled
                                        loops: Animation.Infinite
                                        NumberAnimation { from: 0.9; to: 0.25; duration: 900; easing.type: Easing.InOutSine }
                                        NumberAnimation { from: 0.25; to: 0.9; duration: 900; easing.type: Easing.InOutSine }
                                    }
                                }
                            }
                        }

                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            enabled: window.interactionArmed
                            onTapped: {
                                workspace.selectCategory(categoryButton.modelData.id)
                                window.popupAt(categoryMenu, categoryButton)
                            }
                        }
                        // Drag a category up or down the rail to reorder it.
                        DragHandler {
                            target: null
                            enabled: window.interactionArmed
                            xAxis.enabled: false
                            onActiveChanged: active ?
                                window.beginCategoryDrag(categoryButton.modelData.id, categoryButton.modelData.name,
                                                         centroid.scenePosition) :
                                window.endDrag()
                            onCentroidChanged: if (active)
                                                   window.moveDragGhost(centroid.scenePosition)
                        }
                        // Agents dropped here move to this category; a category
                        // dropped here goes above or below this one.
                        DropArea {
                            id: categoryDrop
                            anchors.fill: parent
                            keys: ["lapis-agents", "lapis-category"]
                            readonly property bool acceptsAgents: window.draggedAgents.length > 0
                                                                   && !categoryButton.marked
                            property bool below: false
                            onPositionChanged: function(drag) { below = drag.y > height / 2 }
                            onDropped: function(drop) {
                                const category = categoryButton.modelData.id
                                if (window.draggedCategory.length > 0) {
                                    const at = categoryButton.index + (below ? 1 : 0)
                                    window.queueDrop(() => window.dropCategory(at))
                                } else {
                                    window.queueDrop(() => window.dropOnCategory(category))
                                }
                                drop.accept()
                            }
                        }
                        Rectangle {
                            objectName: "categoryDropMarker"
                            visible: categoryDrop.containsDrag && !categoryDrop.acceptsAgents
                                     && window.draggedCategory !== categoryButton.modelData.id
                            x: 4
                            width: parent.width - 8
                            height: 2
                            y: categoryDrop.below ? parent.height - 1 : -1
                            color: window.focusedBorderColor
                        }
                    }

                    // A quiet plus right under the last category, in the recall
                    // column, with its shortcut as a readout.
                    footer: Item {
                        width: categoryList.width
                        height: window.tabHeight + categoryList.spacing
                        Button {
                            id: newCategoryButton
                            objectName: "newCategoryButton"
                            y: categoryList.spacing
                            width: parent.width
                            height: window.tabHeight
                            padding: 0
                            focusPolicy: Qt.NoFocus
                            hoverEnabled: true
                            enabled: window.interactionArmed
                            onClicked: window.openCategoryDialog("add")
                            Accessible.name: qsTr("New category")
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("New category")

                            // Agents dropped on the + start a category of their own.
                            DropArea {
                                id: newCategoryDrop
                                anchors.fill: parent
                                keys: ["lapis-agents"]
                                onDropped: function(drop) {
                                    window.queueDrop(() => window.dropOnNewCategory())
                                    drop.accept()
                                }
                            }
                            background: Rectangle {
                                radius: window.chromeRadius
                                border.width: newCategoryDrop.containsDrag ? 1 : 0
                                border.color: window.focusedBorderColor
                                color: newCategoryDrop.containsDrag ? Qt.alpha(window.focusedBorderColor, 0.18) :
                                       newCategoryButton.hovered ? window.hoveredCardColor : window.surfaceColor
                                Behavior on color {
                                    enabled: window.motionEnabled
                                    ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                                }
                            }
                            contentItem: Item {
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 6
                                    spacing: 8
                                    PlainText {
                                        text: "+"
                                        color: newCategoryButton.hovered ? window.textColor : window.mutedTextColor
                                        font.pixelSize: window.chromeFont
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    PlainText {
                                        objectName: "newCategoryHint"
                                        text: window.shortcutText("newCategory")
                                        color: window.mutedTextColor
                                        font.family: window.monoFamily
                                        font.pixelSize: window.readoutFont
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                }
                            }
                        }
                    }
                }
                // The visual view for the selected agent, or for every agent.
                PlainText {
                    text: qsTr("Visual view")
                    color: window.mutedTextColor
                    font.pixelSize: window.readoutFont
                    Layout.topMargin: 4
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    CommandButton {
                        objectName: "visualThisAgent"
                        text: qsTr("This agent")
                        selected: window.visualActive
                        enabled: window.interactionArmed && workspace.focusedSession !== null
                                 && (window.visualActive || window.visualProbe >= 0
                                     && workspace.conversationFile(workspace.focusedSession.sessionId).length > 0)
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        onClicked: window.toggleVisual()
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: enabled ? qsTr("Switch this agent between its terminal and the thread")
                                              : qsTr("Only for a Claude Code agent on this Mac, once its conversation is saved")
                    }
                    CommandButton {
                        objectName: "visualAllAgents"
                        text: qsTr("All agents")
                        selected: window.visualAll
                        enabled: window.interactionArmed && typeof keymap !== "undefined" && keymap !== null
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        onClicked: window.toggleVisualAll()
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Show every Claude Code agent on this Mac as a thread")
                    }
                }
                CommandsButton {
                    objectName: categoryRail.visible ? "commandsButton" : "railCommands"
                    Layout.fillWidth: true
                    Layout.bottomMargin: 0
                }
            }
        }

        ColumnLayout {
            id: stageColumn
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 6

            RowLayout {
                id: categoryBar
                visible: window.narrow || !window.sidebarExpanded
                Layout.fillWidth: true
                spacing: 6

                ComboBox {
                    id: categorySelector
                    objectName: "categorySelector"
                    visible: window.narrow || !window.sidebarExpanded
                    enabled: window.interactionArmed || popup.visible
                    Layout.fillWidth: true
                    implicitHeight: window.tabHeight
                    model: workspace.categories
                    textRole: "name"
                    font.pixelSize: window.chromeFont
                    currentIndex: {
                        const index = window.categoryIndex(workspace.activeCategoryId)
                        if (index >= 0)
                            return index
                        return workspace.categories.length > 0 ? 0 : -1
                    }
                    onActivated: function(index) { window.selectCategoryByIndex(index) }

                    // Same group grammar as the rail: a leading edge for the active
                    // group, and a count of requests waiting in the hidden groups.
                    background: Rectangle {
                        radius: window.chromeRadius
                        color: categorySelector.hovered ? window.hoveredCardColor : window.surfaceColor
                        border.width: 1
                        border.color: window.borderColor
                        Rectangle {
                            x: 1
                            width: 2
                            height: parent.height - 10
                            anchors.verticalCenter: parent.verticalCenter
                            color: window.focusedBorderColor
                        }
                    }
                    contentItem: Item {
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 28
                            spacing: 8
                            PlainText {
                                text: window.activeCategoryName
                                color: window.textColor
                                font.pixelSize: window.chromeFont
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                            }
                            CountBadge {
                                objectName: "otherCategoryRequests"
                                count: window.categoryAttentionElsewhere()
                                elsewhere: true
                                Layout.alignment: Qt.AlignVCenter
                                HoverHandler { id: elsewhereHover }
                                ToolTip.visible: elsewhereHover.hovered
                                ToolTip.text: qsTr("%n agent(s) waiting in other categories", "", count)
                            }
                        }
                    }
                    indicator: PlainText {
                        x: categorySelector.width - width - 8
                        y: (categorySelector.height - height) / 2
                        text: "▾"
                        color: window.mutedTextColor
                        font.pixelSize: window.chromeFont
                    }
                    delegate: ItemDelegate {
                        id: categoryOption
                        required property var modelData
                        required property int index
                        width: categorySelector.width
                        text: modelData.name
                        font.pixelSize: window.chromeFont
                        hoverEnabled: true
                        onClicked: {
                            workspace.selectCategory(modelData.id)
                            categorySelector.popup.close()
                        }
                        contentItem: RowLayout {
                            spacing: 8
                            PlainText {
                                text: window.categoryRecall(categoryOption.index)
                                visible: text.length > 0
                                color: window.mutedTextColor
                                font.family: window.monoFamily
                                font.pixelSize: window.readoutFont
                                leftPadding: 4
                            }
                            PlainText {
                                text: categoryOption.text
                                color: window.textColor
                                font.weight: categoryOption.modelData.id === workspace.activeCategoryId ?
                                                 Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                            }
                            CountBadge {
                                count: categoryOption.modelData.unseenCount
                            }
                        }
                        background: Rectangle {
                            color: categoryOption.hovered ? window.hoveredCardColor : window.cardColor
                            Rectangle {
                                width: 2
                                height: parent.height
                                color: categoryOption.modelData.id === workspace.activeCategoryId ?
                                           window.focusedBorderColor : "transparent"
                            }
                        }
                    }

                    popup.background: Rectangle {
                        color: window.cardColor
                        border.color: window.borderColor
                        border.width: 1
                    }
                    popup.onOpened: {
                        const limit = Math.max(120, window.height - 80)
                        if (categorySelector.popup.height > limit)
                            categorySelector.popup.height = limit
                    }
                }
                CommandsButton {
                    objectName: categoryBar.visible ? "commandsButton" : "barCommands"
                }
            }

            Rectangle {
                objectName: "workspaceErrorBanner"
                visible: workspace.workspaceError.length > 0 && !window.inputBlocked
                Layout.fillWidth: true
                Layout.preferredHeight: window.chromeFont * 3 + 24
                Layout.maximumHeight: window.chromeFont * 5 + 24
                color: window.cardColor
                border.color: window.faultColor
                border.width: 1
                radius: window.chromeRadius
                clip: true

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 8
                    PlainLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: workspace.workspaceError
                        color: window.textColor
                        wrapMode: Text.WordWrap
                    }
                    CommandButton {
                        objectName: "dismissWorkspaceError"
                        text: qsTr("Dismiss")
                        onClicked: workspace.clearError()
                    }
                }
            }

            Rectangle {
                id: stage
                objectName: "focusedPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 120
                color: window.paneColor
                // The stage edge carries the focus accent only while it owns input.
                border.color: liveTerminal.interactive ? window.focusedBorderColor : window.borderColor
                border.width: 1
                radius: window.chromeRadius
                clip: true

                // One agent, the category's selected one, fills the stage.
                readonly property int inset: 6

                // The visual view covers the selected agent's terminal while
                // that agent shows as a thread.
                Transcript {
                    id: visualView
                    objectName: "visualView"
                    z: 3
                    visible: window.visualCovers
                    x: liveTerminal.x
                    y: liveTerminal.y
                    width: liveTerminal.width
                    height: liveTerminal.height
                    model: workspace.transcript
                    themeName: window.visualSettings.theme || "terminal"
                    fontName: window.visualSettings.font || "system"
                    codeFontName: window.visualSettings.codeFont || "system"
                    sizeSetting: window.visualSettings.size || 0
                    terminalFontSize: window.terminalFontSize
                    terminalFamily: window.monoFamily
                    terminalBackground: liveTerminal.backgroundColor
                    terminalAccent: window.focusedBorderColor
                    // The bar types into the terminal underneath, which keeps
                    // the session, history and Tab's guesses it always had.
                    submit: text => liveTerminal.submitText(text)
                    interrupt: () => liveTerminal.interrupt()
                    tabAway: () => liveTerminal.tabFlow && liveTerminal.tabAway()
                    suggestion: liveTerminal.suggestion
                    suggestionConfident: liveTerminal.suggestionConfident
                    working: liveTerminal.document !== null && liveTerminal.document.statusKind === "working"
                    waiting: liveTerminal.document !== null && liveTerminal.document.attentionPending
                    waitingReason: liveTerminal.document !== null ? liveTerminal.document.attentionReason : ""
                    canSend: liveTerminal.document !== null && liveTerminal.document.reachable
                    editor: window.visualSettings.editor || "auto"
                    screen: () => liveTerminal.screenLines()
                    folder: window.visualActive && liveTerminal.document
                            ? workspace.agentFolder(liveTerminal.document.sessionId) : ""
                    slashCommands: liveTerminal.document !== null && liveTerminal.document.harnessId === "claude"
                    onSuggestionFilled: {
                        if (typeof nextPrompt !== "undefined" && nextPrompt !== null && liveTerminal.document)
                            nextPrompt.used(liveTerminal.document.sessionId, false, 0, liveTerminal.suggestionKey)
                    }
                    onSuggestionSent: {
                        if (typeof nextPrompt !== "undefined" && nextPrompt !== null && liveTerminal.document)
                            nextPrompt.used(liveTerminal.document.sessionId, true, 0, liveTerminal.suggestionKey)
                    }
                    // The same picker for every CLI; each CLI's own command
                    // makes the change, typed into its session.
                    models: window.visualActive ? window.visualModels(liveTerminal.document) : []
                    efforts: window.visualActive ? window.visualEfforts(liveTerminal.document) : []
                    onModelChosen: function(id) { window.visualCommand("model", id) }
                    onEffortChosen: function(level) { window.visualCommand("effort", level) }
                    onCommandSent: window.watchVisualDialog(false)
                    onDialogFound: window.watchVisualDialog(true)
                    onDismissed: window.toggleVisual()
                    onEditRequested: function(record, text) { window.forkVisual(record, true, text) }
                }
                Binding {
                    target: workspace.transcript
                    property: "path"
                    // Nothing is read or watched while no agent shows as a thread.
                    value: window.visualActive ? window.visualFile : ""
                }

                // Why the terminal is showing in place of the thread.
                Rectangle {
                    objectName: "visualDialogNote"
                    z: 4
                    visible: window.visualDialog && window.visualActive
                    x: liveTerminal.x + liveTerminal.width - width - 10
                    y: liveTerminal.y + 8
                    width: visualDialogText.implicitWidth + 20
                    height: visualDialogText.implicitHeight + 10
                    radius: 6
                    color: window.cardColor
                    border.color: window.borderColor
                    Text {
                        id: visualDialogText
                        anchors.centerIn: parent
                        text: qsTr("The agent's own dialog · the thread returns when it closes")
                        color: window.mutedTextColor
                        font.pixelSize: 12
                    }
                }

                TerminalSurface {
                    id: liveTerminal
                    ToolTip.visible: hoveredLink.length > 0
                    ToolTip.text: hoveredLink
                    ToolTip.delay: 250
                    objectName: "liveTerminal"
                    // A focused, editable text field to macOS accessibility, so
                    // dictation apps such as Wispr Flow paste into it at once;
                    // seen as the bare window, Wispr Flow falls back to a paste
                    // that can wait seconds. Not multiLine: on a multi-line
                    // editable item Qt keeps Command-` from the window.
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Terminal")
                    Accessible.editable: interactive
                    Accessible.focusable: true
                    // The stage shows one agent: its terminal fills the stage
                    // inside a fixed inset, and the visual view follows it.
                    x: stage.inset
                    y: stage.inset
                    width: Math.max(0, stage.width - 2 * stage.inset)
                    height: Math.max(0, stage.height - 2 * stage.inset)
                    document: workspace.focusedSession
                    fontFamily: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontFamily : ""
                    fontPixelSize: window.terminalFontSize
                    scrollToBottomOnTyping: (typeof keymap !== "undefined" && keymap !== null) ? keymap.scrollToBottomOnTyping : true
                    visible: document !== null
                    enabled: visible
                    // A history page stays interactive for the wheel, selection and
                    // typing back to live; input reaches the agent only when live.
                    interactive: visible && window.visible && !window.inputBlocked && document !== null
                                 && !window.visualCovers
                                 && (preview.active || document.inputReady || document.historyActive)
                    focus: visible && window.visible && !window.inputBlocked && !window.sideTerminalOpen
                           && !window.visualCovers
                    // The next prompt lapis predicted for the agent, while it
                    // waits for a prompt (never over a request): Tab types it
                    // for you to edit, Tab again sends it, and Tab with nothing
                    // offered moves to the next agent that needs you.
                    readonly property bool predicting: typeof nextPrompt !== "undefined" && nextPrompt !== null
                                                       && nextPrompt.enabled && document !== null
                    tabFlow: predicting && nextPrompt.guesses(document.harnessId)
                    suggestion: predicting && nextPrompt.revision >= 0 && !document.historyActive
                                && ["finished", "idle"].indexOf(document.statusKind) >= 0
                                ? nextPrompt.suggestion(document.sessionId) : ""
                    suggestionConfident: predicting && nextPrompt.revision >= 0 && document !== null
                                         && nextPrompt.confidence(document.sessionId) >=
                                         nextPrompt.confidentThreshold
                    onSuggestionSeen: (sessionId, offerKey) => {
                        if (typeof nextPrompt !== "undefined" && nextPrompt !== null)
                            nextPrompt.seenOffer({session: sessionId, offer: offerKey})
                    }
                    onSuggestionUsed: (sessionId, offerKey, sent, typedFirst) => {
                        if (typeof nextPrompt !== "undefined" && nextPrompt !== null)
                            nextPrompt.used(sessionId, sent, typedFirst, offerKey)
                    }
                    // Follows every offer, even one with the same words as the last.
                    suggestionKey: suggestion.length > 0 && nextPrompt.revision >= 0
                                   ? nextPrompt.offerKey(document.sessionId) : ""
                    tabAway: function() {
                        return predicting && workspace.nextPriorityAttention(nextPrompt.readyAgents())
                    }
                    // New and restarted agents start at this grid, not resized
                    // just after they drew.
                    onGridSizeChanged: workspace.setLaunchSize(gridSize)
                    Component.onCompleted: {
                        workspace.setLaunchSize(gridSize)
                        if (focus)
                            forceActiveFocus()
                    }

                    // While history shows, a wide translucent bar down the right
                    // edge. The whole strip answers the pointer: a press on the
                    // thumb drags it from where it was held, one elsewhere jumps
                    // there, and letting go at the bottom is live again. It sits
                    // inside the terminal, so the wheel over it still scrolls.
                    Item {
                        id: historyScrubber
                        objectName: "historyScrubber"
                        readonly property var session: liveTerminal.document
                        readonly property real inset: 4
                        readonly property real trackHeight: Math.max(1, height - 2 * inset)
                        readonly property real thumbHeight:
                            Math.min(trackHeight, Math.max(40, trackHeight * (session ? session.historySpan : 1)))
                        readonly property real travel: Math.max(1, trackHeight - thumbHeight)
                        readonly property bool engaged: scrubArea.containsMouse || scrubArea.pressed
                        property real grab: 0
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.right: parent.right
                        width: 36
                        z: 5
                        visible: session !== null && session.live && session.historyScrubbable
                                 && (session.historyActive || session.historyRequestPending)
                        // The thumb's top in the track while dragging, from the pointer.
                        function dragged(mouseY) {
                            return Math.max(0, Math.min(mouseY - inset - grab, travel))
                        }
                        Rectangle {
                            objectName: "historyTrack"
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            y: historyScrubber.inset
                            height: historyScrubber.trackHeight
                            width: historyScrubber.engaged ? 16 : 12
                            radius: width / 2
                            color: Qt.alpha(window.textColor, historyScrubber.engaged ? 0.14 : 0.07)
                            Behavior on width {
                                enabled: window.motionEnabled
                                NumberAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                            }
                            Behavior on color {
                                enabled: window.motionEnabled
                                ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                            }
                            Rectangle {
                                id: historyThumb
                                objectName: "historyThumb"
                                width: parent.width
                                radius: width / 2
                                height: historyScrubber.thumbHeight
                                y: scrubArea.pressed ? historyScrubber.dragged(scrubArea.mouseY)
                                                     : (historyScrubber.session ? historyScrubber.session.historyPosition : 1)
                                                       * historyScrubber.travel
                                color: Qt.alpha(window.focusedBorderColor,
                                                scrubArea.pressed ? 0.9 : historyScrubber.engaged ? 0.7 : 0.45)
                                Behavior on color {
                                    enabled: window.motionEnabled
                                    ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                                }
                            }
                        }
                        MouseArea {
                            id: scrubArea
                            anchors.fill: parent
                            hoverEnabled: true
                            preventStealing: true
                            cursorShape: Qt.ArrowCursor
                            function jump(mouseY) {
                                const session = historyScrubber.session
                                if (session)
                                    session.historyAt(historyScrubber.dragged(mouseY) / historyScrubber.travel)
                            }
                            onPressed: function(mouse) {
                                const top = historyScrubber.inset + historyThumb.y
                                const onThumb = mouse.y >= top && mouse.y <= top + historyThumb.height
                                historyScrubber.grab = onThumb ? mouse.y - top : historyThumb.height / 2
                                jump(mouse.y)
                            }
                            onPositionChanged: function(mouse) { if (pressed) jump(mouse.y) }
                            onReleased: function(mouse) {
                                // The thumb at the bottom is the live screen.
                                if (historyScrubber.dragged(mouse.y) >= historyScrubber.travel - 1
                                        && historyScrubber.session) {
                                    historyScrubber.session.returnToLive()
                                    preview.deferTerminalFocus()
                                }
                            }
                        }
                    }
                }

                // While history shows, the way back to live floats over the
                // terminal's corner rather than above it, so the terminal keeps
                // its size; scrolling down to the newest row, or typing, also
                // returns.
                Rectangle {
                    id: historyBanner
                    objectName: "historyBar"
                    readonly property var session: workspace.focusedSession
                    readonly property string note: !session ? ""
                                                   : session.historyRequestPending ? qsTr("Loading history…")
                                                   : session.historyMessage
                    visible: liveTerminal.visible && session !== null && session.live
                             && (session.historyActive || session.historyRequestPending)
                    z: 6
                    x: liveTerminal.x + liveTerminal.width - width - historyScrubber.width - 6
                    y: liveTerminal.y + liveTerminal.height - height - 8
                    width: historyRow.implicitWidth + 12
                    height: historyRow.implicitHeight + 8
                    color: window.surfaceColor
                    border.color: window.borderColor
                    border.width: 1
                    radius: window.chromeRadius
                    Row {
                        id: historyRow
                        anchors.centerIn: parent
                        spacing: 8
                        PlainLabel {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: text.length > 0
                            text: historyBanner.note
                            color: window.mutedTextColor
                        }
                        CommandButton {
                            objectName: "historyLive"
                            text: qsTr("Live")
                            enabled: !!historyBanner.session && historyBanner.session.historyActive
                            onClicked: {
                                if (historyBanner.session)
                                    historyBanner.session.returnToLive()
                                preview.deferTerminalFocus()
                            }
                        }
                    }
                }

                // Tab's guesses failing in a row (a usage limit, sign-in, the
                // network) show in the terminal's corner until one succeeds,
                // rather than Tab quietly offering nothing.
                Rectangle {
                    id: tabFailureNote
                    objectName: "tabFailureNote"
                    readonly property int streak: liveTerminal.predicting ? nextPrompt.failureStreak : 0
                    visible: streak >= 3 && liveTerminal.visible
                    z: 6
                    x: liveTerminal.x + 8
                    y: liveTerminal.y + liveTerminal.height - height - 8
                    width: Math.min(liveTerminal.width / 2, tabFailureLabel.implicitWidth + 16)
                    height: tabFailureLabel.height + 10
                    color: window.surfaceColor
                    border.color: window.faultColor
                    border.width: 1
                    radius: window.chromeRadius
                    PlainLabel {
                        id: tabFailureLabel
                        anchors.centerIn: parent
                        width: parent.width - 16
                        text: tabFailureNote.visible
                              ? qsTr("Tab: the last %1 guesses failed (%2)")
                                .arg(tabFailureNote.streak).arg(nextPrompt.lastFailure)
                              : ""
                        color: window.mutedTextColor
                        elide: Text.ElideRight
                    }
                }

                // A paste lapis could not send says why, over the bottom of the
                // terminal, for a few seconds.
                Rectangle {
                    id: pasteNote
                    objectName: "pasteNote"
                    property string reason: ""
                    visible: reason.length > 0 && liveTerminal.visible
                    z: 6
                    width: Math.min(liveTerminal.width - 16, pasteNoteLabel.implicitWidth + 16)
                    height: pasteNoteLabel.height + 12
                    x: liveTerminal.x + (liveTerminal.width - width) / 2
                    y: liveTerminal.y + liveTerminal.height - height - 8
                    color: window.surfaceColor
                    border.color: window.faultColor
                    border.width: 1
                    radius: window.chromeRadius
                    PlainLabel {
                        id: pasteNoteLabel
                        anchors.centerIn: parent
                        width: parent.width - 16
                        text: pasteNote.reason
                        color: window.textColor
                        wrapMode: Text.WordWrap
                    }
                    Timer {
                        id: pasteNoteTimer
                        interval: 8000
                        onTriggered: pasteNote.reason = ""
                    }
                    Connections {
                        target: liveTerminal
                        function onPasteRefused(reason) {
                            pasteNote.reason = reason
                            pasteNoteTimer.restart()
                        }
                        function onDocumentChanged() {
                            pasteNote.reason = ""
                        }
                    }
                }

                // An ended or unreachable agent keeps its last screen; say so on the
                // stage instead of leaving a cursor that looks live.
                Rectangle {
                    id: endedBar
                    objectName: "sessionEndedBar"
                    readonly property var session: workspace.focusedSession
                    readonly property string connection: session && session.live ? session.connectionState : ""
                    visible: connection === "ended" || connection === "disconnected"
                    anchors.left: liveTerminal.left
                    anchors.right: liveTerminal.right
                    anchors.bottom: liveTerminal.bottom
                    anchors.margins: 2
                    height: endedText.implicitHeight + 14
                    radius: window.chromeRadius
                    color: window.surfaceColor
                    border.width: 1
                    border.color: connection === "ended" ? window.borderColor : window.faultColor
                    PlainText {
                        id: endedText
                        objectName: "sessionEndedText"
                        anchors.fill: parent
                        anchors.margins: 7
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        font.family: window.monoFamily
                        font.pixelSize: window.readoutFont
                        color: endedBar.connection === "ended" ? window.mutedTextColor : window.faultColor
                        text: {
                            if (!endedBar.visible) return ""
                            const reason = endedBar.session.activity
                            const close = window.shortcutText("closeAgent")
                            const head = endedBar.connection === "ended" ? qsTr("Agent ended") : qsTr("Agent unreachable")
                            return head + (reason.length > 0 ? " · " + reason : "")
                                + " · " + qsTr("Restart agent in Commands resumes it")
                                + (close.length > 0 ? " · " + qsTr("%1 closes it").arg(close) : "")
                        }
                    }
                }

                // Nothing on the stage: what to do next, recent conversations to
                // resume and the categories that have agents, all by keyboard
                // (up, down, Return) as well as by click.
                ColumnLayout {
                    id: homePanel
                    objectName: "emptyState"
                    visible: workspace.focusedSession === null
                    anchors.centerIn: parent
                    width: Math.min(560, parent.width - 32)
                    spacing: 10
                    PlainLabel {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: window.mutedTextColor
                        text: qsTr("Nothing is open in %1.").arg(window.activeCategoryName)
                    }
                    ListView {
                        id: homeList
                        objectName: "homeList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, Math.max(120, stage.height - 120))
                        model: homeModel
                        clip: true
                        interactive: contentHeight > height
                        boundsBehavior: Flickable.StopAtBounds
                        keyNavigationEnabled: true
                        highlightMoveDuration: 0
                        currentIndex: 0
                        activeFocusOnTab: true
                        section.property: "group"
                        section.delegate: PlainLabel {
                            required property string section
                            visible: section.length > 0
                            height: section.length > 0 ? 30 : 0
                            width: homeList.width
                            verticalAlignment: Text.AlignBottom
                            bottomPadding: 4
                            leftPadding: 10
                            text: section
                            color: window.mutedTextColor
                            font.pixelSize: window.readoutFont
                            font.letterSpacing: window.appearance && window.appearance.headingTracking !== undefined ? window.appearance.headingTracking : 1
                            font.capitalization: Font.AllUppercase
                        }
                        Keys.onReturnPressed: window.runHomeEntry(currentIndex)
                        Keys.onEnterPressed: window.runHomeEntry(currentIndex)
                        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                        delegate: ItemDelegate {
                            id: homeRow
                            required property int index
                            required property string kind
                            required property string label
                            required property string detail
                            required property string hint
                            required property string harness
                            readonly property bool current: index === homeList.currentIndex
                            objectName: "home_" + kind + "_" + index
                            width: homeList.width
                            height: 38
                            focusPolicy: Qt.NoFocus
                            hoverEnabled: true
                            enabled: window.interactionArmed
                            Accessible.name: label + (detail.length > 0 ? ", " + detail : "")
                            onClicked: { homeList.currentIndex = index; window.runHomeEntry(index) }
                            background: Rectangle {
                                radius: window.chromeRadius
                                color: homeRow.current && homeList.activeFocus ? window.focusedColor :
                                       homeRow.hovered ? window.hoveredCardColor : "transparent"
                                Rectangle {
                                    visible: homeRow.current && homeList.activeFocus
                                    width: 2
                                    height: parent.height
                                    color: window.focusedBorderColor
                                }
                            }
                            contentItem: RowLayout {
                                spacing: 10
                                AgentMark {
                                    visible: homeRow.harness.length > 0
                                    harnessId: homeRow.harness
                                    ink: window.textColor
                                    Layout.preferredWidth: 16
                                    Layout.preferredHeight: 16
                                }
                                PlainText {
                                    text: homeRow.label
                                    color: window.textColor
                                    font.pixelSize: window.chromeFont
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                }
                                PlainText {
                                    visible: text.length > 0
                                    text: homeRow.detail
                                    color: window.mutedTextColor
                                    font.family: window.monoFamily
                                    font.pixelSize: window.readoutFont
                                    elide: Text.ElideMiddle
                                    Layout.maximumWidth: homeList.width * 0.4
                                }
                                PlainText {
                                    visible: text.length > 0
                                    text: homeRow.hint
                                    color: window.mutedTextColor
                                    font.family: window.monoFamily
                                    font.pixelSize: window.readoutFont
                                }
                            }
                        }
                    }
                }

                // Files dragged in from Finder: their paths, quoted, are pasted into
                // the selected agent's terminal.
                DropArea {
                    id: fileDrop
                    objectName: "fileDrop"
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    enabled: workspace.focusedSession !== null && window.interactionArmed
                    onDropped: function(drop) {
                        if (!drop.hasUrls)
                            return
                        const text = window.quoteDroppedPaths(drop.urls)
                        if (text.length === 0)
                            return
                        const destinationId = workspace.focusedSession !== null ?
                                                  workspace.focusedSession.sessionId : ""
                        if (destinationId.length === 0)
                            return
                        Qt.callLater(() => window.pastePaths(text, destinationId))
                        drop.acceptProposedAction()
                    }
                }
                Rectangle {
                    objectName: "fileDropHint"
                    visible: fileDrop.containsDrag
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: window.chromeRadius
                    color: Qt.alpha(window.focusedBorderColor, 0.08)
                    border.width: 2
                    border.color: window.focusedBorderColor
                }
            }

            ListView {
                id: agentTabs
                objectName: "agentTabs"
                visible: window.stripShown
                Layout.fillWidth: true
                Layout.preferredHeight: window.stripHeight
                Layout.minimumHeight: window.stripHeight
                Layout.maximumHeight: window.stripHeight
                orientation: ListView.Horizontal
                spacing: 8
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // Pressing and moving a card drags it; the wheel and trackpad scroll.
                interactive: false
                // Hidden cards release their surfaces; off-screen ones are not
                // kept, so only what is visible renders.
                model: visible ? workspace.categorySessions : []
                cacheBuffer: 0
                highlight: null
                highlightFollowsCurrentItem: false
                currentIndex: window.focusedTabIndex()
                readonly property real cardWidth: Math.round(height * 1.9)
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                WheelHandler {
                    onWheel: function(event) {
                        const pixels = Math.abs(event.pixelDelta.x) > Math.abs(event.pixelDelta.y) ?
                                           event.pixelDelta.x : event.pixelDelta.y
                        const angle = Math.abs(event.angleDelta.x) > Math.abs(event.angleDelta.y) ?
                                          event.angleDelta.x : event.angleDelta.y
                        const delta = pixels !== 0 ? pixels : angle / 2
                        const limit = Math.max(0, agentTabs.contentWidth - agentTabs.width)
                        stripScroll.stop()
                        agentTabs.contentX = Math.max(0, Math.min(agentTabs.contentX - delta, limit))
                    }
                }
                // Agents dragged along the strip land between the cards the
                // marker shows.
                DropArea {
                    id: stripDrop
                    objectName: "stripDrop"
                    parent: agentTabs
                    anchors.fill: parent
                    keys: ["lapis-agents"]
                    property int index: -1
                    function update(px) {
                        const step = agentTabs.cardWidth + agentTabs.spacing
                        index = Math.max(0, Math.min(workspace.categorySessions.length,
                                                     Math.round((px + agentTabs.contentX) / step)))
                    }
                    onEntered: function(drag) { update(drag.x) }
                    onPositionChanged: function(drag) { update(drag.x) }
                    onExited: index = -1
                    onDropped: function(drop) {
                        const at = index
                        index = -1
                        if (at >= 0)
                            window.queueDrop(() => window.dropOnStrip(at))
                        drop.accept()
                    }
                }
                Rectangle {
                    objectName: "stripDropMarker"
                    parent: agentTabs
                    visible: stripDrop.containsDrag && stripDrop.index >= 0
                    x: stripDrop.index * (agentTabs.cardWidth + agentTabs.spacing) - agentTabs.contentX
                       - agentTabs.spacing / 2 - 1
                    y: 4
                    width: 3
                    height: agentTabs.height - 8
                    radius: 1
                    color: window.focusedBorderColor
                }

                // Like Neovim's sidescrolloff: selection may approach either edge,
                // but part of the neighboring card stays in view so the next one
                // is always recognizable. The new-agent card counts as a neighbor.
                function revealFocused() {
                    const index = window.focusedTabIndex()
                    if (index < 0 || !visible || width <= 0)
                        return
                    const step = cardWidth + spacing
                    const peek = Math.round(cardWidth * 0.4)
                    const left = index * step - (index > 0 ? spacing + peek : 0)
                    const right = index * step + cardWidth + spacing + peek
                    let target = contentX
                    if (left < contentX)
                        target = left
                    else if (right > contentX + width)
                        target = right - width
                    target = Math.max(0, Math.min(target, contentWidth - width))
                    if (Math.abs(target - contentX) < 0.5)
                        return
                    stripScroll.stop()
                    if (window.motionEnabled) {
                        stripScroll.from = contentX
                        stripScroll.to = target
                        stripScroll.start()
                    } else {
                        contentX = target
                    }
                }
                onCountChanged: Qt.callLater(revealFocused)
                onWidthChanged: Qt.callLater(revealFocused)
                onCurrentIndexChanged: Qt.callLater(revealFocused)
                // Card geometry follows the strip height, so a font-size or
                // density change re-lays the cards out without touching count,
                // width, or selection. Without this the reveal keeps a stale
                // scroll offset and the focused card can sit past the edge.
                onHeightChanged: Qt.callLater(revealFocused)
                NumberAnimation {
                    id: stripScroll
                    target: agentTabs
                    property: "contentX"
                    duration: window.motionDuration * 2
                    easing.type: Easing.OutCubic
                }

                delegate: Item {
                    id: agentTab
                    required property var modelData
                    required property int index
                    readonly property bool marked: workspace.focusedSession !== null
                                                   && workspace.focusedSession.sessionId === modelData.sessionId
                    readonly property bool picked: window.isAgentSelected(modelData.sessionId)
                    objectName: "agentTab_" + modelData.sessionId
                    opacity: window.dragSource === "strip" && window.draggedAgents.indexOf(modelData.sessionId) >= 0 ?
                                 0.45 : 1
                    width: agentTabs.cardWidth
                    height: agentTabs.height
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.agentName + ", " + window.agentTabTitle(modelData) + ", " + modelData.statusLabel
                    // Assistive activation selects the agent, as a click does.
                    Accessible.onPressAction: {
                        if (window.interactionArmed && workspace.selectSession(agentTab.modelData.sessionId))
                            preview.deferTerminalFocus()
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: agentTab.marked ? window.focusedColor : window.paneColor
                        radius: window.chromeRadius
                        border.width: agentTab.marked || agentTab.picked ? 2 : 1
                        border.color: agentTab.marked || agentTab.picked ? window.focusedBorderColor :
                                      agentHover.hovered ? window.mutedTextColor : window.borderColor
                        Behavior on border.color {
                            enabled: window.motionEnabled
                            ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                        }
                    }
                    // Finished or needs you, and not looked at yet: a slow, quiet
                    // pulse of the card edge in ink until the agent is selected.
                    Rectangle {
                        id: unseenCue
                        objectName: "unseenCue_" + agentTab.modelData.sessionId
                        anchors.fill: parent
                        visible: agentTab.modelData.unseen && !agentTab.marked
                        color: "transparent"
                        radius: window.chromeRadius
                        border.width: 2
                        border.color: window.textColor
                        opacity: 0.85
                        SequentialAnimation on opacity {
                            running: unseenCue.visible && window.motionEnabled
                            loops: Animation.Infinite
                            alwaysRunToEnd: false
                            NumberAnimation { from: 0.85; to: 0.2; duration: 900; easing.type: Easing.InOutSine }
                            NumberAnimation { from: 0.2; to: 0.85; duration: 900; easing.type: Easing.InOutSine }
                        }
                    }
                    RowLayout {
                        id: agentHeader
                        x: 8
                        y: 5
                        width: parent.width - 16
                        height: Math.max(16, window.readoutFont + 4)
                        spacing: 6
                        AgentMark {
                            harnessId: agentTab.modelData.harnessId
                            visible: !workspace.previewMode
                            ink: agentTab.marked ? window.textColor : window.mutedTextColor
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                        }
                        StatusMark {
                            objectName: "statusMark_" + agentTab.modelData.sessionId
                            kind: agentTab.modelData.statusKind
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            Layout.alignment: Qt.AlignVCenter
                        }
                        PlainText {
                            text: window.agentTabTitle(agentTab.modelData)
                            color: agentTab.marked ? window.textColor : window.mutedTextColor
                            font.family: window.monoFamily
                            font.pixelSize: window.readoutFont
                            font.weight: agentTab.marked ? Font.DemiBold : Font.Normal
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            verticalAlignment: Text.AlignVCenter
                        }
                        PlainText {
                            objectName: "statusLabel_" + agentTab.modelData.sessionId
                            // On a narrow card the status dot carries the state and
                            // the project path keeps the room.
                            visible: agentTab.marked && agentTab.width >= 240
                            text: agentTab.modelData.statusLabel
                            color: window.statusColor(agentTab.modelData.statusKind)
                            font.family: window.monoFamily
                            font.pixelSize: window.readoutFont
                            Layout.maximumWidth: agentTab.width * 0.45
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    // A scaled copy of the agent's latest lines: never interactive
                    // and never resizes the agent's terminal.
                    TerminalSurface {
                        objectName: "previewTerminal_" + agentTab.modelData.sessionId
                        x: 6
                        y: agentHeader.y + agentHeader.height + 4
                        width: parent.width - 12
                        height: parent.height - y - 6
                        document: agentTab.modelData
                        fontFamily: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontFamily : ""
                        fontPixelSize: window.terminalFontSize
                        interactive: false
                        enabled: false
                        frameInterval: 250
                        minimumScale: 0.5
                    }
                    // Picked for a drag with Command-click or Shift-click.
                    Rectangle {
                        objectName: "pickedCue_" + agentTab.modelData.sessionId
                        anchors.fill: parent
                        visible: agentTab.picked && !agentTab.marked
                        radius: window.chromeRadius
                        color: Qt.alpha(window.focusedBorderColor, 0.08)
                    }
                    HoverHandler { id: agentHover }
                    DragOrClick {
                        objectName: "cardPress_" + agentTab.modelData.sessionId
                        anchors.fill: parent
                        enabled: window.interactionArmed
                        onTapped: function(modifiers) { window.clickAgent(agentTab.modelData.sessionId, modifiers) }
                        onDragStarted: function(scene) {
                            window.beginAgentDrag(window.agentsToDrag(agentTab.modelData.sessionId), "strip", scene)
                        }
                        onDragMoved: function(scene) { window.moveDragGhost(scene) }
                        onDragEnded: window.endDrag()
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        enabled: window.interactionArmed
                        onTapped: {
                            if (workspace.selectSession(agentTab.modelData.sessionId))
                                window.popupAt(agentMenu, agentTab)
                        }
                    }
                }

                footer: Item {
                    width: Math.round(agentTabs.height * 0.8) + agentTabs.spacing
                    height: agentTabs.height
                    CommandButton {
                        id: newAgentButton
                        objectName: "newAgentButton"
                        x: agentTabs.spacing
                        width: parent.width - agentTabs.spacing
                        height: parent.height
                        text: "+"
                        hint: window.shortcutText("newAgent")
                        hintWhenRoom: true
                        Accessible.name: qsTr("New agent")
                        enabled: window.interactionArmed
                        onClicked: window.openNewAgentDialog()
                    }
                }
            }
        }
    }
    // The side terminal: over the stage's right half, above the agents.
    Rectangle {
        id: sidePanel
        property string pasteReason: ""
        Timer { id: sidePasteTimer; interval: 8000; onTriggered: sidePanel.pasteReason = "" }
        objectName: "sideTerminal"
        parent: stage
        z: 60
        visible: window.sideTerminalOpen
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: Math.round(Math.max(Math.min(360, parent.width), Math.min(parent.width * 0.5, 960)))
        color: window.surfaceColor
        border.width: 1
        border.color: sideSurface.activeFocus ? window.focusedBorderColor : window.borderColor
        radius: window.chromeRadius
        // Only the shell: its own prompt says where it is, and the keys that
        // opened it close it.
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 6
            spacing: 0
            TerminalSurface {
                id: sideSurface
                ToolTip.visible: hoveredLink.length > 0
                ToolTip.text: hoveredLink
                ToolTip.delay: 250
                objectName: "sideTerminalSurface"
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Side terminal")
                Accessible.editable: interactive
                Accessible.focusable: true
                onPasteRefused: (reason) => {
                    sidePanel.pasteReason = reason
                    sidePasteTimer.restart()
                }
                onDocumentChanged: sidePanel.pasteReason = ""
                Layout.fillWidth: true
                Layout.fillHeight: true
                document: window.terminalsAvailable ? terminals.current : null
                fontFamily: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontFamily : ""
                fontPixelSize: window.terminalFontSize
                visible: sidePanel.visible && document !== null
                enabled: visible
                // A history page stays interactive for selection and typing
                // back to live, even after the side panel is hidden and shown.
                interactive: visible && window.visible && !window.inputBlocked && document !== null &&
                             (document.inputReady || document.historyActive)
                focus: visible && window.visible && !window.inputBlocked
            }
            PlainLabel {
                visible: !sideSurface.visible
                Layout.fillWidth: true
                Layout.fillHeight: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                color: window.mutedTextColor
                text: window.terminalsAvailable && terminals.error.length > 0 ? terminals.error : qsTr("Starting a shell…")
            }
            PlainLabel {
                objectName: "sidePasteRefusal"
                visible: sidePanel.pasteReason.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: window.faultColor
                text: sidePanel.pasteReason
            }
        }
    }

    // Command-~: which machine the side terminal runs on. Up and down move,
    // Return opens that machine's terminal (starting a shell there when it
    // has none); a dot marks the machines with one running.
    Dialog {
        id: terminalPicker
        objectName: "terminalPicker"
        title: qsTr("Terminal on")
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(420, window.width - 32)
        padding: 12
        font.pixelSize: window.chromeFont
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property var machines: []
        property string pending: ""
        property bool chosen: false
        background: Rectangle {
            color: window.surfaceColor
            border.color: window.focusedBorderColor
            radius: window.chromeRadius
        }
        function choose(index) {
            const machine = machines[index]
            if (!machine)
                return
            pending = machine.id
            chosen = true
            close()
        }
        onOpened: {
            chosen = false
            machines = window.terminalsAvailable ? terminals.machines : []
            machineList.currentIndex = Math.max(0, machines.findIndex(m => m.id === window.lastTerminalMachine))
            machineList.forceActiveFocus()
        }
        onClosed: {
            if (chosen)
                Qt.callLater(function() { window.openTerminalOn(terminalPicker.pending) })
            else
                preview.deferTerminalFocus()
        }
        contentItem: ListView {
            id: machineList
            objectName: "terminalMachines"
            implicitHeight: Math.min(8, count) * 36
            clip: true
            model: terminalPicker.machines
            keyNavigationEnabled: true
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            Keys.onReturnPressed: terminalPicker.choose(currentIndex)
            Keys.onEnterPressed: terminalPicker.choose(currentIndex)
            onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ItemDelegate {
                id: machineRow
                required property var modelData
                required property int index
                objectName: "terminalMachine_" + (modelData.id.length > 0 ? modelData.id : "mac")
                width: machineList.width
                height: 36
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.name: modelData.name + (modelData.open ? qsTr(", running") : "")
                onClicked: terminalPicker.choose(index)
                background: Rectangle {
                    radius: window.chromeRadius
                    color: machineRow.index === machineList.currentIndex ? window.focusedColor :
                           machineRow.hovered ? window.hoveredCardColor : "transparent"
                    Rectangle {
                        visible: machineRow.index === machineList.currentIndex
                        width: 2
                        height: parent.height
                        color: window.focusedBorderColor
                    }
                }
                contentItem: RowLayout {
                    spacing: 10
                    PlainText {
                        text: machineRow.modelData.name
                        color: window.textColor
                        font.pixelSize: window.chromeFont
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        visible: machineRow.modelData.open
                        width: 6
                        height: 6
                        radius: 3
                        color: window.plentyColor
                    }
                }
            }
        }
    }
}
