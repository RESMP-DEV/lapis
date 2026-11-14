import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The factory pipeline: every agent but the orchestrator in four stages,
// from the workspace's factoryPipeline (live state merged with the judge's
// factory_state.json). Colours, type and corners come from the window theme,
// so light mode and every window theme follow it.
Rectangle {
    id: board
    objectName: "factoryBoard"

    // The window, for its theme (colours, fonts, corner radius).
    property var theme: null
    property var items: []
    property var summary: ({})
    property bool collapsed: false
    property bool interactive: true
    property double now: Date.now()

    signal inspectRequested(string agentId)
    signal openTerminalRequested(string agentId)
    signal detailsRequested(string agentId)
    // safeToClose: the judge says the agent can go, so no confirmation is asked.
    signal closeRequested(string agentId, bool safeToClose)
    signal collapseToggled()

    readonly property var stages: [
        { id: "needs_you", label: qsTr("Needs You"), short: qsTr("need you"),
          accent: theme ? theme.attentionColor : "#ff6b5a" },
        { id: "in_progress", label: qsTr("In Progress"), short: qsTr("running"),
          accent: theme ? theme.activityColor : "#74a8ff" },
        { id: "landed", label: qsTr("Landed"), short: qsTr("landed"),
          accent: theme ? theme.plentyColor : "#6fdc8c" },
        { id: "idle_cull", label: qsTr("Idle · Do We Need It?"), short: qsTr("idle"),
          accent: theme ? theme.mutedTextColor : "#8ea0b3" }
    ]
    readonly property int font: theme ? theme.chromeFont : 13
    readonly property int small: theme ? theme.readoutFont : 12
    readonly property string mono: theme ? theme.monoFamily : "Menlo"
    readonly property color text: theme ? theme.textColor : "#e7eef4"
    readonly property color muted: theme ? theme.mutedTextColor : "#8ea0b3"
    readonly property color card: theme ? theme.cardColor : "#15202b"
    readonly property color hovered: theme ? theme.hoveredCardColor : "#1c2a38"
    readonly property color edge: theme ? theme.borderColor : "#243140"
    readonly property int corner: theme ? theme.chromeRadius : 2

    function stageItems(stage) {
        return items.filter(item => item.stage === stage)
    }
    function countFor(stage) {
        return stage === "needs_you" ? (summary.needsYou || 0)
             : stage === "in_progress" ? (summary.inProgress || 0)
             : stage === "landed" ? (summary.landed || 0) : (summary.idleCull || 0)
    }
    function ago(ms) {
        if (!ms)
            return ""
        const minutes = Math.max(0, Math.round((now - ms) / 60000))
        if (minutes < 1)
            return qsTr("just now")
        if (minutes < 60)
            return qsTr("%1m ago").arg(minutes)
        const hours = Math.round(minutes / 60)
        return hours < 48 ? qsTr("%1h ago").arg(hours) : qsTr("%1d ago").arg(Math.round(hours / 24))
    }
    function tint(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha)
    }

    color: theme ? theme.surfaceColor : "#101820"
    border.color: edge
    border.width: 1
    radius: corner
    clip: true
    Timer {
        interval: 30000
        repeat: true
        running: board.visible
        onTriggered: board.now = Date.now()
    }

    // A small framed button in the board's own type.
    component BoardButton: Rectangle {
        id: button
        property string label: ""
        property color accent: board.muted
        property bool strong: false
        signal clicked()
        enabled: board.interactive
        implicitWidth: buttonText.implicitWidth + 16
        implicitHeight: Math.round(board.small * 1.85)
        radius: board.corner
        color: buttonHover.hovered ? board.tint(accent, 0.18) : strong ? board.tint(accent, 0.1) : "transparent"
        border.width: 1
        border.color: activeFocus ? accent : buttonHover.hovered || strong ? accent : board.edge
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: label
        Keys.onSpacePressed: function(event) {
            if (enabled)
                button.clicked()
            event.accepted = true
        }
        Keys.onReturnPressed: function(event) {
            if (enabled)
                button.clicked()
            event.accepted = true
        }
        Keys.onEnterPressed: function(event) {
            if (enabled)
                button.clicked()
            event.accepted = true
        }
        Accessible.onPressAction: if (enabled) button.clicked()
        Text {
            id: buttonText
            anchors.centerIn: parent
            text: button.label
            color: buttonHover.hovered || button.strong ? board.text : board.muted
            font.pixelSize: board.small
            font.weight: Font.DemiBold
        }
        HoverHandler { id: buttonHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { enabled: board.interactive; onTapped: button.clicked() }
    }
    // A short label in a rounded outline.
    component Pill: Rectangle {
        id: pill
        property string label: ""
        property color accent: board.muted
        property bool filled: false
        property bool monoText: false
        property bool elideText: false
        implicitWidth: pillText.implicitWidth + 12
        implicitHeight: Math.round(board.small * 1.5)
        radius: height / 2
        color: filled ? board.tint(accent, 0.2) : "transparent"
        border.width: 1
        border.color: board.tint(accent, filled ? 0.9 : 0.55)
        Text {
            id: pillText
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: pill.label
            elide: pill.elideText ? Text.ElideRight : Text.ElideNone
            horizontalAlignment: Text.AlignHCenter
            color: pill.filled ? board.text : pill.accent
            font.pixelSize: Math.max(10, board.small - 1)
            font.family: pill.monoText ? board.mono : Qt.application.font.family
            font.weight: Font.DemiBold
        }
    }

    // Collapsed: a narrow column of stage counts and the way back out.
    Column {
        objectName: "factoryBoardCollapsed"
        visible: board.collapsed
        anchors.top: parent.top
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 12
        BoardButton {
            objectName: "factoryBoardExpand"
            label: "‹"
            anchors.horizontalCenter: parent.horizontalCenter
            onClicked: board.collapseToggled()
        }
        Repeater {
            model: board.stages
            delegate: Column {
                required property var modelData
                spacing: 3
                anchors.horizontalCenter: parent.horizontalCenter
                Rectangle {
                    width: 26; height: 26; radius: 13
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: board.tint(modelData.accent, 0.16)
                    border.color: modelData.accent
                    Text {
                        anchors.centerIn: parent
                        text: board.countFor(modelData.id)
                        color: board.text
                        font.pixelSize: board.small
                        font.weight: Font.Bold
                    }
                }
            }
        }
    }

    ColumnLayout {
        visible: !board.collapsed
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        // The board's title, the judge, and the counts.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Column {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: qsTr("FACTORY PIPELINE")
                    color: board.text
                    font.pixelSize: board.small
                    font.weight: Font.Bold
                    font.letterSpacing: 1.4
                }
                Text {
                    objectName: "factoryJudgeLine"
                    width: parent.width
                    elide: Text.ElideRight
                    text: board.summary.judged
                          ? qsTr("Judged by %1 · %2").arg(board.summary.judgeModel || qsTr("judge"))
                                                      .arg(board.ago(board.summary.updatedAtMs))
                          : qsTr("Live state only · no judge has run yet")
                    color: board.muted
                    font.pixelSize: Math.max(10, board.small - 1)
                    font.family: board.mono
                }
            }
            BoardButton {
                objectName: "factoryBoardCollapse"
                label: "›"
                onClicked: board.collapseToggled()
            }
        }
        // Stage counts in one strip.
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: board.stages
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    implicitHeight: Math.round(board.small * 3.1)
                    radius: board.corner
                    color: board.tint(modelData.accent, 0.08)
                    border.color: board.tint(modelData.accent, 0.35)
                    Rectangle {
                        x: 1; y: 1
                        width: parent.width - 2; height: 2
                        color: modelData.accent
                    }
                    Column {
                        anchors.centerIn: parent
                        spacing: 0
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: board.countFor(modelData.id)
                            color: board.text
                            font.pixelSize: board.font + 3
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData.short
                            color: modelData.accent
                            font.pixelSize: Math.max(10, board.small - 2)
                            font.family: board.mono
                        }
                    }
                }
            }
        }

        Flickable {
            id: scroller
            objectName: "factoryScroller"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: stagesColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            // Cards rebuild on each status change; keep the scroll position
            // unless the board got shorter than it.
            function clampScroll() {
                const most = Math.max(0, contentHeight - height)
                if (contentY > most)
                    contentY = most
            }
            onContentHeightChanged: clampScroll()
            onHeightChanged: clampScroll()
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Column {
                id: stagesColumn
                width: scroller.width - 4
                spacing: 8

                Repeater {
                    model: board.stages
                    delegate: Column {
                        id: section
                        required property var modelData
                        readonly property var entries: board.stageItems(modelData.id)
                        objectName: "factoryStage_" + modelData.id
                        width: stagesColumn.width
                        spacing: 4

                        // The stage's heading: its accent, name and count.
                        Row {
                            spacing: 8
                            height: Math.round(board.font * 1.4)
                            Rectangle {
                                width: 3; height: parent.height - 4
                                anchors.verticalCenter: parent.verticalCenter
                                radius: 1
                                color: section.modelData.accent
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: section.modelData.label
                                color: board.text
                                font.pixelSize: board.font
                                font.weight: Font.DemiBold
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: section.entries.length
                                color: section.modelData.accent
                                font.pixelSize: board.small
                                font.family: board.mono
                                font.weight: Font.Bold
                            }
                        }
                        Text {
                            visible: section.entries.length === 0
                            leftPadding: 11
                            text: section.modelData.id === "needs_you" ? qsTr("Nothing waits on you")
                                : section.modelData.id === "in_progress" ? qsTr("No agent is working")
                                : section.modelData.id === "landed" ? qsTr("Nothing landed recently")
                                : qsTr("No idle agents")
                            color: board.muted
                            font.pixelSize: board.small
                            font.italic: true
                        }

                        Repeater {
                            model: section.entries
                            delegate: Rectangle {
                                id: card
                                required property var modelData
                                readonly property string stage: section.modelData.id
                                readonly property color accent: section.modelData.accent
                                readonly property bool safe: stage === "idle_cull" && modelData.judged
                                                             && !modelData.keepNeeded
                                objectName: "factoryCard_" + modelData.agentId
                                width: section.width
                                implicitHeight: cardColumn.implicitHeight + 12
                                radius: board.corner
                                color: cardHover.hovered ? board.hovered : board.card
                                border.width: 1
                                border.color: cardHover.hovered ? board.tint(accent, 0.7) : board.edge
                                HoverHandler { id: cardHover }
                                // The stage's edge down the card's side.
                                Rectangle {
                                    x: 1; y: 6
                                    width: 2; height: parent.height - 12
                                    radius: 1
                                    color: card.accent
                                }
                                Column {
                                    id: cardColumn
                                    x: 12; y: 6
                                    width: parent.width - 22
                                    spacing: 3

                                    RowLayout {
                                        width: parent.width
                                        spacing: 6
                                        Text {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: card.modelData.title
                                            elide: Text.ElideRight
                                            color: board.text
                                            font.pixelSize: board.font
                                            font.weight: Font.DemiBold
                                        }
                                        Pill {
                                            label: card.modelData.machine
                                                   ? card.modelData.harnessId + " · " + card.modelData.machine
                                                   : card.modelData.harnessId
                                            monoText: true
                                            elideText: true
                                            Layout.fillWidth: true
                                            Layout.maximumWidth: 180
                                            Layout.minimumWidth: 0
                                        }
                                        Text {
                                            text: card.modelData.shortId.slice(0, 6)
                                            color: board.muted
                                            font.pixelSize: Math.max(10, board.small - 1)
                                            font.family: board.mono
                                        }
                                    }
                                    // What it is doing or waiting on.
                                    Text {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        maximumLineCount: 2
                                        wrapMode: Text.Wrap
                                        text: card.stage === "needs_you" && card.modelData.attentionReason
                                              ? card.modelData.attentionReason
                                              : card.modelData.headline || card.modelData.activity
                                                || card.modelData.statusLabel
                                        color: card.stage === "needs_you" ? board.text : board.muted
                                        font.pixelSize: board.small
                                    }
                                    // The judge's reason, for idle agents.
                                    Text {
                                        visible: card.stage === "idle_cull" && text.length > 0
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: card.modelData.reason || ""
                                        color: board.muted
                                        font.pixelSize: Math.max(10, board.small - 1)
                                        font.italic: true
                                    }
                                    RowLayout {
                                        width: parent.width
                                        spacing: 6
                                        Pill {
                                            visible: card.stage !== "idle_cull"
                                            label: card.modelData.attentionPending
                                                   ? qsTr("%n waiting", "", card.modelData.attentionCount)
                                                   : card.modelData.statusLabel
                                            accent: card.accent
                                        }
                                        Pill {
                                            objectName: "factoryBadge_" + card.modelData.agentId
                                            visible: card.stage === "idle_cull"
                                            label: card.safe ? qsTr("Safe to close")
                                                 : card.modelData.judged ? qsTr("Keep parked") : qsTr("Not judged")
                                            accent: card.safe ? (board.theme ? board.theme.plentyColor : "#6fdc8c")
                                                              : board.muted
                                            filled: card.safe
                                        }
                                        Pill {
                                            objectName: "factoryReceipt_" + card.modelData.agentId
                                            visible: (card.modelData.receipt || "").length > 0
                                                     && (card.stage === "landed" || card.stage === "idle_cull")
                                            label: card.modelData.receipt || ""
                                            accent: board.theme ? board.theme.plentyColor : "#6fdc8c"
                                            monoText: true
                                            elideText: true
                                            Layout.fillWidth: true
                                            Layout.maximumWidth: 180
                                            Layout.minimumWidth: 0
                                        }
                                        Item { Layout.fillWidth: true }
                                        BoardButton {
                                            objectName: "factoryOpen_" + card.modelData.agentId
                                            visible: card.stage === "needs_you"
                                            label: qsTr("Open terminal")
                                            accent: card.accent
                                            strong: true
                                            onClicked: board.openTerminalRequested(card.modelData.agentId)
                                        }
                                        BoardButton {
                                            objectName: "factoryDetails_" + card.modelData.agentId
                                            visible: card.stage === "needs_you"
                                            label: qsTr("Details")
                                            onClicked: board.detailsRequested(card.modelData.agentId)
                                        }
                                        BoardButton {
                                            objectName: "factoryInspect_" + card.modelData.agentId
                                            visible: card.stage !== "needs_you"
                                            label: qsTr("Inspect")
                                            accent: card.accent
                                            onClicked: board.inspectRequested(card.modelData.agentId)
                                        }
                                        BoardButton {
                                            objectName: "factoryClose_" + card.modelData.agentId
                                            visible: card.stage === "landed" || card.stage === "idle_cull"
                                            label: qsTr("Close")
                                            accent: board.theme ? board.theme.attentionColor : "#ff6b5a"
                                            strong: card.safe
                                            onClicked: board.closeRequested(card.modelData.agentId, card.safe)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
