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
    color: "#0b101a"
    title: "lapis"
    objectName: "workspaceWindow"

    readonly property color backgroundColor: "#0b101a"
    readonly property color surfaceColor: "#111927"
    readonly property color cardColor: "#151c29"
    readonly property color hoveredCardColor: "#1a2333"
    readonly property color focusedColor: "#1f2838"
    readonly property color borderColor: "#2b3546"
    readonly property color focusedBorderColor: "#6a76e8"
    readonly property color textColor: "#f2f3ea"
    readonly property color mutedTextColor: "#98a0ae"
    readonly property color attentionColor: "#ff5f56"
    readonly property color attentionTextColor: "#fff4f2"
    readonly property alias cueAnimationEnabled: cueRules.animationEnabled

    // Shortcuts come from lapis.json through the `keymap` context property. The
    // literals below are the fallback used only when no keymap is supplied, such
    // as in an isolated QML preview that does not load a config file.
    function bindings(action, fallback) {
        if (typeof keymap !== "undefined" && keymap !== null) {
            const configured = keymap.actionSequences(action)
            if (configured.length > 0)
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
        sequences: window.bindings("nextCategory", ["Ctrl+Tab"])
        onActivated: workspace.nextCategory()
    }
    Shortcut {
        sequences: window.bindings("previousCategory", ["Ctrl+Shift+Tab"])
        onActivated: workspace.nextCategory(-1)
    }
    Shortcut {
        sequences: window.bindings("category1", ["Ctrl+1"])
        onActivated: workspace.focusCategory(0)
    }
    Shortcut {
        sequences: window.bindings("category2", ["Ctrl+2"])
        onActivated: workspace.focusCategory(1)
    }
    Shortcut {
        sequences: window.bindings("category3", ["Ctrl+3"])
        onActivated: workspace.focusCategory(2)
    }
    Shortcut {
        sequences: window.bindings("category4", ["Ctrl+4"])
        onActivated: workspace.focusCategory(3)
    }

    // Window navigation inside the current category. Holding the chord repeats,
    // because Qt auto-repeats an activated Shortcut while the keys are held.
    Shortcut {
        sequences: window.bindings("nextWindow", ["Ctrl+Shift+]"])
        onActivated: workspace.nextWindow()
    }
    Shortcut {
        sequences: window.bindings("previousWindow", ["Ctrl+Shift+["])
        onActivated: workspace.nextWindow(-1)
    }
    Shortcut {
        sequences: window.bindings("focusLeft", ["Ctrl+Left"])
        onActivated: workspace.nextWindow(-1)
    }
    Shortcut {
        sequences: window.bindings("focusRight", ["Ctrl+Right"])
        onActivated: workspace.nextWindow()
    }

    Shortcut {
        sequences: window.bindings("cycleLayout", ["Ctrl+L"])
        onActivated: {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.toggleLayout()
        }
    }
    Shortcut {
        sequences: window.bindings("reloadConfig", ["Ctrl+R"])
        onActivated: {
            if (typeof keymap !== "undefined" && keymap !== null)
                keymap.reload()
        }
    }

    readonly property bool blocksLayout: typeof keymap !== "undefined" && keymap !== null && keymap.blocks

    // Keyboard ownership lives in C++ so it can be asserted directly rather
    // than inferred from QML property state. Both entry points call it.
    onActiveChanged: if (active) keyboardOwnershipReady()
    onBlocksLayoutChanged: keyboardOwnershipReady()

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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        anchors.topMargin: 6
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
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
                        text: qsTr("Layout: %1").arg(window.blocksLayout ? "blocks" : "focus")
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
            Layout.fillWidth: true
            Layout.fillHeight: !window.blocksLayout
            Layout.preferredHeight: window.blocksLayout ? 0 : -1
            visible: !window.blocksLayout
            color: "#0d131d"
            radius: 12
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
                    color: "#a8b3c5"
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
                             && !window.blocksLayout
                focus: !window.blocksLayout
                Component.onCompleted: if (!window.blocksLayout) forceActiveFocus()
                // Claim the keyboard whenever this pane becomes the active
                // surface, so switching layout never leaves focus on a button.
                Connections {
                    target: window
                    function onBlocksLayoutChanged() {
                        window.setTerminalFocus()
                    }
                }
            }
        }

        ListView {
            id: carousel

            // In blocks mode this becomes the workspace: it fills the window,
            // wraps into a grid and sizes tiles to fill the available space.
            Layout.fillWidth: true
            Layout.fillHeight: window.blocksLayout
            Layout.preferredHeight: window.blocksLayout ? -1 : 172
            Layout.minimumHeight: window.blocksLayout ? 0 : 164
            orientation: ListView.Horizontal
            boundsMovement: Flickable.StopAtBounds
            clip: true
            spacing: 14
            topMargin: 3
            leftMargin: 1
            rightMargin: 1
            model: workspace.sessions

            // Orientation cannot change at runtime, so blocks mode uses the
            // vertical flow, which wraps into as many columns as fit. Tiles are
            // sized so the visible rows fill the viewport with no dead space;
            // more sessions than fit simply scroll.
            readonly property int blockColumns: 3
            readonly property int tileWidth: window.blocksLayout ?
                                                 Math.max(220, Math.floor((width - spacing * (blockColumns - 1)) / blockColumns)) : 238
            readonly property int tileHeight: window.blocksLayout ?
                                                  Math.max(150, Math.floor((height - spacing) / 2)) : 156

            ScrollBar.horizontal: ScrollBar {
                implicitHeight: 8
                contentItem: Rectangle {
                    radius: 4
                    color: window.borderColor
                }
            }

            delegate: Button {
                id: sessionCard

                required property int index
                required property var modelData

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
                    if (window.blocksLayout)
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
                        color: "#0d1421"
                        radius: 5
                        border.color: "#283446"
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
                            enabled: window.blocksLayout
                                     && workspace.focusedIndex === sessionCard.index
                                     && sessionCard.modelData.live
                            interactive: window.blocksLayout
                                         && workspace.focusedIndex === sessionCard.index
                                         && sessionCard.modelData.live
                            focus: window.blocksLayout
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
                                    if (window.blocksLayout && sessionCard.modelData.live
                                            && workspace.focusedIndex === sessionCard.index)
                                        cardTerminal.forceActiveFocus()
                                }
                            }
                            Connections {
                                target: window
                                function onBlocksLayoutChanged() {
                                    window.setTerminalFocus()
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
