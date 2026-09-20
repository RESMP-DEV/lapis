pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lapis 1.0

ApplicationWindow {
    id: window

    component PreviewLabel: Label {
        color: window.mutedTextColor
        renderType: Text.QtRendering
    }

    component PreviewMenuItem: MenuItem {
        id: menuItem
        required property string explanation
        focusPolicy: Qt.NoFocus
        leftPadding: 26
        Accessible.description: explanation
        indicator: Label {
            x: 8
            y: (menuItem.height - height) / 2
            text: "✓"
            color: window.textColor
            visible: menuItem.checked
        }
        contentItem: Column {
            spacing: 3
            Label {
                width: parent.width
                text: menuItem.text
                color: window.textColor
                font.pixelSize: 12
            }
            Label {
                width: parent.width
                text: menuItem.explanation
                color: window.mutedTextColor
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
        background: Rectangle {
            color: menuItem.highlighted ? window.hoveredCardColor : "transparent"
            radius: 4
        }
    }

    width: 1400
    height: 960
    minimumWidth: 980
    minimumHeight: 700
    objectName: "workspaceWindow"

    // Appearance comes from lapis.json. The fallbacks keep an isolated QML
    // preview, which has no config, rendering the default Lapis scheme.
    readonly property var appearance: typeof keymap !== "undefined" && keymap !== null ?
                                           keymap.themes.find(t => t.name === keymap.themeName) : null
    readonly property color backgroundColor: appearance ? appearance.background : "#0b101a"
    readonly property color surfaceColor: appearance ? appearance.surface : "#111927"
    readonly property color cardColor: appearance ? appearance.card : "#151c29"
    readonly property color hoveredCardColor: appearance ? appearance.hoveredCard : "#1a2333"
    readonly property color focusedColor: appearance ? appearance.focused : "#1f2838"
    readonly property color borderColor: appearance ? appearance.border : "#2b3546"
    readonly property color focusedBorderColor: appearance ? appearance.focusedBorder : "#6a76e8"
    readonly property color textColor: appearance ? appearance.text : "#f2f3ea"
    readonly property color mutedTextColor: appearance ? appearance.mutedText : "#98a0ae"
    readonly property color attentionColor: appearance ? appearance.attention : "#ff5f56"
    readonly property color attentionTextColor: appearance ? appearance.attentionText : "#fff4f2"
    // The pane sits one shade off the window background so the terminal reads as
    // a distinct surface in every scheme.
    readonly property color paneColor: appearance ? appearance.surface : "#0d131d"
    color: backgroundColor
    title: "lapis"
    readonly property alias cueAnimationEnabled: cueRules.animationEnabled

    // Layout and density come from lapis.json. The helpers keep the geometry
    // expressions readable and fall back to the historical focus layout when no
    // config is present, which is the isolated QML preview case.
    readonly property string layoutMode: typeof keymap !== "undefined" && keymap !== null ?
                                             keymap.layoutName : "focus"
    readonly property string densityMode: typeof keymap !== "undefined" && keymap !== null ?
                                              keymap.densityName : "comfortable"
    readonly property bool blocksLayout: layoutMode === "blocks"
    // Columns puts the preview strip in a left channel beside a narrower pane.
    readonly property bool columnsLayout: layoutMode === "columns"
    // Stack shows one session at a time with the carousel hidden.
    readonly property bool stackLayout: layoutMode === "stack"
    // The pane owns the keyboard only when it is the visible surface.
    readonly property bool settingsOpen: settingsDialog.visible
    readonly property bool paneVisible: layoutMode === "focus" || columnsLayout
    readonly property int cardSpacing: densityMode === "minimal" ? 8 :
                                       densityMode === "compact" ? 10 : 14
    readonly property int cardHeight: densityMode === "minimal" ? 104 :
                                      densityMode === "compact" ? 128 : 156
    readonly property int paneRadius: densityMode === "minimal" ? 6 : 12

    // Shortcuts come from lapis.json through the `keymap` context property. The
    // literals below are the fallback used only when no keymap is supplied, such
    // as in an isolated QML preview that does not load a config file.
    function bindings(action, fallback) {
        if (typeof keymap !== "undefined" && keymap !== null) {
            const configured = keymap.shortcutBindings[action]
            if (configured && configured.length > 0)
                return configured
        }
        return fallback
    }

    // Cmd-W hides the window and leaves the service-owned session running, which
    // is the documented detach behavior. It must not quit the app: Cmd-W is the
    // key macOS users press to move between windows, and quitting also kills the
    // input path while leaving an orphaned session behind. Quit is explicit.
    Shortcut {
        sequences: window.bindings("quit", ["Ctrl+Q"])
        onActivated: Qt.quit()
    }
    Shortcut {
        sequences: window.bindings("detachWindow", ["Ctrl+W"])
        onActivated: window.hide()
    }

    // Category navigation: one key per category, exactly like the terminal
    // setups this mirrors. Categories are listed in lapis.json.
    Shortcut {
        objectName: "nextCategoryShortcut"
        sequences: window.bindings("nextCategory", ["Ctrl+Tab"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.nextCategory()
    }
    Shortcut {
        sequences: window.bindings("previousCategory", ["Ctrl+Shift+Tab"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.nextCategory(-1)
    }
    Shortcut {
        sequences: window.bindings("category1", ["Ctrl+1"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.focusCategory(0)
    }
    Shortcut {
        sequences: window.bindings("category2", ["Ctrl+2"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.focusCategory(1)
    }
    Shortcut {
        sequences: window.bindings("category3", ["Ctrl+3"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.focusCategory(2)
    }
    Shortcut {
        sequences: window.bindings("category4", ["Ctrl+4"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.focusCategory(3)
    }

    // Window navigation inside the current category. Holding the chord repeats,
    // because Qt auto-repeats an activated Shortcut while the keys are held.
    Shortcut {
        sequences: window.bindings("nextWindow", ["Ctrl+Shift+]"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.nextWindow()
    }
    Shortcut {
        sequences: window.bindings("previousWindow", ["Ctrl+Shift+["])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.nextWindow(-1)
    }
    Shortcut {
        sequences: window.bindings("focusLeft", ["Ctrl+Left"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.nextWindow(-1)
    }
    Shortcut {
        sequences: window.bindings("focusRight", ["Ctrl+Right"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: workspace.nextWindow()
    }

    Shortcut {
        sequences: window.bindings("cycleLayout", ["Ctrl+L"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.toggleLayout()
        }
    }
    Shortcut {
        sequences: window.bindings("reloadConfig", ["Ctrl+R"])
        context: Qt.WindowShortcut
        enabled: !window.settingsOpen
        onActivated: {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.reload()
        }
    }

    // The configured settings shortcut opens Appearance. The terminal
    // surface is a native focus item that consumes key events before a QML
    // Shortcut can fire, so C++ intercepts the key and calls this instead.
    function openSettingsDialog() {
        settingsDialog.open()
        // The dialog holds the keyboard while open, so the terminal never
        // receives the keystrokes meant for the appearance options.
        if (settingsDialog.opened)
            settingsDialog.forceActiveFocus()
    }

    Settings {
        id: settingsDialog

        themeModel: typeof keymap !== "undefined" && keymap !== null ? keymap.themes : []
        layoutModel: window.layoutChoices
        densityModel: window.densityChoices
        currentTheme: typeof keymap !== "undefined" && keymap !== null ? keymap.themeName : "lapis"
        currentLayout: window.layoutMode
        currentDensity: window.densityMode
        configPath: typeof keymap !== "undefined" && keymap !== null ? keymap.sourcePath : ""
        shortcutHint: preview.settingsShortcuts.join(" / ")
        configDiagnostic: typeof keymap !== "undefined" && keymap !== null ? keymap.diagnostic : ""

        onClosed: preview.deferTerminalFocus()

        onThemeChosen: function(name) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setTheme(name)
        }
        onLayoutChosen: function(name) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setLayout(name)
        }
        onDensityChosen: function(name) {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.setDensity(name)
        }
    }

    // The isolated QML preview has no keymap, so these literals keep the dialog
    // usable there and double as the documented set of valid names.
    readonly property var layoutChoices: typeof keymap !== "undefined" && keymap !== null ?
                                             keymap.layouts : ["focus", "columns", "blocks", "stack"]
    readonly property var densityChoices: typeof keymap !== "undefined" && keymap !== null ?
                                              keymap.densities : ["comfortable", "compact", "minimal"]

    // Keyboard ownership lives in C++ so it can be asserted directly rather
    // than inferred from QML property state. Both entry points call it, and a
    // layout switch re-runs it so focus never lands on a hidden pane.
    onActiveChanged: if (active) keyboardOwnershipReady()
    onLayoutModeChanged: keyboardOwnershipReady()

    signal keyboardOwnershipReady()

    QtObject {
        id: cueRules

        readonly property bool visualActive: window.active && window.visible
        readonly property bool animationEnabled: !preview.reducedMotion && visualActive

        onAnimationEnabledChanged: {
            if (animationEnabled)
                window.resumeDecorativeAnimation();
            else if (preview.reducedMotion)
                window.resetDecorativeAnimation();
            else if (!visualActive)
                window.pauseDecorativeAnimation();
        }

        onVisualActiveChanged: {
            if (!visualActive)
                window.pauseDecorativeAnimation();
        }
    }

    Connections {
        target: preview

        function onReducedMotionChanged() {
            // An inactive window already has animationEnabled == false, so its
            // change handler cannot reset a previously paused pulse.
            if (preview.reducedMotion)
                window.resetDecorativeAnimation();
        }
    }

    function pauseDecorativeAnimation() {
        if (!carousel)
            return;
        const count = carousel.count;
        for (let index = 0; index < count; ++index) {
            const card = carousel.itemAtIndex(index);
            if (card && typeof card.pauseCue === "function")
                card.pauseCue();
        }
    }

    function resetDecorativeAnimation() {
        if (!carousel)
            return;
        const count = carousel.count;
        for (let index = 0; index < count; ++index) {
            const card = carousel.itemAtIndex(index);
            if (card && typeof card.resetCue === "function")
                card.resetCue();
        }
    }

    function resumeDecorativeAnimation() {
        if (!carousel)
            return;
        const count = carousel.count;
        for (let index = 0; index < count; ++index) {
            const card = carousel.itemAtIndex(index);
            if (card && typeof card.resumeCue === "function")
                card.resumeCue();
        }
    }

    function replayAttention(scenario) {
        workspace.replayAttention(scenario);
    }

    // Every layout shares this grid. The column count sets the flow direction
    // and the pane and carousel declare their own geometry, so switching layout
    // never rebuilds items and never reassigns a Shortcut.
    GridLayout {
        id: workspaceGrid
        anchors.fill: parent
        anchors.margins: 12
        anchors.topMargin: 6
        columnSpacing: 8
        rowSpacing: 8
        // Focus and blocks stack the pane above the strip; columns puts the
        // strip beside the pane; stack has no pane, so the carousel is the only
        // row and fills the window.
        columns: window.columnsLayout ? 2 : 1
        rows: window.columnsLayout ? 2 : 3
        flow: GridLayout.LeftToRight
        // Row 0 is the header, row 1 holds the pane or the channel, row 2 is the
        // preview strip. Children declare their own stretch, because the
        // stretch properties are attached and cannot be set on the layout.

        RowLayout {
            Layout.fillWidth: true
            Layout.columnSpan: window.columnsLayout ? 2 : 1
            Layout.row: 0
            Layout.column: 0
            Layout.maximumWidth: window.columnsLayout ? 320 : -1
            Layout.minimumHeight: 18
            Layout.maximumHeight: 18
            spacing: 6

            PreviewLabel {
                Layout.fillWidth: true
                text: preview.active ? qsTr("Sample sessions") :
                      workspace.focusedSession ? workspace.focusedSession.activity : qsTr("Disconnected")
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            Button {
                id: sessionTools
                visible: !preview.active
                Layout.preferredHeight: 18
                focusPolicy: Qt.NoFocus
                font.pixelSize: 10
                topPadding: 0
                bottomPadding: 0
                text: qsTr("Session")
                onClicked: sessionMenu.open()
                Menu {
                    id: sessionMenu
                    readonly property bool available: workspace.focusedSession &&
                        workspace.focusedSession.connectionState !== "connecting" &&
                        workspace.focusedSession.connectionState !== "synchronizing" &&
                        !workspace.focusedSession.inputReady
                    MenuItem {
                        text: qsTr("Reconnect")
                        enabled: sessionMenu.available
                        onTriggered: workspace.focusedSession.reconnect()
                    }
                    MenuItem {
                        text: qsTr("Discover existing session")
                        enabled: sessionMenu.available
                        onTriggered: workspace.focusedSession.discoverSession()
                    }
                    MenuItem {
                        text: qsTr("Start new session")
                        enabled: sessionMenu.available
                        onTriggered: workspace.focusedSession.startNewSession()
                    }
                }
            }

            Button {
                id: previewTools
                visible: preview.active
                Layout.preferredHeight: 18
                focusPolicy: Qt.NoFocus
                font.pixelSize: 10
                topPadding: 0
                bottomPadding: 0
                text: preview.diagnostics.length ? qsTr("Preview tools · error") : qsTr("Preview tools")
                background: Rectangle {
                    radius: 4
                    color: parent.hovered ? window.hoveredCardColor : window.cardColor
                    border.color: window.borderColor
                }
                ToolTip.visible: hovered
                ToolTip.text: preview.diagnostics.length ? preview.diagnostics :
                              qsTr("Try sample alerts and apply interface edits.")
                Accessible.description: ToolTip.text
                onClicked: previewMenu.open()

                Menu {
                    id: previewMenu
                    x: previewTools.width - width
                    y: previewTools.height + 4
                    width: 310
                    padding: 6
                    background: Rectangle {
                        radius: 7
                        color: window.cardColor
                        border.color: window.borderColor
                    }

                    PreviewMenuItem {
                        text: qsTr("Show one alert")
                        explanation: qsTr("Highlight the third terminal. Clear it to replay.")
                        onTriggered: window.replayAttention("arrival")
                    }
                    PreviewMenuItem {
                        text: qsTr("Show two alerts")
                        explanation: qsTr("Highlight the second and third terminals.")
                        onTriggered: window.replayAttention("two")
                    }
                    PreviewMenuItem {
                        text: qsTr("Clear one alert")
                        explanation: qsTr("Clear the third terminal; leave other alerts.")
                        onTriggered: window.replayAttention("resolve")
                    }
                    PreviewMenuItem {
                        text: qsTr("Clear all alerts")
                        explanation: qsTr("Remove every alert so you can try again.")
                        onTriggered: window.replayAttention("reset")
                    }
                    MenuSeparator {}
                    PreviewMenuItem {
                        text: qsTr("Repeat the same alert")
                        explanation: qsTr("Show the third terminal's alert again. An existing alert won't pulse again.")
                        onTriggered: window.replayAttention("duplicate")
                    }
                    PreviewMenuItem {
                        text: qsTr("Disable animations")
                        explanation: preview.systemReducedMotion ?
                                     qsTr("Enabled by your macOS Reduce Motion setting.") :
                                     qsTr("Keep alert outlines steady instead of pulsing.")
                        checkable: true
                        checked: preview.reducedMotion
                        enabled: !preview.systemReducedMotion
                        onTriggered: preview.reducedMotion = checked
                    }
                    MenuSeparator {}
                    PreviewMenuItem {
                        text: qsTr("Layout: %1").arg(window.layoutMode)
                        explanation: qsTr("Switch between one large pane and equal blocks. Ctrl-L also toggles this.")
                        onTriggered: if (typeof keymap !== "undefined" && keymap !== null)
                                         keymap.toggleLayout()
                    }
                    PreviewMenuItem {
                        text: qsTr("Reload keybindings")
                        explanation: qsTr("Apply edits from lapis.json. Ctrl-R also reloads.")
                        onTriggered: if (typeof keymap !== "undefined" && keymap !== null)
                                         keymap.reload()
                    }
                    MenuSeparator {}
                    PreviewMenuItem {
                        text: qsTr("Reload interface")
                        explanation: qsTr("Apply saved layout edits to this preview.")
                        onTriggered: preview.reload()
                    }
                }
            }
        }

        Rectangle {
            id: focusedPane

            objectName: "focusedPane"

            // Blocks layout gives every session an equal tile, so the single
            // large pane collapses and the strip below becomes the workspace.
            // Focus layout keeps the current large pane plus a preview strip.
            // Focus and blocks stack the pane above the strip, so the pane takes
            // the free height. Columns puts the pane beside the channel, where it
            // fills the height and claims a width share instead.
            Layout.fillWidth: true
            Layout.fillHeight: window.paneVisible
            Layout.preferredWidth: window.columnsLayout ? Math.max(420, window.width * 0.62) : -1
            Layout.preferredHeight: window.columnsLayout ? -1 :
                                                           window.paneVisible ? -1 : 0
            // The channel is declared after the pane, so pin the pane to column 1
            // in columns mode and let the carousel occupy column 0 to its left.
            Layout.column: window.columnsLayout ? 1 : 0
            // Keep a valid row span; visibility and zero preferred height
            // remove the inactive pane from the layout.
            Layout.row: 1
            Layout.rowSpan: 1
            visible: window.paneVisible
            color: window.paneColor
            radius: window.paneRadius
            border.color: window.focusedBorderColor
            border.width: 1

            RowLayout {
                id: historyBar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                height: 32
                spacing: 8
                property var session: workspace.focusedSession
                visible: session && session.live
                Button {
                    text: qsTr("Older")
                    enabled: historyBar.session && !historyBar.session.historyRequestPending
                    onClicked: historyBar.session.olderHistory()
                }
                Button {
                    text: qsTr("Newer")
                    enabled: historyBar.session && historyBar.session.historyActive &&
                             !historyBar.session.historyRequestPending
                    onClicked: historyBar.session.newerHistory()
                }
                Button {
                    text: qsTr("Live")
                    enabled: historyBar.session && historyBar.session.historyActive
                    onClicked: {
                        historyBar.session.returnToLive()
                        liveTerminal.forceActiveFocus()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: window.mutedTextColor
                    text: !historyBar.session ? "" :
                          historyBar.session.historyRequestPending ? qsTr("Loading history…") :
                          historyBar.session.historyActive ?
                          qsTr("Read only · ") + historyBar.session.historyMessage : qsTr("Live")
                }
            }

            TerminalSurface {
                anchors.fill: parent
                id: liveTerminal
                objectName: "liveTerminal"
                anchors.margins: 18
                anchors.topMargin: historyBar.visible ? 54 : 18
                document: workspace.focusedSession
// Input requires both the session being ready and this pane owning
                // the keyboard, which blocks layout gives to a tile instead.
                interactive: (preview.active || (document && document.inputReady))
                             && window.paneVisible && !window.settingsOpen
                focus: window.paneVisible && !window.settingsOpen
                Component.onCompleted: if (window.paneVisible) forceActiveFocus()
                // Claim the keyboard whenever this pane becomes the active
                // surface, so switching layout never leaves focus on a button.
                Connections {
                    target: window
                    function onLayoutModeChanged() {
                        preview.deferTerminalFocus()
                    }
                }
            }
        }

        // One retained delegate serves the strip, channel, wrapping grid and
        // full-window stack. Grid cell dimensions determine each scroll axis.
        GridView {
            id: carousel
            objectName: "sessionCarousel"
            Layout.fillWidth: true
            Layout.fillHeight: window.columnsLayout || window.blocksLayout || window.stackLayout
            Layout.preferredWidth: window.columnsLayout ? 320 : -1
            Layout.maximumWidth: window.columnsLayout ? 320 : -1
            Layout.preferredHeight: window.paneVisible && !window.columnsLayout ? window.cardHeight + 24 : -1
            Layout.column: 0
            Layout.row: window.columnsLayout || !window.paneVisible ? 1 : 2
            Layout.alignment: Qt.AlignTop | Qt.AlignLeft
            Layout.minimumHeight: window.paneVisible && !window.columnsLayout ? window.cardHeight + 16 : 0
            flow: window.layoutMode === "focus" ? GridView.FlowTopToBottom : GridView.FlowLeftToRight
            boundsMovement: Flickable.StopAtBounds
            clip: true
            model: workspace.sessions
            currentIndex: workspace.focusedIndex
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, GridView.Contain)
            onCurrentItemChanged: preview.deferTerminalFocus()
            onCellWidthChanged: Qt.callLater(function() { carousel.positionViewAtIndex(carousel.currentIndex, GridView.Contain) })
            onCellHeightChanged: Qt.callLater(function() { carousel.positionViewAtIndex(carousel.currentIndex, GridView.Contain) })

            readonly property int blockColumns: Math.max(1, Math.min(4, workspace.sessions.length, Math.floor(width / 220)))
            readonly property int occupiedRows: Math.max(1, Math.ceil(workspace.sessions.length / blockColumns))
            cellWidth: window.blocksLayout ? Math.floor(width / blockColumns) :
                       window.columnsLayout || window.stackLayout ? width : 238 + window.cardSpacing
            cellHeight: window.blocksLayout ? Math.max(150, Math.floor(height / occupiedRows)) :
                        window.stackLayout ? height :
                        window.columnsLayout ? window.cardHeight + window.cardSpacing : height
            readonly property real tileWidth: Math.max(1, cellWidth - window.cardSpacing)
            readonly property real tileHeight: Math.max(1, cellHeight - window.cardSpacing)

            ScrollBar.horizontal: ScrollBar {
                visible: carousel.flow === GridView.FlowTopToBottom && carousel.contentWidth > carousel.width
                implicitHeight: 8
                contentItem: Rectangle { radius: 4; color: window.borderColor }
            }
            ScrollBar.vertical: ScrollBar {
                visible: carousel.flow === GridView.FlowLeftToRight && carousel.contentHeight > carousel.height
                implicitWidth: 8
                contentItem: Rectangle { radius: 4; color: window.borderColor }
            }

            delegate: Button {
                id: sessionCard

                required property int index
                required property var modelData

                // Keep the rendered card inside its grid cell and gutter.
                width: carousel.tileWidth
                height: carousel.tileHeight
                LayoutMirroring.enabled: false
                implicitWidth: carousel.tileWidth
                implicitHeight: carousel.tileHeight
                objectName: "sessionCard_" + sessionCard.modelData.sessionId
                padding: 0
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Terminal %1").arg(sessionCard.index + 1)
                Accessible.description: sessionCard.modelData.live ? qsTr("Live shell") : qsTr("Sample session")
                readonly property bool pending: sessionCard.modelData.attentionPending
                readonly property real cueLevel: cue.level
                readonly property bool cueRunning: cue.running && !cue.paused

                function pauseCue() {
                    if (!cue.running || cue.paused)
                        return;
                    cue.pause();
                }

                function resumeCue() {
                    if (cue.paused)
                        cue.resume();
                }

                function resetCue() {
                    cue.stop();
                    cue.level = 0;
                }

                onPendingChanged: {
                    if (!pending)
                        resetCue();
                }

                Connections {
                    target: sessionCard.modelData

                    function onAttentionArrived() {
                        attentionRim.beginCue();
                    }
                }

                onClicked: {
                    workspace.focusedIndex = sessionCard.index
                    if (!window.paneVisible)
                        cardTerminal.forceActiveFocus()
                    else
                        liveTerminal.forceActiveFocus()
                }
                transform: Translate {
                    y: sessionCard.hovered ? -1 : 0
                    Behavior on y {
                        enabled: cueRules.animationEnabled
                        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                    }
                }

                background: Item {
                    Rectangle {
                        anchors.fill: parent
                        radius: 10
                        color: workspace.focusedIndex === sessionCard.index ? window.focusedColor :
                                sessionCard.hovered ? window.hoveredCardColor : window.cardColor
                        border.color: sessionCard.pending ? window.attentionColor :
                                       workspace.focusedIndex === sessionCard.index ? window.focusedBorderColor : window.borderColor
                        border.width: 1
                        Behavior on color {
                            enabled: cueRules.animationEnabled
                            ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                        Behavior on border.color {
                            enabled: cueRules.animationEnabled
                            ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                    }

                    Rectangle {
                        id: attentionRim

                        anchors.fill: parent
                        radius: 10
                        color: "transparent"
                        border.color: window.attentionColor
                        border.width: sessionCard.cueLevel > 0 ? 3 : 2
                        opacity: sessionCard.pending ? (sessionCard.cueLevel > 0 ? 0.28 + (0.72 * sessionCard.cueLevel) : 0.86) : 0
                        visible: sessionCard.pending

                        SequentialAnimation {
                            id: cue

                            property real level: 0
                            running: false
                            loops: 2

                            onFinished: cue.level = 0

                            NumberAnimation {
                                target: cue
                                property: "level"
                                from: 0
                                to: 1
                                duration: 350
                                easing.type: Easing.InOutSine
                            }

                            NumberAnimation {
                                target: cue
                                property: "level"
                                from: 1
                                to: 0
                                duration: 350
                                easing.type: Easing.InOutSine
                            }
                        }

                        function beginCue() {
                            if (!cueRules.animationEnabled) {
                                resetCue();
                                return;
                            }
                            if (cue.running && !cue.paused)
                                return;
                            if (cue.paused) {
                                cue.resume();
                                return;
                            }
                            cue.stop();
                            cue.restart();
                        }
                    }
                }

                contentItem: ColumnLayout {
                    spacing: 7

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.topMargin: 9
                        Layout.leftMargin: 9
                        Layout.rightMargin: 9
                        color: window.paneColor
                        radius: 5
                        border.color: window.borderColor
                        border.width: 1
                        clip: true

                        TerminalSurface {
                            id: cardTerminal
                            anchors.fill: parent
                            anchors.margins: 1
                            // Only the focused session owns the keyboard, and
                            // only when blocks mode makes this tile the pane.
                            objectName: "cardTerminal_" + sessionCard.modelData.sessionId
                            document: sessionCard.modelData
                            enabled: !window.paneVisible
                                     && workspace.focusedIndex === sessionCard.index
                                     && sessionCard.modelData.live
                            interactive: !window.paneVisible && !window.settingsOpen
                                         && workspace.focusedIndex === sessionCard.index
                                         && sessionCard.modelData.live
                            focus: !window.paneVisible && !window.settingsOpen
                                           && workspace.focusedIndex === sessionCard.index
                                           && sessionCard.modelData.live
                            Component.onCompleted: {
                                // Blocks mode has no separate pane, so the
                                // focused live tile takes keyboard ownership.
                                if (focus)
                                    forceActiveFocus()
                            }
                            Connections {
                                target: workspace
                                function onFocusChanged() {
                                    if (!window.settingsOpen && !window.paneVisible && sessionCard.modelData.live
                                            && workspace.focusedIndex === sessionCard.index)
                                        cardTerminal.forceActiveFocus()
                                }
                            }
                            Connections {
                                target: window
                                function onBlocksLayoutChanged() {
                                    preview.deferTerminalFocus()
                                }
                            }
                        }
                    }

                    PreviewLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: sessionCard.width - 22
                        Layout.leftMargin: 11
                        Layout.rightMargin: 11
                        Layout.bottomMargin: 10
                        text: sessionCard.pending
                              ? qsTr("Needs input · ") + sessionCard.modelData.attentionReason
                              : sessionCard.modelData.directory
                        color: sessionCard.pending ? window.attentionColor : window.mutedTextColor
                        font.pixelSize: sessionCard.pending ? 11 : 10
                        font.weight: sessionCard.pending ? Font.Medium : Font.Normal
                        elide: Text.ElideMiddle
                    }
                }
            }
        }
    }
}
