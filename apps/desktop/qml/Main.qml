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
    readonly property color paneColor: backgroundColor
    readonly property int chromeRadius: appearance ? appearance.cornerRadius : 2
    readonly property int motionDuration: appearance ? appearance.motionDuration : 100
    // One fixed-width family for the terminal and every machine readout (paths,
    // shortcut hints, states, counts). Human names and prose use the UI face.
    readonly property string monoFamily: liveTerminal.resolvedFontFamily
    // Composition and paste own the keyboard until they finish.
    readonly property bool terminalBusy: liveTerminal.composing || liveTerminal.pasting

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
        if (action === "nextCategory")
            return [mod + "Alt+Right", mod + (mac ? "Shift+Down" : "Down")]
        if (action === "previousCategory")
            return [mod + "Alt+Left", mod + (mac ? "Shift+Up" : "Up")]
        if (action === "category1")
            return [mod + "1"]
        if (action === "category2")
            return [mod + "2"]
        if (action === "category3")
            return [mod + "3"]
        if (action === "category4")
            return [mod + "4"]
        if (action === "openSettings")
            return preview.settingsShortcuts
        if (action === "toggleSidebar")
            return [mod + "B"]
        if (action === "openCommands")
            return [mac ? "Meta+Shift+P" : "Ctrl+Shift+P"]
        if (action === "newAgent")
            return [mod + "N"]
        if (action === "reloadConfig")
            return [mod + "R"]
        if (action === "nextWindow")
            return [mac ? "Meta+Shift+]" : "Ctrl+Shift+]"]
        if (action === "previousWindow")
            return [mac ? "Meta+Shift+[" : "Ctrl+Shift+["]
        if (action === "newCategory")
            return [mac ? "Meta+Shift+N" : "Ctrl+Shift+Alt+N"]
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
    function agentBaseTitle(session) {
        return workspace.previewMode || session.title !== projectName(session.directory).slice(0, 80) ?
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
    readonly property var commandEntries: {
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
        add("toggleSidebar", sidebarExpanded ? qsTr("Hide sidebar") : qsTr("Show sidebar"), "toggleSidebar", true, "", () => window.toggleSidebar())
        add("togglePreviews", previewsEnabled ? qsTr("Hide agent previews") : qsTr("Show agent previews"), "togglePreviews", true, "", () => window.togglePreviews())
        add("nextAttention", qsTr("Go to agent that needs you"), "nextAttention", workspace.attentionAgents > 0, qsTr("No agent is waiting"), () => workspace.nextAttention())
        add("nextWindow", qsTr("Next agent in category"), "nextWindow", workspace.categorySessions.length > 1, qsTr("This category needs another agent"), () => workspace.nextSession())
        add("previousWindow", qsTr("Previous agent in category"), "previousWindow", workspace.categorySessions.length > 1, qsTr("This category needs another agent"), () => workspace.nextSession(-1))
        add("closeAgent", qsTr("Close agent"), "closeAgent", hasAgent, needAgent, () => window.closeFocusedAgent())
        const stopped = hasAgent && agent.live && (agent.connectionState === "ended" || agent.connectionState === "disconnected")
        add("restartAgent", qsTr("Restart agent"), "", stopped, qsTr("Only an ended or unreachable agent restarts"), () => workspace.restartAgent(agent.sessionId))
        add("nextCategory", qsTr("Next category"), "nextCategory", workspace.categories.length > 1, qsTr("Add another category first"), () => workspace.nextCategory())
        add("previousCategory", qsTr("Previous category"), "previousCategory", workspace.categories.length > 1, qsTr("Add another category first"), () => workspace.nextCategory(-1))
        for (let i = 0; i < Math.min(4, workspace.categories.length); ++i) {
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
        add("reconnect", qsTr("Reconnect agent"), "", window.recoveryAvailable(), qsTr("The selected agent is already connected or opening"), () => agent.reconnect())
        add("discover", qsTr("Discover existing agent session"), "", window.recoveryAvailable(), qsTr("Available for a disconnected agent"), () => agent.discoverSession())
        add("restart", qsTr("Start replacement agent session"), "", window.recoveryAvailable(), qsTr("Available for a disconnected agent"), () => agent.startNewSession())
        add("appearance", qsTr("Appearance"), "openSettings", true, "", () => window.openSettingsDialog())
        add("reloadConfig", qsTr("Reload configuration"), "reloadConfig", typeof keymap !== "undefined" && keymap !== null, qsTr("No configuration in this fixture"), () => keymap.reload())
        add("quit", qsTr("Quit lapis"), "quit", true, "", () => Qt.quit())
        return entries
    }
    function runCommand(id) {
        if (!interactionArmed) return
        const command = commandEntries.find(entry => entry.id === id)
        if (command && command.enabled) command.run()
    }

    // Every status class has its own shape so no state depends on color alone:
    // dot working, diamond pending request, ring ready, square lost connection,
    // dash ended, hollow box opening or unknown.
    function statusShape(kind) {
        if (kind === "working")
            return "dot"
        if (kind === "waiting")
            return "diamond"
        if (kind === "idle" || kind === "finished")
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
        if (kind === "waiting")
            return attentionColor
        if (kind === "disconnected")
            return faultColor
        if (kind === "idle" || kind === "finished")
            return textColor
        return mutedTextColor
    }
    // Mono recall number for categories reachable by a category1-4 shortcut.
    function categoryRecall(index) {
        return index < 4 && bindings("category" + (index + 1)).length > 0 ? String(index + 1) : ""
    }
    function categoryAttentionElsewhere() {
        let total = 0
        for (const category of workspace.categories) {
            if (category.id !== workspace.activeCategoryId)
                total += category.attentionCount
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
        return commandsDialog.visible || settingsDialog.visible || attentionDialog.visible || agentDialog.visible
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

    property string lastHarness: "codex"
    function openNewAgentDialog() {
        if (terminalBusy)
            return
        if (dialogsVisible())
            return
        agentDialog.localError = ""
        agentDialog.harnesses = workspace.availableHarnesses()
        agentDialog.phase = 0
        const preferred = agentDialog.harnesses.findIndex(h => h.id === window.lastHarness && h.installed)
        harnessChoices.currentIndex = preferred >= 0 ? preferred : Math.max(0, agentDialog.harnesses.findIndex(h => h.installed))
        agentDirectoryField.text = workspace.homeDirectory + "/"
        openFresh(agentDialog)
    }
    // Command-W: close the focused agent. A running agent is confirmed first,
    // the way iTerm2 asks before closing a session with a running job.
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
        closeAgentDialog.reachable = session.inputReady
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
        const title = projectName(folder).slice(0, 80)
        const folderIssue = directoryProblem(agentDirectoryField.text)
        if (folderIssue.length > 0) {
            agentDialog.localError = folderIssue
            return
        }
        if (!workspace.createAgent(agentDirectoryField.text.trim(), title, agentDialog.selectedHarness)) {
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
    Commands {
        id: commandsDialog
        commands: window.commandEntries
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
    // owns the keyboard. Closing the window does not hide it.
    Shortcut {
        sequences: window.bindings("quit")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: Qt.quit()
    }
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
        onActivated: window.closeFocusedAgent()
    }
    Shortcut {
        objectName: "nextAttentionShortcut"
        sequences: window.bindings("nextAttention")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: workspace.nextAttention()
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
    component RequestsButton: CommandButton {
        text: {
            const session = workspace.focusedSession
            return session && session.attentionCount > 0 ? qsTr("Requests · %1").arg(session.attentionCount) : ""
        }
        visible: text.length > 0
        enabled: visible && window.interactionArmed
        selected: true
        frameColor: window.attentionColor
        onClicked: window.openAttentionDialog()
    }
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
        fontSizeMinimum: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSizeMinimum : 16
        fontSizeMaximum: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSizeMaximum : 16
        fontSizeDefault: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSizeDefault : 16
        motionDuration: window.motionDuration
        motionEnabled: window.motionEnabled
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
        property string selectedHarness: "codex"
        readonly property string harnessName: {
            const item = harnesses.find(h => h.id === selectedHarness)
            return item ? item.name : selectedHarness
        }
        function chooseHarness(index) {
            const item = choices[index]
            if (!item || !item.installed) return
            selectedHarness = item.id
            window.lastHarness = item.id
            phase = 1
            Qt.callLater(function() {
                agentDirectoryField.forceActiveFocus()
                agentDirectoryField.cursorPosition = agentDirectoryField.text.length
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
        function refreshFolders() {
            if (!folderResults) return
            const entries = []
            if (directoryModel.status === FolderListModel.Ready) {
                for (let i = 0; i < Math.min(directoryModel.count, 4096) && entries.length < 100; ++i) {
                    const name = directoryModel.get(i, "fileName")
                    const path = directoryModel.get(i, "filePath")
                    if (path.slice(0, path.lastIndexOf("/") + 1) === folderParent
                            && name.toLowerCase().startsWith(folderPrefix.toLowerCase()))
                        entries.push({name: name, path: path + "/"})
                }
            }
            folderResults.model = entries
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
                if (!agentDialog.visible || agentDialog.phase !== 1) return
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
                                ink: harnessChoice.modelData.installed ? window.textColor : window.mutedTextColor
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                            }
                            PlainLabel {
                                text: harnessChoice.modelData.name
                                color: harnessChoice.modelData.installed ? window.textColor : window.mutedTextColor
                                Layout.fillWidth: true
                            }
                            PlainLabel {
                                visible: !harnessChoice.modelData.installed
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
                }
                RowLayout {
                    visible: agentDialog.phase === 1
                    Layout.fillWidth: true
                    FormField {
                        id: agentDirectoryField
                        objectName: "agentDirectoryField"
                        placeholderText: workspace.homeDirectory + "/"
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
                        text: "…"
                        Accessible.name: qsTr("Browse project folders")
                        onClicked: folderPicker.open()
                    }
                }
                ListView {
                    id: folderResults
                    objectName: "folderResults"
                    visible: agentDialog.phase === 1
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
        onClosed: categoryDialog.localError = ""

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
        MenuSeparator {
            visible: window.recoveryAvailable()
        }
        ActionItem {
            objectName: "reconnectAgentAction"
            text: qsTr("Reconnect")
            visible: window.recoveryAvailable()
            onTriggered: if (workspace.focusedSession)
                             workspace.focusedSession.reconnect()
        }
        ActionItem {
            objectName: "discoverAgentAction"
            text: qsTr("Discover existing session")
            visible: window.recoveryAvailable()
            onTriggered: if (workspace.focusedSession)
                             workspace.focusedSession.discoverSession()
        }
        ActionItem {
            objectName: "startSessionAction"
            text: qsTr("Start new session")
            visible: window.recoveryAvailable()
            onTriggered: if (workspace.focusedSession)
                             workspace.focusedSession.startNewSession()
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
                        Accessible.name: modelData.attentionCount > 0 ?
                                             qsTr("%1, %2 requests").arg(modelData.name).arg(modelData.attentionCount) :
                                             modelData.name
                        ToolTip.visible: hovered && recall.length > 0
                        ToolTip.delay: 600
                        ToolTip.text: window.shortcutText("category" + (index + 1))

                        background: Rectangle {
                            radius: window.chromeRadius
                            color: categoryButton.hovered && !categoryButton.marked ? window.hoveredCardColor :
                                                                                       window.surfaceColor
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
                                CountBadge {
                                    count: categoryButton.modelData.attentionCount
                                    Layout.alignment: Qt.AlignVCenter
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
                            onTapped: {
                                workspace.selectCategory(categoryButton.modelData.id)
                                window.popupAt(categoryMenu, categoryButton)
                            }
                        }
                    }
                }
                RequestsButton {
                    objectName: categoryRail.visible ? "reviewAttention" : "railRequests"
                    Layout.fillWidth: true
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
                                ToolTip.text: qsTr("%n request(s) in other categories", "", count)
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
                                count: categoryOption.modelData.attentionCount
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
                RequestsButton {
                    objectName: categoryBar.visible ? "reviewAttention" : "barRequests"
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
                id: historyBanner
                objectName: "historyBar"
                visible: workspace.focusedSession !== null && workspace.focusedSession.live
                         && (workspace.focusedSession.historyActive || workspace.focusedSession.historyRequestPending)
                Layout.fillWidth: true
                Layout.preferredHeight: window.tabHeight + 8
                color: window.surfaceColor
                border.color: window.borderColor
                border.width: 1
                radius: window.chromeRadius

                RowLayout {
                    id: historyRow
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 6
                    CommandButton {
                        objectName: "historyOlder"
                        text: qsTr("Older")
                        enabled: {
                            const session = workspace.focusedSession
                            return !!session && session.live && !session.historyRequestPending
                        }
                        onClicked: {
                            const session = workspace.focusedSession
                            if (session)
                                session.olderHistory()
                        }
                    }
                    CommandButton {
                        objectName: "historyNewer"
                        text: qsTr("Newer")
                        enabled: {
                            const session = workspace.focusedSession
                            return !!session && session.historyActive && !session.historyRequestPending
                        }
                        onClicked: {
                            const session = workspace.focusedSession
                            if (session)
                                session.newerHistory()
                        }
                    }
                    CommandButton {
                        objectName: "historyLive"
                        text: qsTr("Live")
                        enabled: {
                            const session = workspace.focusedSession
                            return !!session && session.historyActive
                        }
                        onClicked: {
                            const session = workspace.focusedSession
                            if (session)
                                session.returnToLive()
                            preview.deferTerminalFocus()
                        }
                    }
                    PlainLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        color: window.mutedTextColor
                        elide: Text.ElideRight
                        text: !workspace.focusedSession ? "" :
                              workspace.focusedSession.historyRequestPending ? qsTr("Loading history…") :
                              qsTr("Read only") + (workspace.focusedSession.historyMessage.length > 0 ?
                                  " · " + workspace.focusedSession.historyMessage : "")
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

                TerminalSurface {
                    id: liveTerminal
                    objectName: "liveTerminal"
                    anchors.fill: parent
                    anchors.margins: 6
                    document: workspace.focusedSession
                    fontFamily: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontFamily : ""
                    fontPixelSize: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSize : 16
                    visible: document !== null
                    enabled: visible
                    // A history page stays interactive for the wheel, selection and
                    // typing back to live; input reaches the agent only when live.
                    interactive: visible && window.visible && !window.inputBlocked && document !== null
                                 && (preview.active || document.inputReady || document.historyActive)
                    focus: visible && window.visible && !window.inputBlocked
                    Component.onCompleted: if (focus)
                                               forceActiveFocus()
                }

                // An ended or unreachable agent keeps its last screen; say so on the
                // stage instead of leaving a cursor that looks live.
                Rectangle {
                    id: endedBar
                    objectName: "sessionEndedBar"
                    readonly property var session: workspace.focusedSession
                    readonly property string connection: session && session.live ? session.connectionState : ""
                    visible: connection === "ended" || connection === "disconnected"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
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

                ColumnLayout {
                    objectName: "emptyState"
                    visible: workspace.focusedSession === null
                    anchors.centerIn: parent
                    width: Math.min(420, parent.width - 32)
                    spacing: 14
                    PlainLabel {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: window.mutedTextColor
                        text: qsTr("Start an agent in %1.").arg(window.activeCategoryName)
                    }
                    CommandButton {
                        objectName: "emptyNewAgent"
                        text: qsTr("New agent")
                        hint: window.shortcutText("newAgent")
                        selected: true
                        Layout.alignment: Qt.AlignHCenter
                        enabled: window.interactionArmed
                        onClicked: window.openNewAgentDialog()
                    }
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
                // Hidden cards release their surfaces; off-screen ones are not
                // kept, so only what is visible renders.
                model: visible ? workspace.categorySessions : []
                cacheBuffer: 0
                highlight: null
                highlightFollowsCurrentItem: false
                currentIndex: window.focusedTabIndex()
                readonly property real cardWidth: Math.round(height * 1.9)
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

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
                    objectName: "agentTab_" + modelData.sessionId
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
                        border.width: agentTab.marked ? 2 : 1
                        border.color: agentTab.marked ? window.focusedBorderColor :
                                      agentHover.hovered ? window.mutedTextColor : window.borderColor
                        Behavior on border.color {
                            enabled: window.motionEnabled
                            ColorAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                        }
                    }
                    // Finished or needs you, and not looked at yet: a slow, quiet
                    // pulse of the card edge until the agent is selected. Pending
                    // requests use the attention color; a finished turn uses ink.
                    Rectangle {
                        id: unseenCue
                        objectName: "unseenCue_" + agentTab.modelData.sessionId
                        anchors.fill: parent
                        visible: agentTab.modelData.unseen && !agentTab.marked
                        color: "transparent"
                        radius: window.chromeRadius
                        border.width: 2
                        border.color: agentTab.modelData.attentionPending || agentTab.modelData.statusKind === "waiting" ?
                                          window.attentionColor : window.textColor
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
                        fontPixelSize: (typeof keymap !== "undefined" && keymap !== null) ? keymap.terminalFontSize : 16
                        interactive: false
                        enabled: false
                        frameInterval: 250
                        minimumScale: 0.5
                    }
                    HoverHandler { id: agentHover }
                    TapHandler {
                        enabled: window.interactionArmed
                        onTapped: {
                            workspace.selectSession(agentTab.modelData.sessionId)
                            preview.deferTerminalFocus()
                        }
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
}
