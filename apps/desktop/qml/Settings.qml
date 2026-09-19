import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Appearance settings, opened with the configured openSettings shortcut.
//
// Every control writes straight through to the keymap, which applies the change
// to the live window and persists it to lapis.json, so the effect is visible
// behind the dialog and survives a restart. Validation lives in C++: an unknown
// name is rejected there rather than silently accepted here.
Dialog {
    id: settings

    required property var themeModel
    required property var layoutModel
    required property var densityModel
    required property string currentTheme
    required property string currentLayout
    required property string currentDensity
    required property string configPath
    required property string configDiagnostic
    required property string shortcutHint

    signal themeChosen(string name)
    signal layoutChosen(string name)
    signal densityChosen(string name)

    objectName: "settingsDialog"
    title: qsTr("Appearance")
    modal: true
    anchors.centerIn: parent
    width: 620
    height: 560
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: settings.paletteWindow
        radius: 10
        border.width: 1
        border.color: settings.paletteBorder
    }

    // The dialog renders in the scheme it is choosing, so a theme reads the
    // same way here as it will in the window behind.
    property var paletteWindow: "#111927"
    property var paletteSurface: "#151c29"
    property var paletteBorder: "#2b3546"
    property var paletteText: "#f2f3ea"
    property var paletteMuted: "#98a0ae"
    property var paletteAccent: "#6a76e8"

    function themeByName(name) {
        for (let i = 0; i < themeModel.length; ++i) {
            if (themeModel[i].name === name)
                return themeModel[i]
        }
        return null
    }

    onCurrentThemeChanged: {
        const t = themeByName(currentTheme)
        if (!t)
            return
        paletteWindow = t.surface
        paletteSurface = t.card
        paletteBorder = t.border
        paletteText = t.text
        paletteMuted = t.mutedText
        paletteAccent = t.focusedBorder
    }

    component SectionLabel: Label {
        color: settings.paletteMuted
        font.pixelSize: 11
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.6
    }

    component ChoiceRow: ColumnLayout {
        id: row

        required property string label
        required property string hint
        required property var names
        required property string selected
        required property var describe
        signal picked(string name)

        spacing: 8
        Layout.fillWidth: true

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                text: row.label
                color: settings.paletteText
                font.pixelSize: 13
            }
            Label {
                Layout.fillWidth: true
                text: row.describe(row.selected)
                color: settings.paletteMuted
                font.pixelSize: 11
                // The description is the only explanation of what a layout does,
                // so wrap rather than cut it off mid-word beside the buttons.
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignLeft
            spacing: 6

            Repeater {
                model: row.names

                delegate: Button {
                    required property string modelData
                    objectName: "choice-" + modelData
                    checked: modelData === row.selected
                    checkable: false
                    text: modelData === "focus" ? qsTr("Focus") :
                          modelData === "columns" ? qsTr("Columns") :
                          modelData === "blocks" ? qsTr("Blocks") :
                          modelData === "stack" ? qsTr("Stack") :
                          modelData === "comfortable" ? qsTr("Comfortable") :
                          modelData === "compact" ? qsTr("Compact") :
                          modelData === "minimal" ? qsTr("Minimal") : modelData
                    font.pixelSize: 11
                    focusPolicy: Qt.NoFocus
                    onClicked: row.picked(modelData)
                }
            }
        }
    }

    // The header is drawn by this dialog, not by the style, so the title is not
    // repeated: the default title bar is suppressed with a zero-height one.
    header: Item {
        implicitHeight: 0
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 52
            color: settings.paletteSurface
            topLeftRadius: 10
            topRightRadius: 10

            Label {
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Appearance")
                color: settings.paletteText
                font.pixelSize: 15
                font.bold: true
            }
            Label {
                anchors.right: parent.right
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                text: settings.shortcutHint
                color: settings.paletteMuted
                font.pixelSize: 11
            }
        }

        Flickable {
            objectName: "settingsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 18
            contentHeight: body.implicitHeight
            ScrollBar.vertical: ScrollBar {}
            clip: true

            ColumnLayout {
                id: body
                width: parent.width
                spacing: 18

                SectionLabel { text: qsTr("Theme") }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 3
                    columnSpacing: 10
                    rowSpacing: 10

                    Repeater {
                        model: settings.themeModel

                        delegate: Button {
                            required property var modelData
                            objectName: "theme-" + modelData.name
                            Layout.fillWidth: true
                            implicitHeight: 62
                            focusPolicy: Qt.NoFocus
                            padding: 0
                            hoverEnabled: true
                            checked: modelData.name === settings.currentTheme
                            ToolTip.visible: hovered
                            ToolTip.text: modelData.label
                            onClicked: settings.themeChosen(modelData.name)

                            background: Rectangle {
                                radius: 7
                                color: modelData.card
                                border.width: modelData.name === settings.currentTheme ? 2 : 1
                                border.color: modelData.name === settings.currentTheme ?
                                                  modelData.focusedBorder : modelData.border

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 4

                                    Label {
                                        text: modelData.label
                                        color: modelData.text
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        spacing: 3
                                        Layout.alignment: Qt.AlignLeft
                                        Repeater {
                                            model: [modelData.text, modelData.mutedText,
                                                    modelData.focusedBorder, modelData.attention]
                                            delegate: Rectangle {
                                                required property var modelData
                                                width: 14
                                                height: 8
                                                radius: 2
                                                color: modelData
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: settings.paletteBorder
                }

                SectionLabel { text: qsTr("Layout") }

                ChoiceRow {
                    label: qsTr("Session arrangement")
                    hint: qsTr("Ctrl-L cycles layouts without opening this dialog.")
                    names: settings.layoutModel
                    selected: settings.currentLayout
                    describe: function(name) {
                        return name === "focus" ? qsTr("One large pane with a preview strip below.") :
                               name === "columns" ? qsTr("Previews in a left channel beside the pane.") :
                               name === "blocks" ? qsTr("Every session gets an equal tile.") :
                               name === "stack" ? qsTr("One session at a time, filling the window.") : ""
                    }
                    onPicked: function(name) { settings.layoutChosen(name) }
                }

                SectionLabel { text: qsTr("Preview size") }

                ChoiceRow {
                    label: qsTr("Card density")
                    hint: qsTr("Applies to the preview cards, not the terminal text.")
                    names: settings.densityModel
                    selected: settings.currentDensity
                    describe: function(name) {
                        return name === "comfortable" ? qsTr("Tallest cards, most terminal visible.") :
                               name === "compact" ? qsTr("Shorter cards, more of them on screen.") :
                               name === "minimal" ? qsTr("Thumbnails only, most sessions at once.") : ""
                    }
                    onPicked: function(name) { settings.densityChosen(name) }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: settings.paletteBorder
                }

                SectionLabel { text: qsTr("Configuration") }

                Label {
                    Layout.fillWidth: true
                    text: settings.configPath
                    color: settings.paletteMuted
                    font.pixelSize: 11
                    font.family: "Menlo"
                    elide: Text.ElideMiddle
                }
                Label {
                    Layout.fillWidth: true
                    visible: settings.configDiagnostic.length > 0
                    text: settings.configDiagnostic
                    color: settings.paletteText
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    font.family: "Menlo"
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Selections are written here immediately. Ctrl-R reloads the file from disk.")
                    color: settings.paletteMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
