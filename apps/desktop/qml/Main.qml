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

    width: 1400
    height: 960
    minimumWidth: 980
    minimumHeight: 700
    color: "#0b101a"
    title: "lapis"

    readonly property color backgroundColor: "#0b101a"
    readonly property color surfaceColor: "#111927"
    readonly property color cardColor: "#151c29"
    readonly property color hoveredCardColor: "#1a2333"
    readonly property color focusedColor: "#1f2838"
    readonly property color borderColor: "#2b3546"
    readonly property color focusedBorderColor: "#6a76e8"
    readonly property color textColor: "#f2f3ea"
    readonly property color mutedTextColor: "#98a0ae"

    Shortcut { sequences: [StandardKey.Quit]; onActivated: Qt.quit() }
    Shortcut { sequences: [StandardKey.Close]; onActivated: Qt.quit() }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 18

        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: 2
            spacing: 16

            Label {
                text: "lapis"
                color: window.textColor
                font.pixelSize: 21
                font.weight: Font.DemiBold
                font.letterSpacing: 0.2
                renderType: Text.QtRendering
            }

            Rectangle {
                implicitWidth: 1
                implicitHeight: 30
                color: window.borderColor
            }

            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Layout.maximumWidth: 520

                PreviewLabel {
                    Layout.fillWidth: true
                    text: workspace.focusedSession ? workspace.focusedSession.title : "No focused session"
                    color: window.textColor
                    font.pixelSize: 15
                    font.weight: Font.Medium
                    elide: Text.ElideMiddle
                }

                PreviewLabel {
                    Layout.fillWidth: true
                    text: workspace.focusedSession ? workspace.focusedSession.directory : ""
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                }
            }

            Item {
                Layout.fillWidth: true
            }

            ColumnLayout {
                spacing: 4
                Layout.alignment: Qt.AlignRight

                RowLayout {
                    spacing: 8
                    Layout.alignment: Qt.AlignRight

                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: 8
                        implicitHeight: 8
                        radius: 4
                        color: workspace.focusedSession ? workspace.focusedSession.accent : window.borderColor
                    }

                    PreviewLabel {
                        text: workspace.focusedSession ? workspace.focusedSession.activity : "Idle"
                        color: window.textColor
                        font.pixelSize: 13
                        font.weight: Font.Medium
                    }
                }

                Rectangle {
                    Layout.alignment: Qt.AlignRight
                    implicitWidth: previewIndicator.implicitWidth + 18
                    implicitHeight: 24
                    radius: 4
                    color: "#141b29"
                    border.color: window.borderColor
                    border.width: 1

                    PreviewLabel {
                        id: previewIndicator
                        anchors.centerIn: parent
                        text: "Live shell · carousel preview"
                        font.pixelSize: 11
                        font.letterSpacing: 0.3
                    }
                }
            }
        }

        Rectangle {
            id: focusedPane

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
                padding: 0
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                Accessible.role: Accessible.Button
                Accessible.name: sessionCard.modelData.title + ", " + sessionCard.modelData.activity
                Accessible.description: sessionCard.index === 0 ? "Live shell preview" : "Placeholder session"

                onClicked: liveTerminal.forceActiveFocus()
                transform: Translate {
                    y: sessionCard.hovered ? -2 : 0
                    Behavior on y { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }

                background: Rectangle {
                    radius: 10
                    color: workspace.focusedIndex === sessionCard.index ? window.focusedColor :
                            sessionCard.hovered ? window.hoveredCardColor : window.cardColor
                    border.color: workspace.focusedIndex === sessionCard.index ? window.focusedBorderColor : window.borderColor
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 130; easing.type: Easing.OutCubic } }
                    Behavior on border.color { ColorAnimation { duration: 130; easing.type: Easing.OutCubic } }
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
                            document: sessionCard.modelData
                        }
                    }

                    PreviewLabel {
                        Layout.leftMargin: 11
                        Layout.rightMargin: 11
                        Layout.bottomMargin: 10
                        text: sessionCard.modelData.directory
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                    }
                }
            }
        }
    }
}
