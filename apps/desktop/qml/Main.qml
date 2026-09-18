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
        focusPolicy: Qt.NoFocus
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

    Shortcut { sequences: [StandardKey.Quit]; onActivated: Qt.quit() }
    Shortcut { sequences: [StandardKey.Close]; onActivated: Qt.quit() }

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
        anchors.margins: 20
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            spacing: 10

            PreviewLabel {
                Layout.fillWidth: true
                Layout.maximumWidth: 460
                text: workspace.focusedSession ? workspace.focusedSession.title : "No focused session"
                color: window.textColor
                font.pixelSize: 12
                font.weight: Font.Medium
                elide: Text.ElideMiddle
            }

            PreviewLabel {
                Layout.fillWidth: true
                Layout.maximumWidth: 430
                text: workspace.focusedSession ? workspace.focusedSession.directory : ""
                font.pixelSize: 11
                elide: Text.ElideMiddle
            }

            PreviewLabel {
                Layout.alignment: Qt.AlignVCenter
                Layout.maximumWidth: 155
                text: workspace.focusedSession ? workspace.focusedSession.activity : "Idle"
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 8
                implicitHeight: 8
                radius: 4
                color: workspace.focusedSession ? workspace.focusedSession.accent : window.borderColor
            }

            Item { Layout.fillWidth: true }

            Loader {
                Layout.alignment: Qt.AlignVCenter
                active: preview.active
                sourceComponent: RowLayout {
                    spacing: 6

                    Button {
                        focusPolicy: Qt.NoFocus
                        implicitHeight: 24
                        text: preview.diagnostics.length ? qsTr("Preview · error") : qsTr("Preview controls")
                        background: Rectangle {
                            radius: 5
                            color: window.cardColor
                            border.color: window.borderColor
                        }
                        ToolTip.visible: hovered && preview.diagnostics.length > 0
                        ToolTip.text: preview.diagnostics
                        Accessible.description: preview.diagnostics

                        onClicked: previewMenu.popup()

                        Menu {
                            id: previewMenu

                            PreviewMenuItem { action: replayArrival }
                            PreviewMenuItem { action: replayDuplicate }
                            PreviewMenuItem { action: replayTwo }
                            PreviewMenuItem { action: replayResolve }
                            PreviewMenuItem { action: replayReset }
                            MenuSeparator {}
                            PreviewMenuItem { action: previewReload }
                            PreviewMenuItem {
                                action: toggleReducedMotion
                            }
                        }
                    }
                }
            }
        }

        Action {
            id: replayArrival

            text: qsTr("Arrival")
            enabled: preview.active
            onTriggered: window.replayAttention("arrival")
        }

        Action {
            id: replayDuplicate

            text: qsTr("Duplicate")
            enabled: preview.active
            onTriggered: window.replayAttention("duplicate")
        }

        Action {
            id: replayTwo

            text: qsTr("Two")
            enabled: preview.active
            onTriggered: window.replayAttention("two")
        }

        Action {
            id: replayResolve

            text: qsTr("Resolve")
            enabled: preview.active
            onTriggered: window.replayAttention("resolve")
        }

        Action {
            id: replayReset

            text: qsTr("Reset")
            enabled: preview.active
            onTriggered: window.replayAttention("reset")
        }

        Action {
            id: previewReload

            text: qsTr("Reload")
            enabled: preview.active
            onTriggered: preview.reload()
        }

        Action {
            id: toggleReducedMotion

            text: qsTr("Reduced Motion")
            checkable: true
            checked: preview.reducedMotion
            enabled: preview.active && !preview.systemReducedMotion
            onTriggered: {
                preview.reducedMotion = !preview.reducedMotion;
            }
        }

        Rectangle {
            id: focusedPane

            objectName: "focusedPane"

            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0d131d"
            radius: 12
            border.color: window.focusedBorderColor
            border.width: 1

            TerminalSurface {
                anchors.fill: parent
                id: liveTerminal
                objectName: "liveTerminal"
                anchors.margins: 18
                document: workspace.focusedSession
                interactive: true
                focus: true
                Component.onCompleted: forceActiveFocus()
            }
        }

        ListView {
            id: carousel

            Layout.fillWidth: true
            Layout.preferredHeight: 172
            Layout.minimumHeight: 164
            orientation: ListView.Horizontal
            boundsMovement: Flickable.StopAtBounds
            clip: true
            spacing: 14
            topMargin: 3
            leftMargin: 1
            rightMargin: 1
            model: workspace.sessions

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

                implicitWidth: 238
                implicitHeight: 156
                objectName: "sessionCard_" + sessionCard.modelData.sessionId
                padding: 0
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.role: Accessible.Button
                Accessible.name: sessionCard.modelData.title + ", " + sessionCard.modelData.activity
                Accessible.description: sessionCard.index === 0 ? "Live shell preview" : "Placeholder session"
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

                onClicked: liveTerminal.forceActiveFocus()
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

                    RowLayout {
                        Layout.leftMargin: 11
                        Layout.rightMargin: 11
                        Layout.topMargin: 9
                        spacing: 7

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 3
                            implicitHeight: 16
                            radius: 1.5
                            color: sessionCard.modelData.accent
                        }

                        PreviewLabel {
                            Layout.fillWidth: true
                            text: sessionCard.modelData.title
                            color: window.textColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            elide: Text.ElideMiddle
                        }

                        PreviewLabel {
                            text: sessionCard.modelData.live ? "Live" : "Preview"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.leftMargin: 9
                        Layout.rightMargin: 9
                        color: "#0d1421"
                        radius: 5
                        border.color: "#283446"
                        border.width: 1
                        clip: true

                        TerminalSurface {
                            anchors.fill: parent
                            anchors.margins: 1
                            enabled: false
                            document: sessionCard.modelData
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
