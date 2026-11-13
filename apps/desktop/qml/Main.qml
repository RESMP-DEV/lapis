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
    color: chrome.base
    title: windowTitle

    // Context objects: workspace, app and config are always objects (a window
    // given no config gets a defaults-only one). The optional services below
    // are an object or null, never undefined; check the xAvailable flag.
    readonly property bool alertsAvailable: alerts !== null
    readonly property bool nextPromptAvailable: nextPrompt !== null

    // Theme colors come from the config. Without a matching theme, the stage
    // uses a dark command-deck palette. Selection is a thin accent edge, not a second surface.
    // focusedBorderColor only marks where keyboard input goes; activity,
    // attention (pending requests) and fault (lost connection, errors) are
    // separate meanings and are always paired with a shape or text cue.
    readonly property var appearance: config.themes.find(function(theme) { return theme.name === config.themeName })
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
    // Every eased colour change in the chrome (hover, press, selection, a drop
    // target, a status) takes this one transition. It starts on the change
    // and is retargeted by the next, so it never holds input back.
    readonly property int colorDuration: 150
    readonly property bool colorMotion: motionEnabled && !chromeSwapping

    // The chrome's colours. While a thread (the visual view) covers the stage,
    // the sidebar, the window around the stage, the stage's frame, the top bar
    // and the strip take the thread's theme, so the side panel belongs to the
    // page beside it, as an editor's does; with a terminal showing they keep
    // the window theme. Terminals keep their own colours either way.
    readonly property var windowTones: ({
        thread: false,
        base: backgroundColor, panel: surfaceColor, panelEdge: borderColor, stage: paneColor,
        card: cardColor, hover: hoveredCardColor, press: focusedColor, border: borderColor,
        text: textColor, muted: mutedTextColor, accent: focusedBorderColor,
        activity: activityColor, attention: attentionColor, fault: faultColor, plenty: plentyColor,
        categories: appearance && appearance.categoryColors ? appearance.categoryColors : ({}),
        quietCategories: appearance && appearance.categoryQuietColors ? appearance.categoryQuietColors : ({})
    })
    // A thread theme's chrome, from its own roles: the panel is the theme's
    // recessed tone (its code well, as Tokyo Night's side bar is), the gaps
    // around the stage share it, and the stage's frame is the page. Category
    // hues and the attention colour come from a window theme of the same kind
    // (light or dark) as the thread, so a light page gets a light sidebar.
    function threadTones(c) {
        const light = c.dark === false
        let kin = appearance
        if (!appearance || (appearance.light === true) !== light) {
            const name = visualSettings[light ? "lightWindowTheme" : "darkWindowTheme"]
            kin = config.themes.find(theme => theme.name === name) || appearance
        }
        const panel = c.code
        return {
            thread: true,
            base: panel, panel: panel, panelEdge: panel, stage: c.background,
            card: c.background, hover: Qt.tint(panel, Qt.alpha(c.text, 0.07)),
            press: Qt.tint(panel, Qt.alpha(c.text, 0.12)), border: c.border,
            text: c.text, muted: c.muted, accent: c.accent,
            activity: c.accent, attention: kin ? kin.attention : attentionColor, fault: c.fault, plenty: c.added,
            categories: kin && kin.categoryColors ? kin.categoryColors : ({}),
            quietCategories: kin && kin.categoryQuietColors ? kin.categoryQuietColors : ({})
        }
    }
    readonly property var chromeTarget: visualView.visible ? threadTones(visualView.colors) : windowTones
    // `chrome` (beside the stage) and `windowPalette` (dialogs and the rest)
    // are written only by applyChrome, and every eased colour reads one of
    // them: a change of palette (a theme, light or dark, a thread replacing a
    // terminal) then lands everywhere in the same frame as the thread itself,
    // with no eased half-way colour. Only state changes within a palette ease.
    property var chrome: windowTones
    property var windowPalette: windowTones
    property bool chromeSwapping: false
    onChromeTargetChanged: applyChrome()
    onWindowTonesChanged: applyChrome()
    function applyChrome() {
        chromeSwapping = true
        windowPalette = windowTones
        chrome = chromeTarget
        chromeSwapping = false
    }
    // One fixed-width family for the terminal and every machine readout (paths,
    // shortcut hints, states, counts). Human names and prose use the UI face.
    readonly property string monoFamily: liveTerminal.resolvedFontFamily
    readonly property int terminalFontSize: config.terminalFontSize
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
    readonly property string densityMode: config.densityName
    readonly property int chromeFont: densityMode === "minimal" ? Math.max(12, uiFont - 1) : uiFont
    readonly property int readoutFont: Math.max(11, chromeFont - 1)
    // The sidebar is a side panel, not a rail: its names sit a step above
    // the chrome's text and follow the terminal text size (0.85 of it, at
    // most eight over the chrome's), and its width grows with them.
    readonly property int sidebarFont: Math.max(chromeFont + 1, Math.min(chromeFont + 8, Math.round(terminalFontSize * 0.85)))
    readonly property int sidebarReadout: Math.max(11, sidebarFont - 2)
    // An agent's status mark, as large in the strip as in the sidebar.
    readonly property int statusMarkSize: Math.max(9, Math.round(sidebarFont * 0.72))
    // 240, 260 or 288 at the default text size, wider with larger text; never
    // over 420 or 30% of the window, so a narrow window keeps its stage. The
    // factory view's three columns (sidebar, thread, board) take the 240.
    readonly property int railWidth: Math.min(420, Math.round(width * 0.3),
                                              Math.round((densityMode === "minimal" || factoryHomeShown ? 240
                                                          : densityMode === "compact" ? 260 : 288)
                                                         * sidebarFont / (chromeFont + 1)))
    readonly property int tabHeight: Math.max(32, chromeFont + (densityMode === "minimal" ? 16 : densityMode === "compact" ? 20 : 24))
    readonly property int sidebarHeaderHeight: Math.max(tabHeight, sidebarFont + (densityMode === "minimal" ? 16 : densityMode === "compact" ? 20 : 26))
    readonly property int sidebarRowHeight: Math.max(26, sidebarFont + (densityMode === "minimal" ? 10 : densityMode === "compact" ? 14 : 18))
    // The sidebar's inner padding: names start this far in from its edge.
    readonly property int sidebarInset: densityMode === "minimal" ? 10 : densityMode === "compact" ? 12 : 14
    // Every category with its agents, in the sidebar's order. The sidebar's
    // repeaters count these rather than take them as models, so a change in
    // a count or a mark updates rows in place instead of rebuilding them
    // (a rebuild would drop a drag, a hover and the scroll position).
    readonly property var sidebarGroups: workspace.categories
    readonly property var emptyGroup: ({id: "", name: "", agents: [], attentionCount: 0, color: "grey", colorChoice: ""})
    // The category palette's names in swatch order (category_colors.hpp);
    // the theme gives each its hex.
    readonly property var categoryColorNames: ["grey", "blue", "red", "yellow", "green", "pink", "purple", "cyan"]
    readonly property bool previewsEnabled: config.previewsVisible
    // The agent strip under the stage is the category's navigation: live
    // previews in tab order. Short windows get shorter cards, not fewer.
    readonly property int stripHeight: height < 640 ? 96 :
                                       densityMode === "minimal" ? 112 : densityMode === "compact" ? 128 : 144
    readonly property bool stripShown: previewsEnabled && workspace.categoryAgents.length > 0
    readonly property bool sidebarExpanded: config.sidebarVisible
    readonly property bool narrow: width < 860
    // The title line over the stage while the sidebar is hidden: the
    // focused agent's category (from the sidebar's groups) and its title as
    // its sidebar row gives it; the active category's name with no agent.
    readonly property bool stageTitleShown: narrow || !sidebarExpanded
    readonly property int stageTitleHeight: Math.round(chromeFont * 1.8)
    readonly property var stageTitleGroup: {
        const agent = workspace.focusedAgent
        const id = agent ? agentCategory(agent.agentId) : workspace.activeCategoryId
        return sidebarGroups.find(entry => entry.id === id) ?? null
    }
    readonly property string stageTitleText: workspace.focusedAgent
        ? agentTabTitle(workspace.focusedAgent, stageTitleGroup ? stageTitleGroup.agents : undefined)
        : activeCategoryName
    // Zero-duration themes and reduced motion change state instantly.
    readonly property bool motionEnabled: !app.reducedMotion && active && visible && motionDuration > 0
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
        const session = workspace.focusedAgent
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
        const configured = config.shortcutBindings[action]
        if (configured && configured.length > 0)
            return configured
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
            return app.settingsShortcuts
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
        if (action === "toggleFactory")
            return [mac ? "Meta+Shift+F" : "Ctrl+Shift+F"]
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
        return workspace.fixtureMode || session.title !== projectTitle(session.directory) ?
                    session.title : workspace.displayPath(session.directory)
    }
    // Agents started in the same folder share a default title; number the
    // later ones so each card and menu entry names a distinct agent.
    // `agents` is the session's category's list; the shown category's by default.
    function agentTabTitle(session, agents) {
        const base = agentBaseTitle(session)
        if (workspace.fixtureMode) return base
        const list = agents || workspace.categoryAgents
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
        config.setSidebarVisible(!sidebarExpanded)
    }
    function togglePreviews() {
        if (!interactionArmed) return
        config.setPreviewsVisible(!previewsEnabled)
    }
    function openCommandsDialog() {
        if (!interactionArmed) return
        commandsDialog.open()
    }
    readonly property bool conversationsAvailable: conversations !== null
    function openResumeDialog() {
        if (!interactionArmed || dialogsVisible() || !conversationsAvailable) return
        resumeDialog.open()
    }
    // The home list shown when nothing is open: actions, then recent
    // conversations, then the other categories that have agents.
    ListModel { id: homeModel }
    Component.onCompleted: {
        applyChrome()
        homeRebuild.restart()
        recallWindowMemory()
    }
    property var homeRuns: []
    function rebuildHome() {
        if (workspace.focusedAgent !== null && homeModel.count > 0)
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
        function onAgentsChanged() { homeRebuild.restart() }
    }
    Connections {
        target: window.conversationsAvailable ? conversations : null
        function onChanged() { homeRebuild.restart() }
    }
    // What gets the keyboard when no dialog does: the side terminal while it
    // shows, else the selected agent's terminal, or its thread's bar while
    // the thread covers the terminal, or the home list when nothing is open.
    // AppWindow gives it the keyboard after every move between agents.
    readonly property string focusTarget: sideTerminalOpen && terminalsAvailable && terminals.current !== null ?
                                              "sideTerminalSurface" :
                                          workspace.focusedAgent === null ? "homeList" :
                                          visualCovers ? "visualComposer" : "liveTerminal"
    // Command-` (or Control-`): a plain shell on this Mac or an ssh host,
    // beside the agents, for a quick command; never an agent. Command-~
    // picks the machine.
    readonly property bool terminalsAvailable: terminals !== null
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
        app.deferTerminalFocus()
    }
    function closeSideTerminal() {
        sideTerminalOpen = false
        app.deferTerminalFocus()
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
    readonly property var toolStatus: tools
    // The same for the visual view's file actions (the Transcript's own
    // `mediaActions` property would otherwise bind to itself).
    readonly property var fileActions: mediaActions
    readonly property bool toolsAvailable: toolStatus !== null
    // Built when a dialog that lists them is open: as a property it would be
    // rebuilt, and the closed lists' delegates with it, on every switch.
    function commandEntries() {
        const agent = workspace.focusedAgent
        const hasAgent = agent !== null
        const needAgent = qsTr("Select an agent first")
        const entries = []
        // keywords: other words it is found by, such as an older name.
        function add(id, label, action, enabled, reason, run, keywords) {
            entries.push({id: id, label: label, shortcut: action ? window.shortcutText(action) : "",
                          enabled: enabled, reason: reason || "", run: run, keywords: keywords || ""})
        }
        add("newAgent", qsTr("New agent"), "newAgent", true, "", () => window.openNewAgentDialog())
        add("newCategory", qsTr("New category"), "newCategory", true, "", () => window.openCategoryDialog("add"))
        add("openTools", qsTr("Tools"), "", toolsAvailable, qsTr("The tools pane is not available here"), () => window.openToolsDialog())
        add("toggleSidebar", sidebarExpanded ? qsTr("Hide sidebar") : qsTr("Show sidebar"), "toggleSidebar", true, "", () => window.toggleSidebar())
        add("togglePreviews", previewsEnabled ? qsTr("Hide agent previews") : qsTr("Show agent previews"), "togglePreviews", true, "", () => window.togglePreviews())
        // In the sidebar's order, into the next category after a category's last agent.
        const another = workspace.agents.length > (hasAgent ? 1 : 0)
        add("nextWindow", qsTr("Next agent"), "nextWindow", another, qsTr("Open another agent first"), () => workspace.nextAgentAcrossCategories(1))
        add("previousWindow", qsTr("Previous agent"), "previousWindow", another, qsTr("Open another agent first"), () => workspace.nextAgentAcrossCategories(-1))
        add("closeAgent", qsTr("Close agent"), "closeAgent", hasAgent, needAgent, () => window.closeFocusedAgent())
        add("reopenAgent", qsTr("Reopen closed agent"), "reopenAgent", workspace.canReopenAgent, qsTr("No agent was closed since lapis opened"), () => window.reopenAgent())
        add("resumeConversation", qsTr("Resume a conversation"), "resumeConversation", conversationsAvailable, qsTr("Not available here"), () => window.openResumeDialog())
        add("toggleTerminal", sideTerminalOpen ? qsTr("Hide terminal") : qsTr("Terminal"), "toggleTerminal", terminalsAvailable, qsTr("Not available here"), () => window.toggleTerminal())
        add("chooseTerminal", qsTr("Terminal on another machine"), "chooseTerminal", terminalsAvailable, qsTr("Not available here"), () => window.chooseTerminal())
        const conversationFile = hasAgent && !window.visualActive && !window.visualSupported
                                 ? workspace.conversationFile(agent.agentId) : ""
        add("toggleVisual", window.visualActive ? qsTr("Show terminal") : qsTr("Show visual view"), "",
            window.visualActive || window.visualSupported || conversationFile.length > 0,
            hasAgent ? (workspace.visualUnavailable(agent.agentId)
                        || qsTr("Only for a Claude Code, Codex or Antigravity agent on this Mac, or Claude Code over ssh"))
                     : needAgent,
            () => window.toggleVisual())
        add("toggleVisualAll", window.visualAll ? qsTr("Show terminals for all agents") : qsTr("Show visual view for all agents"), "",
            true, "", () => window.toggleVisualAll())
        add("toggleAppearance", window.visualSettings.light ? qsTr("Switch to dark mode") : qsTr("Switch to light mode"), "",
            true, "", () => window.toggleAppearance())
        add("toggleFactory", qsTr("Toggle Factory view"), "toggleFactory", true, "", () => window.toggleFactory(),
            qsTr("factory pipeline orchestrator classic"))
        add("pinFactoryHome", qsTr("Pin agent as Factory orchestrator"), "",
            hasAgent && workspace.homeAgentId !== agent.agentId, needAgent,
            () => workspace.setHomeAgent(agent.agentId))
        add("textBigger", qsTr("Bigger text"), "textBigger", true, "", () => window.changeTextSize(1))
        add("textSmaller", qsTr("Smaller text"), "textSmaller", true, "", () => window.changeTextSize(-1))
        add("textReset", qsTr("Default text size"), "textReset", true, "", () => window.changeTextSize(0))
        const stopped = hasAgent && agent.live && (agent.connectionState === "ended" || agent.connectionState === "disconnected")
        add("restartAgent", qsTr("Restart agent"), "", stopped, qsTr("Only an ended or unreachable agent restarts"), () => workspace.restartAgent(agent.agentId))
        // A CLI rereads its settings only when it starts: these end it and
        // start it again, resuming its conversation.
        add("reloadTab", qsTr("Reload agent"), "", hasAgent && agent.live, needAgent, () => workspace.reloadAgent(agent.agentId))
        add("reloadCategory", qsTr("Reload category"), "", workspace.categoryAgents.length > 0, qsTr("This category has no agents"), () => workspace.reloadCategory())
        add("reloadWindow", qsTr("Reload window"), "", workspace.agents.length > 0, qsTr("No agents are open"), () => workspace.reloadAll())
        add("nextCategory", qsTr("Next category"), "nextCategory", workspace.categories.length > 1, qsTr("Add another category first"), () => workspace.nextCategory())
        add("previousCategory", qsTr("Previous category"), "previousCategory", workspace.categories.length > 1, qsTr("Add another category first"), () => workspace.nextCategory(-1))
        for (let i = 0; i < Math.min(9, workspace.categories.length); ++i) {
            const category = workspace.categories[i]
            add("category" + (i + 1), qsTr("Switch category: %1").arg(category.name), "category" + (i + 1), true, "", () => workspace.selectCategory(category.id))
        }
        add("renameCategory", qsTr("Rename category"), "", true, "", () => window.openCategoryDialog("rename"))
        add("removeCategory", qsTr("Remove category"), "", window.canRemoveActiveCategory(), qsTr("Move its agents first; keep at least one category"), () => window.removeActiveCategory())
        add("renameAgent", qsTr("Rename agent"), "", hasAgent, needAgent, () => window.openRenameDialog())
        add("earlier", qsTr("Move agent earlier"), "", hasAgent && window.focusedTabIndex() > 0, qsTr("Already first in this category"), () => window.moveFocused(-1))
        add("later", qsTr("Move agent later"), "", hasAgent && window.focusedTabIndex() < workspace.categoryAgents.length - 1, qsTr("Already last in this category"), () => window.moveFocused(1))
        for (const category of workspace.categories) {
            if (category.id !== workspace.activeCategoryId)
                add("move:" + category.id, qsTr("Move agent to %1").arg(category.name), "", hasAgent, needAgent, () => workspace.moveAgent(agent.agentId, category.id))
        }
        add("requests", qsTr("Review requests"), "", hasAgent && agent.attentionCount > 0, qsTr("No pending requests"), () => window.openAttentionDialog())
        add("older", qsTr("Browse older history"), "", hasAgent && agent.live && agent.connectionState === "ready" && !agent.historyRequestPending, qsTr("Connect the agent and wait for pending history"), () => agent.olderHistory())
        add("newer", qsTr("Browse newer history"), "", hasAgent && agent.historyActive && !agent.historyRequestPending, qsTr("Open archived history first"), () => agent.newerHistory())
        add("live", qsTr("Return to live output"), "", hasAgent && agent.historyActive, qsTr("Already showing live output"), () => agent.returnToLive())
        // Takes the agent back from another view, such as the phone holding a
        // session that cannot be joined; nothing else reclaims a replaced view.
        add("reconnect", qsTr("Reconnect agent"), "", window.recoveryAvailable(), qsTr("The selected agent is already connected or opening"), () => agent.reconnect())
        add("appearance", qsTr("Settings"), "openSettings", true, "", () => window.openSettingsDialog(),
            qsTr("appearance preferences options"))
        add("reloadConfig", qsTr("Reload configuration"), "reloadConfig", config.fileBacked, qsTr("No configuration in this fixture"), () => config.reload())
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

    // The status mark says what the agent is doing, never whether you looked
    // at it, and every class has its own shape so no state depends on colour
    // alone: a filled dot in the activity colour while it works, a quiet
    // hollow ring once its turn is done (finished or idle, looked at or not),
    // a diamond in the attention colour while it waits on your answer, a
    // square when its connection is lost, a dash once it ended, and a hollow
    // box while it opens or its status is unknown. The sidebar and the strip
    // draw the same mapping.
    function statusShape(kind) {
        if (kind === "working")
            return "dot"
        if (kind === "idle" || kind === "finished")
            return "ring"
        if (kind === "waiting")
            return "diamond"
        if (kind === "disconnected")
            return "square"
        if (kind === "ended")
            return "dash"
        return "box"
    }
    // A status's colour in the chrome's palette.
    function statusColor(kind) {
        return statusTone(kind, chrome)
    }
    function statusTone(kind, t) {
        if (kind === "working")
            return t.activity
        if (kind === "waiting")
            return t.attention
        if (kind === "disconnected")
            return t.fault
        return t.muted
    }
    // A category colour's hex on this theme; the focus accent without one.
    function categoryHex(name) {
        return categoryTone(name, windowPalette)
    }
    // The quieter name colour of a group that is not shown.
    function categoryQuietHex(name) {
        return categoryQuietTone(name, windowPalette)
    }
    // The same in a palette (windowPalette or chrome).
    function categoryTone(name, t) {
        return t.categories[name] ? t.categories[name] : t.accent
    }
    function categoryQuietTone(name, t) {
        return t.quietCategories[name] ? t.quietCategories[name] : t.muted
    }
    function categoryColorLabel(name) {
        return {grey: qsTr("Grey"), blue: qsTr("Blue"), red: qsTr("Red"), yellow: qsTr("Yellow"),
                green: qsTr("Green"), pink: qsTr("Pink"), purple: qsTr("Purple"), cyan: qsTr("Cyan")}[name] || name
    }
    readonly property string activeCategoryColor: {
        const group = sidebarGroups.find(entry => entry.id === workspace.activeCategoryId)
        return group ? group.color : ""
    }
    // The category an agent is in, by id.
    function agentCategory(id) {
        for (const group of sidebarGroups) {
            if (group.agents.some(agent => agent.agentId === id))
                return group.id
        }
        return ""
    }
    // Every agent in the sidebar's order: each category's in turn.
    function sidebarIds() {
        const ids = []
        for (const group of sidebarGroups) {
            for (const agent of group.agents)
                ids.push(agent.agentId)
        }
        return ids
    }
    // Recall number for categories reachable by a category1-9 shortcut; the
    // sidebar's names show the shortcut on hover.
    function categoryRecall(index) {
        return index < 9 && bindings("category" + (index + 1)).length > 0 ? String(index + 1) : ""
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
        const session = workspace.focusedAgent
        const list = workspace.categoryAgents
        if (!session)
            return -1
        for (let i = 0; i < list.length; ++i) {
            if (list[i].agentId === session.agentId)
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
            app.deferTerminalFocus()
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
        if (menusVisible()) {
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
        if (workspace.focusedAgent && workspace.focusedAgent.hasAttentionSource)
            attentionDialog.showAgent(workspace.focusedAgent)
    }

    // What the last new agent was started with, kept for the next one: the
    // CLI, the approval mode across CLIs, and each CLI's model.
    property string lastHarness: ""
    // The machine the last new agent started on: "" for this Mac, else an ssh host.
    property string lastMachine: ""
    property string lastMode: ""
    property var lastModels: ({})
    // The window's own choices outlive a restart of the window (guiState;
    // null in fixtures): the new-agent form's and the side terminal's.
    readonly property var windowMemory: guiState
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
        if (workspace.focusedAgent !== null) {
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
        const session = workspace.focusedAgent
        if (!session || session.closing)
            return
        if (!session.live || session.connectionState === "ended") {
            workspace.closeAgent(session.agentId, false)
            return
        }
        confirmCloseAgent(session)
    }
    function confirmCloseAgent(session) {
        closeAgentDialog.agentId = session.agentId
        closeAgentDialog.harnessId = session.harnessId
        closeAgentDialog.agentName = session.agentName || session.title
        closeAgentDialog.place = workspace.displayPath(session.directory)
        closeAgentDialog.reachable = session.reachable
        openFresh(closeAgentDialog)
    }
    // Close from the factory board. The judge's Safe to close (or an agent
    // that has already ended) closes in one click; any other running agent is
    // confirmed first. Fixture agents are never live, so fixture mode asks the
    // same way a live agent would.
    function closeFactoryAgent(id, safeToClose) {
        if (terminalBusy)
            return
        const session = workspace.agents.find(a => a.agentId === id)
        if (!session || session.closing)
            return
        const running = session.live ? session.connectionState !== "ended" : workspace.fixtureMode
        if (safeToClose || !running) {
            workspace.closeAgent(id, false)
            return
        }
        confirmCloseAgent(session)
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
        const session = workspace.focusedAgent
        if (!session || dialogsVisible())
            return
        renameAgentDialog.agentId = session.agentId
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
            workspace.placeAgents(pendingCategoryAgents, workspace.activeCategoryId, 1 << 20)
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
        if (!workspace.renameAgent(renameAgentDialog.agentId, renameAgentField.text.trim())) {
            renameAgentDialog.localError = workspace.workspaceError.length > 0 ? workspace.workspaceError :
                                                                              qsTr("Could not rename the agent.")
            return
        }
        renameAgentDialog.localError = ""
        renameAgentDialog.close()
    }

    function canRemoveActiveCategory() {
        return workspace.categories.length > 1 && workspace.categoryAgents.length === 0
    }

    function removeActiveCategory() {
        if (!canRemoveActiveCategory())
            return
        workspace.removeCategory(workspace.activeCategoryId)
    }

    function moveFocused(delta) {
        const session = workspace.focusedAgent
        if (!session)
            return
        workspace.moveAgentBy(session.agentId, delta)
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
    // "All agents" (saved in lapis.json) shows every Claude Code, Codex and
    // Antigravity agent on this Mac as a thread; visualAgents holds this run's per-agent choices,
    // which win over it until lapis restarts or "all" is switched again.
    property var visualAgents: ({})
    readonly property var visualSettings: config.visual
    readonly property bool visualAll: visualSettings.all === true
    readonly property bool visualWanted: {
        const agent = workspace.focusedAgent
        if (agent === null)
            return false
        // The factory view's orchestrator always shows as its thread.
        if (factoryHomeShown && agent.agentId === factoryHomeId)
            return true
        const choice = visualAgents[agent.agentId]
        return choice === undefined ? visualAll : choice
    }
    // A Claude Code, Codex or Antigravity agent on this Mac can show as a
    // thread (Workspace::visualSupported), from the moment it opens.
    // A Claude Code agent over ssh shows its conversation mirrored from that
    // machine (RemoteThreads); visualProbe moves when its mirror changes.
    readonly property bool visualSupported: workspace.focusedAgent !== null && visualProbe >= 0
                                            && workspace.visualSupported(workspace.focusedAgent.agentId)
    // The shown agent's mirror ({} for an agent on this Mac): machine, phase,
    // reason, status, cached, behind.
    readonly property var remoteThread: workspace.focusedAgent !== null && visualProbe >= 0
                                        ? workspace.remoteThreadState(workspace.focusedAgent.agentId) : ({})
    // The ssh host the shown agent runs on, "" on this Mac.
    readonly property string remoteMachine: workspace.focusedAgent !== null
                                            ? workspace.agentPlace(workspace.focusedAgent.agentId).machine || "" : ""
    Connections {
        target: workspace
        function onRemoteThreadChanged(agentId) {
            if (workspace.focusedAgent !== null && workspace.focusedAgent.agentId === agentId)
                window.visualProbe += 1
        }
        function onAttachmentProgress(token, fraction) { visualView.uploadProgress(token, fraction) }
        function onAttachmentUploaded(token, remote, error) { visualView.uploadFinished(token, remote, error) }
    }
    // The file appears once the CLI saves its first message; until then the
    // check repeats while the thread is wanted, and the thread shows empty
    // (visualBlank). A Codex or Antigravity agent's keeps being checked, so
    // the thread follows its /clear or /new.
    property int visualProbe: 0
    readonly property string visualFile: visualProbe >= 0 && visualWanted
                                         ? workspace.conversationFile(workspace.focusedAgent.agentId) : ""
    readonly property bool visualActive: visualWanted && (visualFile.length > 0 || visualSupported)
    readonly property bool visualBlank: visualActive && visualFile.length === 0
    // A prompt sent before the file exists is looked for every half second,
    // so the thread takes over as soon as the CLI saves it.
    readonly property bool visualFirstSend: visualBlank && workspace.transcript.pendingPrompts.length > 0
    Timer {
        interval: window.visualFirstSend ? 500 : 2000
        repeat: true
        running: window.visualWanted && (window.visualFile.length === 0
                                         || workspace.focusedAgent.harnessId === "codex"
                                         || workspace.focusedAgent.harnessId === "agy")
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
    readonly property bool visualCovers: visualActive && !visualDialog && !visualStartScreen
    property string visualDialogAgent: ""
    function watchVisualDialog(found) {
        if (!visualActive || liveTerminal.document === null)
            return
        visualDialogAgent = liveTerminal.document.agentId
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
            if (!window.visualActive || agent === null || agent.agentId !== window.visualDialogAgent) {
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
    // Before the CLI saved anything, its terminal can show a screen of its
    // own in place of its input box: a trust-this-folder prompt, a login, an
    // update menu. While there is no file the screen is read four times a
    // second. No input box yet: the bar holds what is typed (visualStarting).
    // A dialog in Claude Code's layout, or anything else drawn with no input
    // box for a second, shows the terminal with the keyboard until the input
    // box is back, unless the agent is at work or raised a request (those
    // stay the agent's, as above), or the person typed in the bar in the
    // last 1.5 s (the keys would land in the screen). lapis never answers it.
    property bool visualStartScreen: false
    property bool visualStarting: false
    property int visualStartTicks: 0
    property string visualStartAgent: ""
    property double visualTypedAt: 0
    Connections {
        target: visualView.composer
        function onTextChanged() { window.visualTypedAt = Date.now() }
    }
    function endStartScreen() {
        visualStartScreen = false
        visualStarting = false
        visualStartTicks = 0
        visualStartAgent = ""
    }
    function readStartScreen() {
        const agent = liveTerminal.document
        if (agent === null || !visualBlank) {
            endStartScreen()
            return
        }
        if (agent.agentId !== visualStartAgent) {
            endStartScreen()
            visualStartAgent = agent.agentId
        }
        const screen = liveTerminal.screenLines()
        // The model its start screen names, when lapis gave it none.
        workspace.transcript.readStartScreen(screen)
        const known = workspace.transcript.dialogShowing(screen)
        if (!known && workspace.transcript.inputBoxShowing(screen)) {
            visualStartTicks = 0
            visualStarting = false
            visualStartScreen = false
            return
        }
        visualStarting = true
        if (visualStartScreen)
            return
        const drawn = screen.some(row => row.trim().length > 0)
        const dialog = known || (drawn && ++visualStartTicks >= 4)
        if (dialog && agent.statusKind !== "working" && !agent.attentionPending
                && Date.now() - visualTypedAt > 1500)
            visualStartScreen = true
    }
    Timer {
        interval: 250
        repeat: true
        triggeredOnStart: true
        running: window.visualBlank && window.visible
        onTriggered: window.readStartScreen()
        onRunningChanged: if (!running) window.endStartScreen()
    }
    // Selecting another agent can show or hide the thread on its own (All
    // agents); the keyboard follows whichever is shown.
    onVisualCoversChanged: if (!inputBlocked && !dialogsVisible()) visualFocus()
    function visualFocus() {
        Qt.callLater(() => visualCovers ? visualView.composer.forceActiveFocus() : liveTerminal.forceActiveFocus())
    }
    function toggleVisual() {
        const agent = workspace.focusedAgent
        if (!agent)
            return
        const next = Object.assign({}, visualAgents)
        next[agent.agentId] = !visualActive
        visualAgents = next
        endVisualDialog()
        visualFocus()
    }
    // Light or dark reading: the thread and the window around it, never the
    // terminals. From "Match macOS" it switches to the other kind for good.
    function toggleAppearance() {
        config.setAppearance(visualSettings.light === true ? "dark" : "light")
    }
    function toggleVisualAll() {
        visualAgents = ({})
        config.setVisual("all", !visualAll)
        visualFocus()
    }
    // Factory view (visual.factory): one orchestrator conversation fills the
    // stage beside the pipeline board. A board card, an agent link in the
    // thread or the sidebar shows a worker for a look, with a bar back.
    readonly property bool factoryMode: visualSettings.factory === true
    property string factoryInspectAgentId: ""
    readonly property bool factoryInspecting: factoryMode && factoryInspectAgentId.length > 0
    readonly property var factoryHome: workspace.effectiveHomeAgent
    readonly property string factoryHomeId: factoryHome ? factoryHome.agentId : ""
    readonly property bool factoryHomeShown: factoryMode && !factoryInspecting
    property bool factoryBoardCollapsed: false
    // The inspected worker's board entry (stage and receipt for the bar).
    readonly property var factoryInspected: {
        if (!factoryInspecting)
            return null
        const items = workspace.factoryPipeline
        for (let i = 0; i < items.length; ++i)
            if (items[i].agentId === factoryInspectAgentId)
                return items[i]
        return null
    }
    function factoryStageLabel(stage) {
        return stage === "needs_you" ? qsTr("Needs You")
             : stage === "in_progress" ? qsTr("In Progress")
             : stage === "landed" ? qsTr("Landed") : qsTr("Idle")
    }
    function factoryStageColor(stage) {
        return stage === "needs_you" ? attentionColor : stage === "in_progress" ? activityColor
             : stage === "landed" ? plentyColor : mutedTextColor
    }
    function toggleFactory() {
        if (!interactionArmed)
            return
        const next = !factoryMode
        factoryInspectAgentId = ""
        config.setVisual("factory", next)
        if (next)
            showFactoryHome()
        visualFocus()
    }
    function showFactoryHome() {
        factoryInspectAgentId = ""
        if (factoryHomeId.length > 0
                && (workspace.focusedAgent === null || workspace.focusedAgent.agentId !== factoryHomeId))
            workspace.selectAgent(factoryHomeId)
    }
    function backToFactory() {
        showFactoryHome()
        visualFocus()
    }
    function inspectFactoryAgent(id) {
        if (!factoryMode || id.length === 0 || id === factoryHomeId) {
            showFactoryHome()
            return
        }
        factoryInspectAgentId = id
        workspace.selectAgent(id)
        visualFocus()
    }
    // A worker's terminal rather than its thread, to answer it there.
    function openFactoryTerminal(id) {
        inspectFactoryAgent(id)
        if (factoryInspectAgentId === id && visualActive)
            toggleVisual()
    }
    function factoryDetails(id) {
        inspectFactoryAgent(id)
        if (workspace.focusedAgent !== null && workspace.focusedAgent.attentionCount > 0)
            openAttentionDialog()
    }
    // A link in the thread that names an agent opens it; true when handled.
    function openFactoryLink(link) {
        const internal = /^(lapis|conversation):/i.test(link)
        const id = workspace.resolveAgentLink(link)
        if (id.length === 0)
            return internal
        if (factoryMode)
            inspectFactoryAgent(id)
        else
            workspace.selectAgent(id)
        return true
    }
    // Choosing another agent anywhere while in the factory view inspects it;
    // choosing the orchestrator goes back.
    Connections {
        target: workspace
        function onFocusChanged() {
            if (!window.factoryMode)
                return
            const agent = workspace.focusedAgent
            const id = agent ? agent.agentId : ""
            if (id === window.factoryHomeId)
                window.factoryInspectAgentId = ""
            else if (id.length > 0)
                window.factoryInspectAgentId = id
        }
        function onHomeAgentChanged() {
            if (window.factoryHomeShown)
                Qt.callLater(window.showFactoryHome)
        }
        Component.onCompleted: if (window.factoryMode) Qt.callLater(window.showFactoryHome)
    }
    // Each CLI's commands for the visual view's model and effort pickers. A
    // CLI without an entry shows what it reports but offers no choices.
    // The models and each model's effort levels are what the CLI lists now
    // (workspace.pickerModels, followed through modelsRevision); the
    // transcript model's efforts() are the fallback for a model listed
    // without levels.
    readonly property var visualCommands: ({
        claude: {
            model: id => "/model " + id,
            effort: level => "/effort " + level
        },
        // Antigravity's /model takes a name, slug or label and /effort a
        // level directly (agy 1.5.7); a family's model is sent as its
        // level's own id. Codex's /model opens a picker, so Codex offers
        // no choices.
        agy: {
            model: id => "/model " + id,
            effort: level => "/effort " + level
        }
    })
    function visualModels(agent) {
        if (!agent || !visualCommands[agent.harnessId] || workspace.modelsRevision < 0)
            return []
        return workspace.pickerModels(agent.harnessId)
    }
    function visualEfforts(agent) {
        const commands = agent ? visualCommands[agent.harnessId] : null
        return commands ? workspace.transcript.efforts(agent.harnessId) : []
    }
    // `shown` and `effort`: what the bar shows at once for a model choice
    // (the family and its level), `label` the CLI's name for it.
    function visualCommand(kind, value, shown, effort, label) {
        const agent = liveTerminal.document
        const commands = agent ? visualCommands[agent.harnessId] : null
        if (!commands || !commands[kind])
            return
        const before = liveTerminal.screenLines()
        if (!liveTerminal.submitText(commands[kind](value)))
            return
        if (kind === "model")
            workspace.transcript.choose(shown, effort, label, before)
        else
            workspace.transcript.choose("", value, "", before)
        visualChoiceTicks = 0
        watchVisualDialog(false)
    }
    // The CLI's answer to a choice: its screen (and agy's saved default)
    // read four times a second for up to 30 s while the choice is pending.
    property int visualChoiceTicks: -1
    Timer {
        interval: 250
        repeat: true
        running: window.visualActive && window.visualChoiceTicks >= 0 && workspace.transcript.choicePending
        onTriggered: {
            if (++window.visualChoiceTicks > 120 || liveTerminal.document === null)
                window.visualChoiceTicks = -1
            else
                workspace.transcript.watchChoice(liveTerminal.screenLines())
        }
    }
    // A new agent from this conversation: up to a reply, or just before a
    // prompt with that prompt waiting in its input.
    function forkVisual(record, before, prefill) {
        const agent = workspace.focusedAgent
        if (!agent)
            return
        const fork = workspace.forkConversation(agent.agentId, record, before)
        if (fork.length === 0)
            return
        const defaults = workspace.agentDefaults()
        const mode = window.lastMode.length > 0 ? window.lastMode :
                     defaults.mode && defaults.mode.length > 0 ? defaults.mode : "full"
        const title = (agent.title.length > 0 ? agent.title : "claude").slice(0, 73) + " (fork)"
        workspace.resumeAgent(workspace.agentFolder(agent.agentId), title, "claude", fork, mode, prefill)
    }

    function stripIds() {
        return workspace.categoryAgents.map(session => session.agentId)
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
        const focused = workspace.focusedAgent ? workspace.focusedAgent.agentId : ""
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
        if (workspace.selectAgent(id))
            app.deferTerminalFocus()
    }
    // Dragging a selected card or sidebar row carries the whole selection,
    // in the sidebar's order (the strip's, within a category).
    function agentsToDrag(id) {
        if (!isAgentSelected(id) || selectedAgents.length < 2)
            return [id]
        const ids = sidebarIds().filter(candidate => isAgentSelected(candidate))
        return ids.indexOf(id) >= 0 ? ids : [id]
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
        const first = workspace.agents.find(session => session.agentId === ids[0])
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
        workspace.placeAgents(ids, workspace.activeCategoryId, index)
        clearAgentSelection()
    }
    // Agents dropped on a category go to its end; any already there stay put.
    function dropOnCategory(categoryId) {
        const ids = draggedAgents.filter(id => agentCategory(id) !== categoryId)
        if (ids.length > 0)
            workspace.placeAgents(ids, categoryId, 1 << 20)
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
    readonly property bool desktopAvailable: desktop !== null
    // Command-plus, minus and zero: the terminal text size, saved to the config.
    function changeTextSize(delta) {
        const next = delta === 0 ? config.terminalFontSizeDefault : config.terminalFontSize + delta
        config.setTerminalFontSize(Math.max(config.terminalFontSizeMinimum,
                                            Math.min(config.terminalFontSizeMaximum, next)))
    }
    function reopenAgent() {
        if (interactionArmed && workspace.reopenAgent())
            app.deferTerminalFocus()
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
        const destination = workspace.focusedAgent
        if (!destination || liveTerminal.document !== destination)
            return false
        if (destination.agentId !== destinationId)
            return false
        return liveTerminal.pasteText(text)
    }

    function recoveryAvailable() {
        const session = workspace.focusedAgent
        return session && session.live && !session.inputReady
                && session.connectionState !== "connecting"
                && session.connectionState !== "synchronizing"
    }

    onActiveChanged: if (active)
                         keyboardOwnershipReady()
    onInputBlockedChanged: if (!inputBlocked) {
                               keyboardOwnershipReady()
                               app.deferTerminalFocus()
                           }

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
        objectName: "factoryShortcut"
        sequences: window.bindings("toggleFactory")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        autoRepeat: false
        onActivated: window.toggleFactory()
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
        // Eased colours read windowPalette, so a theme change lands at once.
        surfaceColor: window.windowPalette.panel
        textColor: window.windowPalette.text
        mutedColor: window.windowPalette.muted
        accentColor: window.windowPalette.accent
        selectionColor: window.windowPalette.press
        hoverColor: window.windowPalette.hover
        borderColor: window.windowPalette.border
        monoFamily: window.monoFamily
        uiFont: window.chromeFont
        readoutFont: window.readoutFont
        chromeRadius: window.chromeRadius
        motionDuration: window.colorDuration
        motionEnabled: window.colorMotion
        onChosen: function(conversation) { Qt.callLater(function() { window.resumeConversation(conversation) }) }
        onClosed: app.deferTerminalFocus()
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
        motionDuration: window.colorDuration
        motionEnabled: window.colorMotion
        onClosed: app.deferTerminalFocus()
    }

    Commands {
        id: commandsDialog
        commands: visible ? window.commandEntries() : []
        surfaceColor: window.windowPalette.panel
        textColor: window.windowPalette.text
        mutedColor: window.windowPalette.muted
        accentColor: window.windowPalette.accent
        selectionColor: window.windowPalette.press
        hoverColor: window.windowPalette.hover
        borderColor: window.windowPalette.border
        monoFamily: window.monoFamily
        uiFont: window.chromeFont
        readoutFont: window.readoutFont
        chromeRadius: window.chromeRadius
        motionDuration: window.colorDuration
        motionEnabled: window.colorMotion
        onRequested: function(commandId) { Qt.callLater(function() { window.runCommand(commandId) }) }
        onClosed: app.deferTerminalFocus()
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
        onActivated: workspace.nextAgentAcrossCategories(1)
    }
    Shortcut {
        objectName: "previousSessionShortcut"
        sequences: window.bindings("previousWindow")
        context: Qt.WindowShortcut
        enabled: window.shortcutsArmed
        onActivated: workspace.nextAgentAcrossCategories(-1)
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
            if (config.fileBacked)
                config.reload()
        }
    }

    component CommandButton: Button {
        id: control

        property bool selected: false
        // Optional machine readout, such as the configured shortcut.
        property string hint: ""
        // Drop the readout rather than overflow a button with a fixed width.
        property bool hintWhenRoom: false
        // Its palette: the window theme's, or the chrome's beside a thread.
        property var tones: window.windowPalette
        property color frameColor: selected ? tones.accent : tones.border

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
                    color: control.enabled ? control.tones.text : control.tones.muted
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
                    color: control.tones.muted
                }
            }
        }

        background: Rectangle {
            radius: window.chromeRadius
            color: control.down ? control.tones.press :
                   control.hovered ? control.tones.hover : control.tones.card
            border.width: control.selected ? 2 : 1
            border.color: control.frameColor
            ColorEase on color {}
            ColorEase on border.color {}
        }
    }

    // The one eased colour change (see colorDuration). A change of palette
    // turns it off for that frame, so a theme or a thread replacing a
    // terminal never passes through a half-way colour.
    component ColorEase: Behavior {
        enabled: window.colorMotion
        ColorAnimation { duration: window.colorDuration; easing.type: Easing.OutCubic }
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
        // Drawn this many pixels across; it grows with the text beside it.
        property int size: 8
        readonly property string shape: window.statusShape(kind)
        // Status marks live in the chrome beside the stage (sidebar, strip).
        readonly property color tone: window.statusTone(kind, window.chrome)
        readonly property real stroke: Math.max(1.5, size / 6)

        implicitWidth: size
        implicitHeight: size
        Rectangle {
            anchors.centerIn: parent
            visible: mark.shape === "dot" || mark.shape === "ring" || mark.shape === "square"
                     || mark.shape === "box"
            width: mark.size
            height: mark.size
            radius: mark.shape === "dot" || mark.shape === "ring" ? mark.size / 2 : 0
            antialiasing: true
            color: mark.shape === "dot" || mark.shape === "square" ? mark.tone : "transparent"
            border.width: mark.shape === "ring" || mark.shape === "box" ? mark.stroke : 0
            border.color: mark.tone
            ColorEase on color {}
            ColorEase on border.color {}
        }
        Rectangle {
            anchors.centerIn: parent
            visible: mark.shape === "diamond"
            width: Math.round(mark.size * 0.82)
            height: width
            rotation: 45
            antialiasing: true
            color: mark.tone
            ColorEase on color {}
        }
        Rectangle {
            anchors.centerIn: parent
            visible: mark.shape === "dash"
            width: mark.size
            height: Math.max(2, Math.round(mark.stroke))
            color: mark.tone
            ColorEase on color {}
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
        onClosed: app.deferTerminalFocus()
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
        themeModel: config.themes
        densityModel: config.densities
        currentTheme: config.themeName
        currentDensity: window.densityMode
        configPath: config.sourcePath
        configDiagnostic: config.diagnostic
        shortcutHint: window.shortcutText("openSettings")
        fontFamilies: app.monospaceFamilies
        fontFamily: config.terminalFontFamily
        resolvedFontFamily: window.monoFamily
        fontSize: liveTerminal.fontPixelSize
        fontSizeMinimum: config.terminalFontSizeMinimum
        fontSizeMaximum: config.terminalFontSizeMaximum
        fontSizeDefault: config.terminalFontSizeDefault
        motionDuration: window.colorDuration
        motionEnabled: window.colorMotion
        finishSound: config.finishSound
        keepAwake: config.keepAwake
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
        sidebarVisible: config.sidebarVisible
        previewsVisible: config.previewsVisible
        scrollToBottomOnTyping: config.scrollToBottomOnTyping
        values: config.settings
        defaults: config.defaults()
        harnesses: visible ? workspace.availableHarnesses().map(h => ({ id: h.id, name: h.name })) : []
        fileBacked: config.fileBacked
        onVisualChosen: function(key, value) { config.setVisual(key, value) }
        onAppearanceChosen: function(mode) { config.setAppearance(mode) }
        onSidebarChosen: function(on) { config.setSidebarVisible(on) }
        onPreviewsChosen: function(on) { config.setPreviewsVisible(on) }
        onScrollChosen: function(on) { config.setScrollToBottomOnTyping(on) }
        onSettingChosen: function(path, value) { config.setSetting(path, value) }
        onReloadRequested: config.reload()
        onOpenConfigRequested: window.fileActions.open(config.sourcePath)
        onOpenDocsRequested: Qt.openUrlExternally("https://github.com/RESMP-DEV/lapis/blob/main/docs/config.md")
        onLaunchAtLoginChosen: function(on) { if (window.desktopAvailable) desktop.setLaunchAtLogin(on) }
        onCheckUpdates: if (window.desktopAvailable) desktop.checkForUpdates()
        onFinishSoundChosen: function(on) { config.setFinishSound(on) }
        onKeepAwakeChosen: function(on) { config.setKeepAwake(on) }
        onChimePlayed: function(needsYou) { if (window.alertsAvailable) alerts.preview(needsYou) }
        onClosed: app.deferTerminalFocus()
        onFontFamilyChosen: function(name) { config.setTerminalFontFamily(name) }
        onFontSizeChosen: function(pixels) { config.setTerminalFontSize(pixels) }
        onThemeChosen: function(name) { config.setTheme(name) }
        onDensityChosen: function(name) { config.setDensity(name) }
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
        // This agent's offered model (from the CLI or the configured
        // replacement list) and approval mode. The mode stays across CLIs; a
        // CLI without it uses its nearest, less access first, and the
        // preference returns on a CLI that has it.
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
        title: reachable ? qsTr("End agent") : qsTr("Close agent")
        height: Math.min(window.height - 32, closeForm.implicitHeight + topPadding + bottomPadding)
        property string agentId: ""
        property string harnessId: ""
        property string agentName: ""
        property string place: ""
        property bool reachable: true
        function confirm() {
            const id = agentId
            const abandon = !reachable
            close()
            if (!workspace.closeAgent(id, abandon))
                app.deferTerminalFocus()
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
                              qsTr("Close the %1 agent in %2?").arg(closeAgentDialog.agentName).arg(closeAgentDialog.place)
                    color: window.textColor
                    font.pixelSize: window.chromeFont + 1
                    font.bold: true
                    wrapMode: Text.Wrap
                }
            }
            PlainLabel {
                Layout.fillWidth: true
                text: closeAgentDialog.reachable ?
                          qsTr("Its process stops and lapis closes it.") :
                          qsTr("lapis can't reach this agent. If it is still running, it keeps running outside lapis.")
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
                    text: closeAgentDialog.reachable ? qsTr("End agent") : qsTr("Close agent")
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
        property string agentId: ""
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
                  workspace.categoryAgents.length > 0 ? qsTr("Move agents out before removing") :
                  qsTr("Remove category")
            onTriggered: window.removeActiveCategory()
        }
        // The category's colour in the sidebar, from the fixed palette.
        Menu {
            id: categoryColorMenu
            objectName: "categoryColorMenu"
            title: qsTr("Colour")
            onClosed: window.onMenuClosed()
            background: MenuSurface {}

            Repeater {
                model: window.categoryColorNames
                delegate: MenuItem {
                    id: colorItem
                    required property string modelData
                    readonly property bool current: modelData === window.activeCategoryColor
                    objectName: "categoryColor_" + modelData
                    text: window.categoryColorLabel(modelData)
                    font.pixelSize: window.chromeFont
                    implicitHeight: Math.max(32, window.chromeFont + 16)
                    enabled: workspace.activeCategoryId.length > 0
                    onTriggered: workspace.setCategoryColor(workspace.activeCategoryId, modelData)
                    contentItem: RowLayout {
                        spacing: 10
                        Rectangle {
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            Layout.alignment: Qt.AlignVCenter
                            radius: 3
                            color: window.categoryHex(colorItem.modelData)
                        }
                        PlainText {
                            text: colorItem.text
                            color: window.textColor
                            font.pixelSize: window.chromeFont
                            font.weight: colorItem.current ? Font.DemiBold : Font.Normal
                            Layout.fillWidth: true
                            verticalAlignment: Text.AlignVCenter
                        }
                        PlainText {
                            text: colorItem.current ? "✓" : ""
                            color: window.textColor
                            font.pixelSize: window.chromeFont
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }
                }
            }
        }
        MenuSeparator {}
        ActionItem {
            objectName: "appearanceAction"
            text: qsTr("Settings")
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
            text: workspace.focusedAgent ? workspace.focusedAgent.title : ""
            visible: text.length > 0
        }
        ActionItem {
            objectName: "sessionDiagnostic"
            enabled: false
            text: workspace.focusedAgent ? (workspace.focusedAgent.attentionDiagnostic || workspace.focusedAgent.activity) : ""
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
            enabled: workspace.focusedAgent !== null
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
                return index >= 0 && index < workspace.categoryAgents.length - 1
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
                        const session = workspace.focusedAgent
                        if (session)
                            workspace.moveAgent(session.agentId, modelData.id)
                    }
                }
            }
        }
        MenuSeparator {}
        ActionItem {
            objectName: "closeAgentAction"
            text: qsTr("Close")
            enabled: workspace.focusedAgent !== null
            onTriggered: window.closeFocusedAgent()
        }
        MenuSeparator {
            visible: workspace.focusedAgent !== null && workspace.focusedAgent.live
        }
        ActionItem {
            objectName: "historyOlderAction"
            text: qsTr("Older history")
            visible: workspace.focusedAgent !== null && workspace.focusedAgent.live
            enabled: workspace.focusedAgent !== null && !workspace.focusedAgent.historyRequestPending
            onTriggered: if (workspace.focusedAgent)
                             workspace.focusedAgent.olderHistory()
        }
        ActionItem {
            objectName: "historyNewerAction"
            text: qsTr("Newer history")
            visible: workspace.focusedAgent !== null && workspace.focusedAgent.live
            enabled: workspace.focusedAgent !== null && workspace.focusedAgent.historyActive
                     && !workspace.focusedAgent.historyRequestPending
            onTriggered: if (workspace.focusedAgent)
                             workspace.focusedAgent.newerHistory()
        }
        ActionItem {
            objectName: "historyLiveAction"
            text: qsTr("Back to live")
            visible: workspace.focusedAgent !== null && workspace.focusedAgent.live
            enabled: workspace.focusedAgent !== null && workspace.focusedAgent.historyActive
            onTriggered: if (workspace.focusedAgent)
                             workspace.focusedAgent.returnToLive()
        }
    }

    Connections {
        target: workspace
        function onFocusChanged() {
            window.keyboardOwnershipReady()
            agentTabs.revealFocused()
            categoryList.revealActive()

            if (!window.inputBlocked)
                app.deferTerminalFocus()
        }
        function onAgentsChanged() {
            agentTabs.revealFocused()
        }
        function onCategoryChanged() {
            categoryList.revealActive()
            agentTabs.revealFocused()
        }
    }

    // With the sidebar hidden (Command-B, or a window too narrow for it) the
    // top of the window names the agent on the stage, quietly: its
    // category's colour as a small mark, then the title its sidebar row
    // shows. Text in the gap above the stage, no bar or button: it takes no
    // keyboard and no clicks, and hovering names the category.
    Item {
        id: stageTitle
        objectName: "stageTitle"
        visible: window.stageTitleShown && window.stageTitleText.length > 0
        x: 8
        width: window.width - 16
        height: window.stageTitleHeight
        Row {
            anchors.centerIn: parent
            spacing: Math.round(window.chromeFont * 0.5)
            Rectangle {
                objectName: "stageTitleMark"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(window.chromeFont * 0.55)
                height: width
                radius: width / 2
                color: window.categoryTone(window.stageTitleGroup ? window.stageTitleGroup.color : "",
                                           window.chrome)
                ColorEase on color {}
            }
            PlainText {
                objectName: "stageTitleText"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, stageTitle.width - 80)
                text: window.stageTitleText
                color: window.chrome.muted
                font.pixelSize: window.chromeFont
                elide: Text.ElideRight
                ColorEase on color {}
            }
        }
        HoverHandler { id: stageTitleHover }
        ToolTip.visible: stageTitleHover.hovered && window.stageTitleGroup !== null
        ToolTip.delay: 600
        ToolTip.text: window.stageTitleGroup ? window.stageTitleGroup.name : ""
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        // The stage starts under the agent's title when the sidebar is hidden.
        anchors.topMargin: window.stageTitleShown ? window.stageTitleHeight : 8
        spacing: 8

        Rectangle {
            id: categoryRail
            objectName: "categoryRail"
            visible: !window.narrow && window.sidebarExpanded
            Layout.preferredWidth: window.railWidth
            Layout.maximumWidth: window.railWidth
            Layout.fillHeight: true
            // Beside a thread the panel is the thread theme's recessed tone,
            // flush with the gaps around the stage (no divider of its own).
            color: window.chrome.panel
            border.color: window.chrome.panelEdge
            border.width: 1
            radius: window.chromeRadius

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Math.round(window.sidebarInset * 0.7)
                anchors.topMargin: 0
                spacing: 8

                // The sidebar is the navigation: every category as a colour-coded
                // group, its agents listed under it, scrolling as one list.
                Flickable {
                    id: categoryList
                    objectName: "sidebarList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: width
                    contentHeight: sidebarColumn.implicitHeight
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    // Scroll just enough to show `item`; never animated.
                    function reveal(item) {
                        if (!item || height <= 0)
                            return
                        const top = item.mapToItem(sidebarColumn, 0, 0).y
                        const limit = Math.max(0, contentHeight - height)
                        if (top < contentY)
                            contentY = Math.max(0, top)
                        else if (top + item.height > contentY + height)
                            contentY = Math.min(limit, top + item.height - height)
                    }
                    function revealActive() {
                        Qt.callLater(() => {
                            const group = sidebarRepeater.itemAt(window.categoryIndex(workspace.activeCategoryId))
                            if (group) {
                                categoryList.reveal(group.header)
                                categoryList.reveal(group.focusedRow())
                            }
                        })
                    }

                    Column {
                        id: sidebarColumn
                        width: categoryList.width
                        spacing: 8

                        Repeater {
                            id: sidebarRepeater
                            model: window.sidebarGroups.length
                            delegate: Column {
                                id: group
                                required property int index
                                readonly property var modelData: window.sidebarGroups[index] ?? window.emptyGroup
                                readonly property var agents: modelData.agents
                                readonly property bool active: modelData.id === workspace.activeCategoryId
                                readonly property color hue: window.categoryTone(modelData.color, window.chrome)
                                readonly property color quietHue: window.categoryQuietTone(modelData.color, window.chrome)
                                readonly property alias header: categoryButton
                                width: sidebarColumn.width
                                spacing: 0

                                function focusedRow() {
                                    for (let i = 0; i < agentRows.count; ++i) {
                                        const row = agentRows.itemAt(i)
                                        if (row && row.marked)
                                            return row
                                    }
                                    return null
                                }

                                // Categories are group labels: the name in the group's
                                // colour, the active group's in full strength and bold with a
                                // leading edge in that colour. No resting fill. The tooltip
                                // gives the Command-1 to 9 shortcut that recalls it.
                                Button {
                                    id: categoryButton
                                    readonly property var modelData: group.modelData
                                    readonly property int index: group.index
                                    objectName: "category_" + modelData.id
                                    readonly property bool marked: group.active
                                    readonly property string recall: window.categoryRecall(index)
                                    width: group.width
                                    height: window.sidebarHeaderHeight
                                    padding: 0
                                    focusPolicy: Qt.NoFocus
                                    hoverEnabled: true
                                    enabled: window.interactionArmed
                                    onClicked: workspace.selectCategory(modelData.id)
                                    Accessible.name: modelData.name
                                    ToolTip.visible: hovered && recall.length > 0
                                    ToolTip.delay: 600
                                    ToolTip.text: window.shortcutText("category" + (index + 1))

                                    background: Rectangle {
                                        radius: window.chromeRadius
                                        color: categoryDrop.containsDrag && categoryDrop.acceptsAgents ?
                                                   Qt.alpha(window.chrome.accent, 0.18) :
                                               categoryButton.hovered && !categoryButton.marked ? window.chrome.hover :
                                                                                                   window.chrome.panel
                                        border.width: categoryDrop.containsDrag && categoryDrop.acceptsAgents ? 1 : 0
                                        border.color: window.chrome.accent
                                        ColorEase on color {}
                                        Rectangle {
                                            objectName: "categoryMarker"
                                            x: 0
                                            width: 2
                                            height: parent.height - 10
                                            anchors.verticalCenter: parent.verticalCenter
                                            // Transparent in the hue's own colour, so the
                                            // fade never passes through black.
                                            color: categoryButton.marked ? group.hue : Qt.alpha(group.hue, 0)
                                            ColorEase on color {}
                                        }
                                    }

                                    contentItem: Item {
                                        RowLayout {
                                            anchors.fill: parent
                                            // The name starts a little in from the
                                            // panel's edge, above its rows' titles.
                                            anchors.leftMargin: window.sidebarInset - 2
                                            anchors.rightMargin: window.sidebarInset - 2
                                            spacing: 8
                                            // The name carries the group's colour: full for the
                                            // shown group (and under the pointer), quieter for
                                            // the rest.
                                            PlainText {
                                                objectName: "categoryName_" + categoryButton.modelData.id
                                                text: categoryButton.modelData.name
                                                color: categoryButton.marked || categoryButton.hovered ? group.hue :
                                                                                                         group.quietHue
                                                ColorEase on color {}
                                                font.pixelSize: window.sidebarFont
                                                font.weight: categoryButton.marked ? Font.DemiBold : Font.Medium
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                verticalAlignment: Text.AlignVCenter
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
                                        readonly property bool acceptsAgents: window.draggedAgents.some(
                                            id => window.agentCategory(id) !== categoryButton.modelData.id)
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
                                        color: window.chrome.accent
                                    }
                                }

                                // The category's agents in order. A thin line in the group's
                                // colour runs down its rows; the agent on the stage lights its
                                // segment and takes a faint fill of that colour.
                                Repeater {
                                    id: agentRows
                                    model: group.agents.length
                                    delegate: Item {
                                        id: agentRow
                                        required property int index
                                        readonly property var agent: group.agents[index] ?? null
                                        readonly property string agentId: agent ? agent.agentId : ""
                                        readonly property bool marked: agent !== null && workspace.focusedAgent !== null
                                                                       && workspace.focusedAgent.agentId === agentId
                                        readonly property bool picked: agentId.length > 0 && window.isAgentSelected(agentId)
                                        readonly property string title: agent ? window.agentTabTitle(agent, group.agents) : ""
                                        objectName: "sidebarAgent_" + agentId
                                        width: group.width
                                        height: window.sidebarRowHeight
                                        opacity: window.dragSource === "sidebar" && window.draggedAgents.indexOf(agentId) >= 0 ?
                                                     0.45 : 1
                                        Accessible.role: Accessible.Button
                                        Accessible.name: agent ? title + ", " + agent.statusLabel : ""
                                        Accessible.onPressAction: {
                                            if (window.interactionArmed && workspace.selectAgent(agentRow.agentId))
                                                app.deferTerminalFocus()
                                        }

                                        Rectangle {
                                            objectName: "sidebarAgentFill_" + agentRow.agentId
                                            anchors.fill: parent
                                            radius: window.chromeRadius
                                            // At rest, the hover tone at no strength, so an
                                            // eased change never dips through black.
                                            color: agentRow.marked ? Qt.alpha(group.hue, 0.16) :
                                                   rowPress.pressed ? window.chrome.press :
                                                   rowHover.hovered ? window.chrome.hover :
                                                   agentRow.picked ? Qt.alpha(window.chrome.accent, 0.08) :
                                                                     Qt.alpha(window.chrome.hover, 0)
                                            ColorEase on color {}
                                        }
                                        Rectangle {
                                            objectName: "sidebarAgentEdge_" + agentRow.agentId
                                            x: 0
                                            width: 2
                                            height: parent.height
                                            color: agentRow.marked ? group.hue : Qt.alpha(group.hue, group.active ? 0.55 : 0.38)
                                            ColorEase on color {}
                                        }
                                        RowLayout {
                                            anchors.fill: parent
                                            // The title starts under the group's name. Only the
                                            // rare marks (an answer wanted, a lost connection, an
                                            // ended agent) show, after the title; working, done and
                                            // opening agents show the title alone.
                                            anchors.leftMargin: window.sidebarInset
                                            anchors.rightMargin: window.sidebarInset - 2
                                            spacing: 10
                                            PlainText {
                                                id: rowTitle
                                                objectName: "sidebarTitle_" + agentRow.agentId
                                                text: agentRow.title
                                                color: agentRow.marked || rowHover.hovered || group.active ? window.chrome.text :
                                                                                                             window.chrome.muted
                                                ColorEase on color {}
                                                font.pixelSize: window.sidebarFont
                                                font.weight: agentRow.marked ? Font.DemiBold : Font.Normal
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            StatusMark {
                                                objectName: "sidebarStatus_" + agentRow.agentId
                                                kind: agentRow.agent ? agentRow.agent.statusKind : "unknown"
                                                size: window.statusMarkSize
                                                visible: shape !== "dot" && shape !== "ring" && shape !== "box"
                                                Layout.preferredWidth: size
                                                Layout.preferredHeight: size
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                        }
                                        HoverHandler { id: rowHover }
                                        ToolTip.visible: rowHover.hovered && rowTitle.truncated && !rowPress.dragging
                                        ToolTip.delay: 600
                                        ToolTip.text: agentRow.title
                                        // A click shows the agent (Command- and Shift-click pick
                                        // several, as on the strip); a drag carries it to a
                                        // category, the + or the strip.
                                        DragOrClick {
                                            id: rowPress
                                            objectName: "sidebarPress_" + agentRow.agentId
                                            anchors.fill: parent
                                            enabled: window.interactionArmed && agentRow.agent !== null
                                            onTapped: function(modifiers) { window.clickAgent(agentRow.agentId, modifiers) }
                                            onDragStarted: function(scene) {
                                                window.beginAgentDrag(window.agentsToDrag(agentRow.agentId), "sidebar", scene)
                                            }
                                            onDragMoved: function(scene) { window.moveDragGhost(scene) }
                                            onDragEnded: window.endDrag()
                                        }
                                        TapHandler {
                                            acceptedButtons: Qt.RightButton
                                            enabled: window.interactionArmed && agentRow.agent !== null
                                            onTapped: {
                                                if (workspace.selectAgent(agentRow.agentId))
                                                    window.popupAt(agentMenu, agentRow)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // A quiet plus right under the last category, lined up with
                        // the names, with its shortcut as a readout.
                        Button {
                            id: newCategoryButton
                            objectName: "newCategoryButton"
                            width: sidebarColumn.width
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
                                border.color: window.chrome.accent
                                color: newCategoryDrop.containsDrag ? Qt.alpha(window.chrome.accent, 0.18) :
                                       newCategoryButton.hovered ? window.chrome.hover : window.chrome.panel
                                ColorEase on color {}
                            }
                            contentItem: Item {
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: window.sidebarInset - 2
                                    anchors.rightMargin: 6
                                    spacing: 8
                                    PlainText {
                                        text: "+"
                                        color: newCategoryButton.hovered ? window.chrome.text : window.chrome.muted
                                        ColorEase on color {}
                                        font.pixelSize: window.sidebarFont
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    PlainText {
                                        objectName: "newCategoryHint"
                                        text: window.shortcutText("newCategory")
                                        color: window.chrome.muted
                                        font.family: window.monoFamily
                                        font.pixelSize: window.sidebarReadout
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
                // The visual view for the selected agent, or for every agent,
                // and light or dark reading beside its title.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 6
                    PlainText {
                        text: qsTr("Visual view")
                        color: window.chrome.muted
                        font.pixelSize: window.readoutFont
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        id: appearanceToggle
                        objectName: "appearanceToggle"
                        readonly property bool light: window.visualSettings.light === true
                        implicitWidth: Math.round(window.chromeFont * 1.9)
                        implicitHeight: implicitWidth
                        radius: window.chromeRadius
                        color: appearanceHover.hovered ? window.chrome.hover : Qt.alpha(window.chrome.hover, 0)
                        border.width: 1
                        border.color: appearanceHover.hovered ? window.chrome.muted : window.chrome.border
                        ColorEase on color {}
                        ColorEase on border.color {}
                        Accessible.role: Accessible.Button
                        Accessible.name: light ? qsTr("Switch to dark mode") : qsTr("Switch to light mode")
                        // The mode it switches to: a moon in light mode, a sun in dark.
                        Canvas {
                            id: appearanceGlyph
                            anchors.centerIn: parent
                            width: Math.round(parent.width * 0.56)
                            height: width
                            readonly property color ink: appearanceHover.hovered ? window.chrome.text : window.chrome.muted
                            readonly property bool moon: appearanceToggle.light
                            onInkChanged: requestPaint()
                            onMoonChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onPaint: {
                                const g = getContext("2d")
                                g.reset()
                                const s = width, c = s / 2
                                g.fillStyle = ink
                                g.strokeStyle = ink
                                if (moon) {
                                    g.beginPath()
                                    g.arc(c, c, s * 0.42, 0, Math.PI * 2)
                                    g.fill()
                                    g.globalCompositeOperation = "destination-out"
                                    g.beginPath()
                                    g.arc(c + s * 0.22, c - s * 0.16, s * 0.36, 0, Math.PI * 2)
                                    g.fill()
                                } else {
                                    g.beginPath()
                                    g.arc(c, c, s * 0.2, 0, Math.PI * 2)
                                    g.fill()
                                    g.lineWidth = Math.max(1.2, s / 11)
                                    g.lineCap = "round"
                                    for (let i = 0; i < 8; ++i) {
                                        const a = i * Math.PI / 4
                                        g.beginPath()
                                        g.moveTo(c + Math.cos(a) * s * 0.32, c + Math.sin(a) * s * 0.32)
                                        g.lineTo(c + Math.cos(a) * s * 0.46, c + Math.sin(a) * s * 0.46)
                                        g.stroke()
                                    }
                                }
                            }
                        }
                        HoverHandler { id: appearanceHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            enabled: window.interactionArmed
                            onTapped: window.toggleAppearance()
                        }
                        ToolTip.visible: appearanceHover.hovered
                        ToolTip.delay: 600
                        ToolTip.text: Accessible.name
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    CommandButton {
                        objectName: "visualThisAgent"
                        tones: window.chrome
                        text: qsTr("This agent")
                        selected: window.visualActive
                        enabled: window.interactionArmed && workspace.focusedAgent !== null
                                 && (window.visualActive || window.visualSupported || window.visualProbe >= 0
                                     && workspace.conversationFile(workspace.focusedAgent.agentId).length > 0)
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        onClicked: window.toggleVisual()
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: enabled ? qsTr("Switch this agent between its terminal and the thread")
                                              : (workspace.focusedAgent !== null && window.visualProbe >= 0
                                                 && workspace.visualUnavailable(workspace.focusedAgent.agentId))
                                                || qsTr("Only for a Claude Code, Codex or Antigravity agent on this Mac, or Claude Code over ssh")
                    }
                    CommandButton {
                        objectName: "visualAllAgents"
                        tones: window.chrome
                        text: qsTr("All agents")
                        selected: window.visualAll
                        enabled: window.interactionArmed
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        onClicked: window.toggleVisualAll()
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Show every Claude Code, Codex and Antigravity agent on this Mac as a thread")
                    }
                }
                // Factory view: the orchestrator's thread and the pipeline
                // board, or back to categories. Its own row, so no label
                // clips at the narrowest rail.
                CommandButton {
                    objectName: "visualFactoryMode"
                    tones: window.chrome
                    text: window.factoryMode ? qsTr("Factory · on") : qsTr("Factory")
                    hint: window.shortcutText("toggleFactory")
                    hintWhenRoom: true
                    selected: window.factoryMode
                    enabled: window.interactionArmed
                    Layout.fillWidth: true
                    onClicked: window.toggleFactory()
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: window.factoryMode
                                  ? qsTr("Back to categories and per-agent views")
                                  : qsTr("One orchestrator conversation beside every agent's pipeline stage")
                }
                CommandsButton {
                    objectName: "commandsButton"
                    tones: window.chrome
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

            // The factory view's bar: the orchestrator (and its picker), the
            // pipeline's counts and the way back to categories.
            Rectangle {
                id: factoryHeader
                objectName: "factoryHeader"
                visible: window.factoryHomeShown
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(34, window.chromeFont + 20)
                color: window.surfaceColor
                border.color: window.borderColor
                border.width: 1
                radius: window.chromeRadius
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 6
                    spacing: 8
                    Rectangle {
                        width: 3
                        Layout.preferredHeight: parent.height - 14
                        radius: 1
                        color: window.focusedBorderColor
                    }
                    PlainText {
                        text: qsTr("FACTORY")
                        color: window.focusedBorderColor
                        font.pixelSize: window.readoutFont
                        font.weight: Font.Bold
                        font.letterSpacing: 1.6
                    }
                    ComboBox {
                        id: factoryHomeSelector
                        objectName: "factoryHomeSelector"
                        Accessible.name: qsTr("Orchestrator")
                        enabled: window.interactionArmed || popup.visible
                        Layout.fillWidth: true
                        Layout.preferredWidth: 270
                        Layout.maximumWidth: 320
                        Layout.minimumWidth: 160
                        implicitHeight: Math.max(26, window.chromeFont + 12)
                        leftPadding: 0
                        rightPadding: indicator.width + 14
                        indicator: PlainText {
                            x: factoryHomeSelector.width - width - 8
                            y: (factoryHomeSelector.height - height) / 2
                            text: "▾"
                            color: window.mutedTextColor
                            font.pixelSize: window.chromeFont
                        }
                        model: workspace.agents
                        textRole: "title"
                        font.pixelSize: window.chromeFont
                        currentIndex: workspace.agents.indexOf(window.factoryHome)
                        onActivated: function(index) {
                            const chosen = workspace.agents[index]
                            if (chosen)
                                workspace.setHomeAgent(chosen.agentId)
                        }
                        background: Rectangle {
                            radius: window.chromeRadius
                            color: factoryHomeSelector.hovered ? window.hoveredCardColor : window.cardColor
                            border.width: 1
                            border.color: window.borderColor
                        }
                        contentItem: RowLayout {
                            spacing: 6
                            PlainText {
                                objectName: "factoryHomeTitle"
                                leftPadding: 8
                                text: window.factoryHome ? window.factoryHome.title : qsTr("No agent")
                                color: window.textColor
                                font.pixelSize: window.chromeFont
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                            }
                            PlainText {
                                objectName: "factoryHomePin"
                                text: workspace.homeAgentId.length > 0 ? qsTr("pinned")
                                      : window.factoryHome && window.factoryHome.harnessId === "agy" ? qsTr("auto · agy")
                                      : qsTr("auto")
                                color: window.mutedTextColor
                                font.family: window.monoFamily
                                font.pixelSize: window.sidebarReadout
                            }
                        }
                        delegate: ItemDelegate {
                            id: homeOption
                            required property var modelData
                            width: factoryHomeSelector.width
                            text: modelData.title
                            font.pixelSize: window.chromeFont
                            onClicked: {
                                workspace.setHomeAgent(modelData.agentId)
                                factoryHomeSelector.popup.close()
                            }
                            contentItem: PlainText {
                                text: homeOption.text + "  ·  " + homeOption.modelData.harnessId
                                color: window.textColor
                                elide: Text.ElideRight
                            }
                            background: Rectangle {
                                color: homeOption.hovered ? window.hoveredCardColor : window.cardColor
                            }
                        }
                        popup.background: Rectangle {
                            color: window.cardColor
                            border.color: window.borderColor
                        }
                    }
                    PlainText {
                        objectName: "factoryHeaderSummary"
                        Layout.fillWidth: true
                        Layout.preferredWidth: implicitWidth + 4
                        Layout.minimumWidth: 120
                        elide: Text.ElideRight
                        readonly property var summary: window.factoryMode ? workspace.factorySummary : ({})
                        text: qsTr("%1 need you · %2 running · %3 landed · %4 idle")
                              .arg(summary.needsYou || 0).arg(summary.inProgress || 0)
                              .arg(summary.landed || 0).arg(summary.idleCull || 0)
                        color: window.mutedTextColor
                        font.family: window.monoFamily
                        font.pixelSize: window.sidebarReadout
                    }
                    CommandButton {
                        objectName: "exitFactory"
                        text: qsTr("Classic view")
                        implicitHeight: Math.max(26, window.chromeFont + 12)
                        enabled: window.interactionArmed
                        onClicked: window.toggleFactory()
                    }
                }
            }
            // A worker shown from the factory view: one click back.
            Rectangle {
                id: backToFactoryBar
                objectName: "backToFactoryBar"
                visible: window.factoryInspecting
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                color: backHover.hovered ? window.hoveredCardColor : window.surfaceColor
                border.color: backHover.hovered ? window.focusedBorderColor : window.borderColor
                border.width: 1
                radius: window.chromeRadius
                Accessible.role: Accessible.Button
                Accessible.name: backLabel.text
                HoverHandler { id: backHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    enabled: window.interactionArmed
                    onTapped: window.backToFactory()
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 10
                    PlainText {
                        id: backLabel
                        text: qsTr("← Back to Factory (%1)").arg(window.factoryHome ? window.factoryHome.title : qsTr("orchestrator"))
                        color: window.focusedBorderColor
                        font.pixelSize: window.chromeFont
                        font.weight: Font.DemiBold
                    }
                    PlainText {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: workspace.focusedAgent ? workspace.focusedAgent.title : ""
                        color: window.textColor
                        font.pixelSize: window.chromeFont
                    }
                    Rectangle {
                        id: backStagePill
                        objectName: "backToFactoryStage"
                        visible: window.factoryInspected !== null
                        // Not "stage": that names the stage pane in this file.
                        readonly property string stageId: window.factoryInspected ? window.factoryInspected.stage : ""
                        readonly property color accent: window.factoryStageColor(stageId)
                        implicitWidth: backStageText.implicitWidth + 14
                        implicitHeight: 20
                        radius: 10
                        color: Qt.rgba(accent.r, accent.g, accent.b, 0.16)
                        border.color: accent
                        PlainText {
                            id: backStageText
                            anchors.centerIn: parent
                            text: window.factoryStageLabel(backStagePill.stageId)
                            color: window.textColor
                            font.pixelSize: window.sidebarReadout
                            font.weight: Font.DemiBold
                        }
                    }
                    PlainText {
                        visible: text.length > 0
                        text: window.factoryInspected && window.factoryInspected.receipt ? window.factoryInspected.receipt : ""
                        color: window.plentyColor
                        font.family: window.monoFamily
                        font.pixelSize: window.sidebarReadout
                    }
                }
            }

            Rectangle {
                id: stage
                objectName: "focusedPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 120
                // Under a thread the stage is the thread's page, so its inset
                // around the thread is the same surface.
                color: window.chrome.stage
                // The stage edge carries the focus accent only while a terminal
                // shows and owns input. Under a thread (one stage, so the accent
                // marks nothing) it is a hairline in the thread's border colour.
                border.color: liveTerminal.interactive && !window.chrome.thread ? window.chrome.accent
                                                                               : window.chrome.border
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
                    mediaActions: window.fileActions
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
                    pressReturn: () => liveTerminal.pressReturn()
                    typeText: text => liveTerminal.typeText(text)
                    sendKeys: bytes => liveTerminal.sendKeys(bytes)
                    typedScreen: () => liveTerminal.screenLines(true)
                    interrupt: () => liveTerminal.interrupt()
                    // Tab on the empty bar with nothing offered: the next
                    // agent that needs you, for every CLI and with guesses on
                    // or off (the bar's Tab means nothing else); false when
                    // none waits. The terminal's own Tab is the program's
                    // unless its Tab flow is on.
                    tabAway: () => workspace.nextPriorityAttention(window.nextPromptAvailable
                                                                   ? nextPrompt.readyAgents() : ({}))
                    agent: liveTerminal.document !== null ? liveTerminal.document.agentId : ""
                    suggestion: liveTerminal.suggestion
                    suggestionKey: liveTerminal.suggestionKey
                    suggestionAuthor: liveTerminal.suggestionAuthor
                    suggestionConfident: liveTerminal.suggestionConfident
                    working: liveTerminal.document !== null && liveTerminal.document.statusKind === "working"
                    waiting: liveTerminal.document !== null && liveTerminal.document.attentionPending
                    waitingReason: liveTerminal.document !== null ? liveTerminal.document.attentionReason : ""
                    canSend: liveTerminal.document !== null && liveTerminal.document.reachable
                    // Before the CLI saved anything: its name and folder on
                    // the empty thread, and the bar holds what is typed
                    // until the CLI draws its input box.
                    blank: window.visualBlank
                    starting: window.visualBlank && window.visualStarting
                    editor: window.visualSettings.editor || "auto"
                    screen: () => liveTerminal.screenLines()
                    // On another machine, its folder there (never resolved here).
                    folder: window.visualActive && liveTerminal.document
                            ? workspace.visualFolder(liveTerminal.document.agentId) : ""
                    // A remote conversation still loading says so; once its
                    // CLI is up, a conversation not saved there yet too.
                    blankStatus: window.remoteThread.phase === "connecting"
                                 || window.remoteThread.phase === "following"
                                 || (window.remoteThread.phase === "waiting" && !window.visualStarting)
                                 ? (window.remoteThread.status || "") : ""
                    // Attachments for an agent on another machine are copied
                    // there first (Workspace.uploadAttachment).
                    upload: window.remoteMachine.length > 0 && liveTerminal.document
                            ? (path => workspace.uploadAttachment(liveTerminal.document.agentId, path)) : null
                    uploadMachine: window.remoteMachine
                    harness: liveTerminal.document !== null ? liveTerminal.document.harnessId : ""
                    slashCommands: ["claude", "codex", "agy"].indexOf(harness) >= 0
                    // The experiment's record, as the terminal's Tab keeps it:
                    // seen once on screen (here, in the bar), used when Tab
                    // takes it, sent only when a second Tab sends it unchanged.
                    onSuggestionShown: {
                        if (window.nextPromptAvailable && liveTerminal.document && liveTerminal.suggestionKey.length > 0)
                            nextPrompt.seenOffer({session: liveTerminal.document.agentId, offer: liveTerminal.suggestionKey})
                    }
                    onSuggestionFilled: offerKey => {
                        if (window.nextPromptAvailable && liveTerminal.document)
                            nextPrompt.used(liveTerminal.document.agentId, false, 0, offerKey)
                    }
                    onSuggestionSent: offerKey => {
                        if (window.nextPromptAvailable && liveTerminal.document)
                            nextPrompt.used(liveTerminal.document.agentId, true, 0, offerKey)
                    }
                    // The same picker for every CLI; each CLI's own command
                    // makes the change, typed into its session.
                    models: window.visualActive ? window.visualModels(liveTerminal.document) : []
                    efforts: window.visualActive ? window.visualEfforts(liveTerminal.document) : []
                    onModelChosen: function(id, family, effort, label) {
                        window.visualCommand("model", id, family, effort, label)
                    }
                    onEffortChosen: function(level) { window.visualCommand("effort", level, "", "", "") }
                    onPickerOpened: workspace.refreshModels()
                    onCommandSent: window.watchVisualDialog(false)
                    onDialogFound: window.watchVisualDialog(true)
                    onTerminalNeeded: window.toggleVisual()
                    onDismissed: window.toggleVisual()
                    onEditRequested: function(record, text) { window.forkVisual(record, true, text) }
                    openLink: link => window.openFactoryLink(link)
                    // Room above the orchestrator's composer for the gate strip.
                    composerInset: homeAttentionInbox.wanted ? Math.round(window.chromeFont * 2.4) + 6 : 0
                }
                // Requests other agents wait on, right above the orchestrator's
                // composer; a chip shows that agent.
                Rectangle {
                    id: homeAttentionInbox
                    objectName: "homeAttentionInbox"
                    readonly property var entries: window.factoryHomeShown ? workspace.attentionInbox : []
                    readonly property bool wanted: window.factoryHomeShown && entries.length > 0
                    parent: visualView.accessory
                    anchors.fill: parent
                    visible: wanted && window.visualCovers
                    radius: window.chromeRadius + 4
                    color: Qt.rgba(window.attentionColor.r, window.attentionColor.g, window.attentionColor.b, 0.1)
                    border.width: 1
                    border.color: window.attentionColor
                    clip: true
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 6
                        spacing: 8
                        PlainText {
                            text: "◆"
                            color: window.attentionColor
                            font.pixelSize: window.readoutFont
                        }
                        PlainText {
                            text: qsTr("%n gate(s) waiting", "", homeAttentionInbox.entries.length)
                            color: window.textColor
                            font.pixelSize: window.readoutFont
                            font.weight: Font.DemiBold
                        }
                        Repeater {
                            model: homeAttentionInbox.entries.slice(0, 3)
                            delegate: Rectangle {
                                id: gateChip
                                required property var modelData
                                objectName: "homeAttentionChip_" + modelData.agentId
                                Layout.maximumWidth: 260
                                implicitWidth: gateText.implicitWidth + 16
                                implicitHeight: Math.round(window.readoutFont * 1.9)
                                Layout.minimumWidth: 60
                                radius: height / 2
                                color: gateHover.hovered ? window.hoveredCardColor : window.cardColor
                                border.color: gateHover.hovered ? window.attentionColor : window.borderColor
                                PlainText {
                                    id: gateText
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    text: gateChip.modelData.title
                                          + (gateChip.modelData.reason ? " · " + gateChip.modelData.reason : "")
                                    color: window.textColor
                                    font.pixelSize: window.sidebarReadout
                                }
                                HoverHandler { id: gateHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    enabled: window.interactionArmed
                                    onTapped: window.openFactoryTerminal(gateChip.modelData.agentId)
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
                Binding {
                    target: workspace.transcript
                    property: "shown"
                    // Nothing is read or watched while no agent shows as a
                    // thread; an agent with no file yet shows an empty one.
                    // `started`: the model and effort it runs before its
                    // file says (Workspace.agentModel), again when the
                    // CLIs' lists change.
                    value: window.visualActive
                           ? ({ agent: workspace.focusedAgent.agentId,
                                harness: workspace.focusedAgent.harnessId,
                                path: window.visualFile,
                                started: workspace.modelsRevision >= 0
                                         ? workspace.agentModel(workspace.focusedAgent.agentId) : ({}) })
                           : ({})
                }

                // Why the terminal is showing in place of the thread.
                Rectangle {
                    objectName: "visualDialogNote"
                    z: 4
                    visible: (window.visualDialog || window.visualStartScreen) && window.visualActive
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
                        text: window.visualStartScreen && !window.visualDialog
                              ? qsTr("The agent's own screen · answer it here; the thread returns at its input box")
                              : qsTr("The agent's own dialog · the thread returns when it closes")
                        color: window.mutedTextColor
                        font.pixelSize: 12
                    }
                }
                // A remote agent's thread: behind while its connection is
                // back on the way (a cached copy shows), or why the terminal
                // shows instead of a thread that cannot load.
                Rectangle {
                    objectName: "remoteThreadNote"
                    readonly property bool behind: window.visualCovers && window.remoteThread.behind === true
                    readonly property bool missing: window.visualWanted && !window.visualActive
                                                    && (window.remoteThread.reason || "").length > 0
                    z: 4
                    visible: behind || missing
                    x: liveTerminal.x + (liveTerminal.width - width) / 2
                    y: liveTerminal.y + 8
                    width: Math.min(remoteThreadText.implicitWidth + 20, liveTerminal.width - 20)
                    height: remoteThreadText.implicitHeight + 10
                    radius: 6
                    color: window.cardColor
                    border.color: window.borderColor
                    Text {
                        id: remoteThreadText
                        anchors.centerIn: parent
                        width: Math.min(implicitWidth, liveTerminal.width - 40)
                        elide: Text.ElideRight
                        text: parent.behind
                              ? qsTr("Reconnecting to %1 · the thread may be behind").arg(window.remoteMachine)
                              : qsTr("No thread: %1 · showing the terminal").arg(window.remoteThread.reason || "")
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
                    document: workspace.focusedAgent
                    fontFamily: config.terminalFontFamily
                    fontPixelSize: window.terminalFontSize
                    scrollToBottomOnTyping: config.scrollToBottomOnTyping
                    visible: document !== null
                    enabled: visible
                    // A history page stays interactive for the wheel, selection and
                    // typing back to live; input reaches the agent only when live.
                    interactive: visible && window.visible && !window.inputBlocked && document !== null
                                 && !window.visualCovers
                                 && (app.active || document.inputReady || document.historyActive)
                    focus: visible && window.visible && !window.inputBlocked && !window.sideTerminalOpen
                           && !window.visualCovers
                    // The next prompt lapis predicted for the agent, while it
                    // waits for a prompt (never over a request): Tab types it
                    // for you to edit, Tab again sends it, and Tab with nothing
                    // offered moves to the next agent that needs you.
                    readonly property bool predicting: window.nextPromptAvailable
                                                       && nextPrompt.enabled && document !== null
                    tabFlow: predicting && nextPrompt.guesses(document.harnessId)
                    suggestion: predicting && nextPrompt.revision >= 0 && !document.historyActive
                                && ["finished", "idle"].indexOf(document.statusKind) >= 0
                                ? nextPrompt.suggestion(document.agentId) : ""
                    suggestionConfident: predicting && nextPrompt.revision >= 0 && document !== null
                                         && nextPrompt.confidence(document.agentId) >=
                                         nextPrompt.confidentThreshold
                    onSuggestionSeen: (agentId, offerKey) => {
                        if (window.nextPromptAvailable)
                            nextPrompt.seenOffer({session: agentId, offer: offerKey})
                    }
                    onSuggestionUsed: (agentId, offerKey, sent, typedFirst) => {
                        if (window.nextPromptAvailable)
                            nextPrompt.used(agentId, sent, typedFirst, offerKey)
                    }
                    // Follows every offer, even one with the same words as the last.
                    suggestionKey: suggestion.length > 0 && nextPrompt.revision >= 0
                                   ? nextPrompt.offerKey(document.agentId) : ""
                    // Who wrote the guess, shown beside it; empty when unknown.
                    suggestionAuthor: suggestion.length > 0 && nextPrompt.revision >= 0
                                      ? nextPrompt.author(document.agentId) : ""
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
                            color: Qt.alpha(window.windowPalette.text, historyScrubber.engaged ? 0.14 : 0.07)
                            Behavior on width {
                                enabled: window.motionEnabled
                                NumberAnimation { duration: window.motionDuration; easing.type: Easing.OutCubic }
                            }
                            ColorEase on color {}
                            Rectangle {
                                id: historyThumb
                                objectName: "historyThumb"
                                width: parent.width
                                radius: width / 2
                                height: historyScrubber.thumbHeight
                                y: scrubArea.pressed ? historyScrubber.dragged(scrubArea.mouseY)
                                                     : (historyScrubber.session ? historyScrubber.session.historyPosition : 1)
                                                       * historyScrubber.travel
                                color: Qt.alpha(window.windowPalette.accent,
                                                scrubArea.pressed ? 0.9 : historyScrubber.engaged ? 0.7 : 0.45)
                                ColorEase on color {}
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
                                    app.deferTerminalFocus()
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
                    readonly property var session: workspace.focusedAgent
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
                                app.deferTerminalFocus()
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
                    readonly property var session: workspace.focusedAgent
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
                    visible: workspace.focusedAgent === null
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
                    enabled: workspace.focusedAgent !== null && window.interactionArmed
                    onDropped: function(drop) {
                        if (!drop.hasUrls)
                            return
                        const text = window.quoteDroppedPaths(drop.urls)
                        if (text.length === 0)
                            return
                        const destinationId = workspace.focusedAgent !== null ?
                                                  workspace.focusedAgent.agentId : ""
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
                model: visible ? workspace.categoryAgents : []
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
                        index = Math.max(0, Math.min(workspace.categoryAgents.length,
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
                    readonly property bool marked: workspace.focusedAgent !== null
                                                   && workspace.focusedAgent.agentId === modelData.agentId
                    readonly property bool picked: window.isAgentSelected(modelData.agentId)
                    objectName: "agentTab_" + modelData.agentId
                    opacity: window.dragSource === "strip" && window.draggedAgents.indexOf(modelData.agentId) >= 0 ?
                                 0.45 : 1
                    width: agentTabs.cardWidth
                    height: agentTabs.height
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.agentName + ", " + window.agentTabTitle(modelData) + ", " + modelData.statusLabel
                    // Assistive activation selects the agent, as a click does.
                    Accessible.onPressAction: {
                        if (window.interactionArmed && workspace.selectAgent(agentTab.modelData.agentId))
                            app.deferTerminalFocus()
                    }

                    Rectangle {
                        anchors.fill: parent
                        // The cards' frames follow the chrome; the previews
                        // inside keep their terminals' colours.
                        color: agentTab.marked ? window.chrome.press : window.chrome.stage
                        radius: window.chromeRadius
                        border.width: agentTab.marked || agentTab.picked ? 2 : 1
                        border.color: agentTab.marked || agentTab.picked ? window.chrome.accent :
                                      agentHover.hovered ? window.chrome.muted : window.chrome.border
                        ColorEase on color {}
                        ColorEase on border.color {}
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
                            visible: !workspace.fixtureMode
                            ink: agentTab.marked ? window.chrome.text : window.chrome.muted
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                        }
                        StatusMark {
                            objectName: "statusMark_" + agentTab.modelData.agentId
                            kind: agentTab.modelData.statusKind
                            size: window.statusMarkSize
                            Layout.preferredWidth: size
                            Layout.preferredHeight: size
                            Layout.alignment: Qt.AlignVCenter
                        }
                        PlainText {
                            text: window.agentTabTitle(agentTab.modelData)
                            color: agentTab.marked ? window.chrome.text : window.chrome.muted
                            font.family: window.monoFamily
                            font.pixelSize: window.readoutFont
                            font.weight: agentTab.marked ? Font.DemiBold : Font.Normal
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            verticalAlignment: Text.AlignVCenter
                        }
                        PlainText {
                            objectName: "statusLabel_" + agentTab.modelData.agentId
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
                        objectName: "previewTerminal_" + agentTab.modelData.agentId
                        x: 6
                        y: agentHeader.y + agentHeader.height + 4
                        width: parent.width - 12
                        height: parent.height - y - 6
                        document: agentTab.modelData
                        fontFamily: config.terminalFontFamily
                        fontPixelSize: window.terminalFontSize
                        interactive: false
                        enabled: false
                        frameInterval: 250
                        minimumScale: 0.5
                    }
                    // Picked for a drag with Command-click or Shift-click.
                    Rectangle {
                        objectName: "pickedCue_" + agentTab.modelData.agentId
                        anchors.fill: parent
                        visible: agentTab.picked && !agentTab.marked
                        radius: window.chromeRadius
                        color: Qt.alpha(window.chrome.accent, 0.08)
                    }
                    HoverHandler { id: agentHover }
                    DragOrClick {
                        objectName: "cardPress_" + agentTab.modelData.agentId
                        anchors.fill: parent
                        enabled: window.interactionArmed
                        onTapped: function(modifiers) { window.clickAgent(agentTab.modelData.agentId, modifiers) }
                        onDragStarted: function(scene) {
                            window.beginAgentDrag(window.agentsToDrag(agentTab.modelData.agentId), "strip", scene)
                        }
                        onDragMoved: function(scene) { window.moveDragGhost(scene) }
                        onDragEnded: window.endDrag()
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        enabled: window.interactionArmed
                        onTapped: {
                            if (workspace.selectAgent(agentTab.modelData.agentId))
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
                        tones: window.chrome
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

        // The factory view's pipeline: about 40% of the room beside the
        // sidebar, or a narrow column of counts when collapsed.
        FactoryBoard {
            id: factoryBoard
            readonly property real room: window.width - 16
                                         - (categoryRail.visible ? categoryRail.width + 8 : 0)
            visible: window.factoryHomeShown
            theme: window
            // Read only in the factory view, so the classic view never
            // rebuilds hidden cards on each status change.
            items: window.factoryMode ? workspace.factoryPipeline : []
            summary: window.factoryMode ? workspace.factorySummary : ({})
            collapsed: window.factoryBoardCollapsed
            interactive: window.interactionArmed
            Layout.fillHeight: true
            Layout.preferredWidth: collapsed ? 46 : Math.max(280, Math.round(room * 0.4))
            Layout.maximumWidth: Layout.preferredWidth
            onCollapseToggled: window.factoryBoardCollapsed = !window.factoryBoardCollapsed
            onInspectRequested: id => window.inspectFactoryAgent(id)
            onOpenTerminalRequested: id => window.openFactoryTerminal(id)
            onDetailsRequested: id => window.factoryDetails(id)
            onCloseRequested: (id, safeToClose) => window.closeFactoryAgent(id, safeToClose)
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
                fontFamily: config.terminalFontFamily
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
                app.deferTerminalFocus()
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
