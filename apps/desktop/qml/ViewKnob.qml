import QtQuick
import QtQuick.Controls

// One agent's switch between its thread and its terminal: a small pill with
// a sliding dot and "Thread", the size and quiet tones of the thread bar's
// model and effort pickers. The thread's bar shows it on; the stage shows it
// off over a terminal the person chose. It is never a keyboard target: a
// click flips the view and the keyboard follows whichever view shows.
Rectangle {
    id: knob
    property bool on: false
    // The caller's theme tokens; nothing here picks a colour of its own.
    property color textColor
    property color mutedColor
    property color restColor: "transparent"
    property color edgeColor: "transparent"
    property string fontFamily
    property int pixelSize: 12
    property int barHeight: Math.round(pixelSize * 1.86)
    // Milliseconds for the dot's slide; 0 with reduced motion.
    property int motion: 120
    signal toggled()

    readonly property bool hot: mouse.containsMouse
    readonly property color ink: hot ? textColor : mutedColor
    implicitWidth: row.implicitWidth + 16
    implicitHeight: barHeight
    radius: 8
    readonly property color hoverTint: Qt.rgba(textColor.r, textColor.g, textColor.b, 0.08)
    color: !hot ? restColor : restColor.a > 0 ? Qt.tint(restColor, hoverTint) : hoverTint
    border.width: edgeColor.a > 0 ? 1 : 0
    border.color: edgeColor
    activeFocusOnTab: false
    Accessible.role: Accessible.CheckBox
    Accessible.name: qsTr("Thread")
    Accessible.description: on ? qsTr("Show this agent's terminal") : qsTr("Show this agent's thread")
    Accessible.checkable: true
    Accessible.checked: on
    Accessible.focusable: false
    Accessible.onPressAction: knob.toggled()
    Accessible.onToggleAction: knob.toggled()
    ToolTip.visible: hot
    ToolTip.delay: 600
    ToolTip.text: Accessible.description

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Math.round(knob.pixelSize * 0.45)
        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round(knob.pixelSize * 1.7)
            height: Math.round(knob.pixelSize * 0.95)
            radius: height / 2
            color: knob.on ? Qt.rgba(knob.ink.r, knob.ink.g, knob.ink.b, 0.24) : "transparent"
            border.width: 1
            border.color: Qt.rgba(knob.ink.r, knob.ink.g, knob.ink.b, knob.on ? 0.24 : 0.7)
            Rectangle {
                objectName: "viewKnobDot"
                width: track.height - 4
                height: width
                radius: width / 2
                anchors.verticalCenter: parent.verticalCenter
                x: knob.on ? track.width - width - 2 : 2
                color: knob.ink
                Behavior on x {
                    enabled: knob.motion > 0
                    NumberAnimation { duration: knob.motion; easing.type: Easing.OutCubic }
                }
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Thread")
            color: knob.ink
            font.family: knob.fontFamily
            font.pixelSize: knob.pixelSize
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: knob.toggled()
    }
}
