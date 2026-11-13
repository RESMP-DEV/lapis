import QtQuick

// A few lines of a made-up thread in the reading view's colors and fonts,
// so a change in Settings shows before it closes: a prompt, a reply with
// code in a sentence, and a code card.
Rectangle {
    id: preview
    // A visual-view theme's colors ({background, raised, code, border, text,
    // muted, accent}) and the reading and code font families.
    property var colors: ({})
    property string sansFamily: ""
    property string monoFamily: ""
    property int fontSize: 14
    readonly property int codeSize: Math.round(fontSize * 0.86)
    readonly property bool lightCode: colors.code !== undefined && Qt.color(colors.code).hslLightness > 0.5

    implicitHeight: column.implicitHeight + 28
    radius: 8
    color: colors.background || "#1a1b26"
    border.width: 1
    border.color: colors.border || "#292e42"
    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Preview of the reading view")

    Column {
        id: column
        x: 14
        y: 14
        width: preview.width - 28
        spacing: 10
        Rectangle {
            anchors.right: parent.right
            width: prompt.implicitWidth + 24
            height: prompt.implicitHeight + 12
            radius: height / 2
            color: preview.colors.raised || "#24283b"
            Text {
                id: prompt
                anchors.centerIn: parent
                text: qsTr("Is the build green?")
                color: preview.colors.text || "#c0caf5"
                font.family: preview.sansFamily
                font.pixelSize: preview.fontSize
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            // Inline code is a tint of the text toward the muted color.
            text: qsTr("Yes. All 43 checks pass; run %1 again after the next change.")
                  .arg("<font face=\"" + preview.monoFamily + "\" color=\"" + (preview.colors.muted || "#737aa2")
                       + "\">ctest</font>")
            color: preview.colors.text || "#c0caf5"
            linkColor: preview.colors.accent || "#7aa2f7"
            font.family: preview.sansFamily
            font.pixelSize: preview.fontSize
        }
        Rectangle {
            width: parent.width
            height: code.implicitHeight + 16
            radius: 6
            color: preview.colors.code || "#16161e"
            Text {
                id: code
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "ctest --test-dir build -j 4"
                color: preview.lightCode ? "#3760bf" : "#c0caf5"
                font.family: preview.monoFamily
                font.pixelSize: preview.codeSize
            }
        }
    }
}
