import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Appearance. Themes, density and the terminal font come from the keymap and
// apply immediately. There is one workspace stage, so this dialog does not
// offer a layout or preview picker. Font controls live only here.
Dialog {
    id: settings

    required property var themeModel
    required property var densityModel
    required property string currentTheme
    required property string currentDensity
    required property string configPath
    required property string configDiagnostic
    required property string shortcutHint
    required property var fontFamilies
    // Configured family; empty selects the platform's fixed-width font.
    required property string fontFamily
    required property string resolvedFontFamily
    required property int fontSize
    required property int fontSizeMinimum
    required property int fontSizeMaximum
    required property int fontSizeDefault
    required property int motionDuration
    required property bool motionEnabled

    signal themeChosen(string name)
    signal densityChosen(string name)
    signal fontFamilyChosen(string name)
    signal fontSizeChosen(int pixels)

    readonly property int uiFont: {
        if (font.pixelSize > 0)
            return Math.max(12, font.pixelSize)
        if (font.pointSize > 0)
            return Math.max(12, Math.round(font.pointSize * 96 / 72))
        return 13
    }

    readonly property var currentAppearance: themeByName(currentTheme)
    readonly property int chromeRadius: currentAppearance ? currentAppearance.cornerRadius : 2
    readonly property int readoutFont: Math.max(11, uiFont - 2)
    font.family: currentAppearance && currentAppearance.monoChrome ? resolvedFontFamily : Qt.application.font.family

    objectName: "settingsDialog"
    title: qsTr("Appearance")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(560, Math.max(300, (parent ? parent.width : 640) - 24))
    height: Math.min(560, Math.max(280, (parent ? parent.height : 480) - 24))
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property color paletteWindow: "#070b10"
    property color paletteSurface: "#101820"
    property color paletteBorder: "#243140"
    property color paletteText: "#e7eef4"
    property color paletteMuted: "#8ea0b3"
    property color paletteAccent: "#7ddec8"
    property color paletteSelected: "#193638"
    property color paletteHover: "#1b303e"
    property color paletteFault: "#ee7a8a"

    function themeByName(name) {
        for (let i = 0; i < themeModel.length; ++i) {
            if (themeModel[i].name === name)
                return themeModel[i]
        }
        return null
    }

    function applyTheme(name) {
        const theme = themeByName(name)
        if (!theme)
            return
        paletteWindow = theme.surface
        paletteSurface = theme.card
        paletteBorder = theme.border
        paletteText = theme.text
        paletteMuted = theme.mutedText
        paletteAccent = theme.focusedBorder
        paletteSelected = theme.focused
        paletteHover = theme.hoveredCard
        paletteFault = theme.fault
    }

    Component.onCompleted: applyTheme(currentTheme)
    onCurrentThemeChanged: applyTheme(currentTheme)

    background: Rectangle {
        color: settings.paletteWindow
        radius: settings.chromeRadius
        border.width: 1
        border.color: settings.paletteBorder
    }

    header: Item {
        implicitHeight: 0
    }

    component PlainLabel: Label { textFormat: Text.PlainText }

    // Shared control feedback: hover and press wash, selection fill with a
    // focus edge. Color changes only, within the theme's motion duration.
    component ChoiceSurface: Rectangle {
        id: surface
        property bool marked: false
        property bool hovered: false
        property bool pressed: false
        radius: settings.chromeRadius
        color: marked || pressed ? settings.paletteSelected :
               hovered ? settings.paletteHover : settings.paletteWindow
        border.width: marked ? 2 : 1
        border.color: marked ? settings.paletteAccent :
                      hovered ? settings.paletteMuted : settings.paletteBorder
        Behavior on color {
            enabled: settings.motionEnabled
            ColorAnimation { duration: settings.motionDuration; easing.type: Easing.OutCubic }
        }
        Behavior on border.color {
            enabled: settings.motionEnabled
            ColorAnimation { duration: settings.motionDuration; easing.type: Easing.OutCubic }
        }
    }

    component StepButton: Button {
        id: stepButton
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        font.family: settings.resolvedFontFamily
        font.pixelSize: Math.max(12, settings.uiFont)
        implicitWidth: Math.max(36, implicitContentWidth + 20)
        implicitHeight: Math.max(32, settings.uiFont + 18)
        contentItem: PlainLabel {
            text: stepButton.text
            font: stepButton.font
            color: stepButton.enabled ? settings.paletteText : settings.paletteMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: ChoiceSurface {
            hovered: stepButton.hovered && stepButton.enabled
            pressed: stepButton.down
        }
    }

    component SectionLabel: PlainLabel {
        color: settings.paletteMuted
        font.pixelSize: Math.max(12, settings.uiFont - 1)
        font.bold: true
    }

    component ChoiceRow: ColumnLayout {
        id: row

        required property string label
        required property var names
        required property string selected
        required property var describe
        signal picked(string name)

        spacing: 8
        Layout.fillWidth: true

        PlainLabel {
            text: row.label
            color: settings.paletteText
            font.pixelSize: settings.uiFont
        }
        PlainLabel {
            Layout.fillWidth: true
            text: row.describe(row.selected)
            color: settings.paletteMuted
            font.pixelSize: Math.max(12, settings.uiFont - 1)
            wrapMode: Text.WordWrap
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: row.names

                delegate: Button {
                    id: choiceButton
                    required property string modelData
                    readonly property bool marked: modelData === row.selected
                    objectName: "choice-" + modelData
                    text: modelData === "comfortable" ? qsTr("Comfortable") :
                          modelData === "compact" ? qsTr("Compact") :
                          modelData === "minimal" ? qsTr("Minimal") : modelData
                    font.pixelSize: Math.max(12, settings.uiFont - 1)
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    onClicked: row.picked(modelData)

                    contentItem: PlainLabel {
                        text: choiceButton.text
                        font: choiceButton.font
                        color: settings.paletteText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: ChoiceSurface {
                        marked: choiceButton.marked
                        hovered: choiceButton.hovered
                        pressed: choiceButton.down
                    }
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(48, settings.uiFont + 32)
            color: settings.paletteSurface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                PlainLabel {
                    text: qsTr("Appearance")
                    color: settings.paletteText
                    font.pixelSize: settings.uiFont + 2
                    font.bold: true
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                PlainLabel {
                    text: settings.shortcutHint
                    color: settings.paletteMuted
                    font.family: settings.resolvedFontFamily
                    font.pixelSize: settings.readoutFont
                    elide: Text.ElideRight
                }
            }
        }

        Flickable {
            id: settingsFlick
            objectName: "settingsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 16
            contentWidth: width
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            ColumnLayout {
                id: body
                width: settingsFlick.width
                spacing: 16

                SectionLabel {
                    text: qsTr("Theme")
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: settings.width < 460 ? 2 : 3
                    columnSpacing: 8
                    rowSpacing: 8

                    Repeater {
                        model: settings.themeModel

                        delegate: Button {
                            id: themeButton
                            required property var modelData
                            objectName: "theme-" + modelData.name
                            Layout.fillWidth: true
                            implicitHeight: Math.max(64, settings.uiFont * 4)
                            focusPolicy: Qt.NoFocus
                            padding: 0
                            hoverEnabled: true
                            onClicked: settings.themeChosen(modelData.name)

                            background: Rectangle {
                                radius: settings.chromeRadius
                                color: themeButton.hovered ? modelData.hoveredCard : modelData.card
                                border.width: modelData.name === settings.currentTheme ? 2 : 1
                                border.color: modelData.name === settings.currentTheme ? modelData.focusedBorder :
                                              themeButton.hovered ? modelData.mutedText : modelData.border
                                Behavior on color {
                                    enabled: settings.motionEnabled
                                    ColorAnimation { duration: settings.motionDuration; easing.type: Easing.OutCubic }
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 6

                                    PlainLabel {
                                        text: modelData.label
                                        color: modelData.text
                                        font.pixelSize: Math.max(12, settings.uiFont - 1)
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        spacing: 4
                                        Repeater {
                                            // Focus, activity, pending request and fault.
                                            model: [modelData.focusedBorder, modelData.activity,
                                                    modelData.attention, modelData.fault]
                                            delegate: Rectangle {
                                                required property var modelData
                                                implicitWidth: 16
                                                implicitHeight: 8
                                                Layout.preferredWidth: 16
                                                Layout.preferredHeight: 8
                                                radius: 1
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

                SectionLabel {
                    text: qsTr("Density")
                }

                ChoiceRow {
                    label: qsTr("Chrome")
                    names: settings.densityModel
                    selected: settings.currentDensity
                    describe: function(name) {
                        return name === "comfortable" ? qsTr("Wider category rail and tabs. Terminal text is unchanged.") :
                               name === "compact" ? qsTr("Tighter categories and tabs. Terminal text is unchanged.") :
                               name === "minimal" ? qsTr("Smallest chrome. The terminal stays the main surface.") : ""
                    }
                    onPicked: function(name) { settings.densityChosen(name) }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: settings.paletteBorder
                }

                SectionLabel {
                    text: qsTr("Terminal font")
                }

                PlainLabel {
                    Layout.fillWidth: true
                    text: qsTr("Used by the terminal and by paths, shortcuts and states in the window.")
                    color: settings.paletteMuted
                    font.pixelSize: Math.max(12, settings.uiFont - 1)
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ComboBox {
                        id: familyChoice
                        objectName: "fontFamilyChoice"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 120
                        implicitHeight: Math.max(32, settings.uiFont + 18)
                        font.pixelSize: Math.max(12, settings.uiFont - 1)
                        // Index 0 is the platform default; the rest are installed
                        // fixed-pitch families. A configured name that is not
                        // installed is still listed so the selection stays visible.
                        model: {
                            const names = [""].concat(settings.fontFamilies)
                            if (settings.fontFamily.length > 0 && names.indexOf(settings.fontFamily) < 0)
                                names.splice(1, 0, settings.fontFamily)
                            return names
                        }
                        currentIndex: Math.max(0, model.indexOf(settings.fontFamily))
                        displayText: currentIndex <= 0 ? qsTr("System default · %1").arg(settings.resolvedFontFamily) :
                                                         currentText
                        onActivated: function(index) {
                            settings.fontFamilyChosen(model[index])
                            // Activation assigns currentIndex; follow the saved value again.
                            currentIndex = Qt.binding(() => Math.max(0, model.indexOf(settings.fontFamily)))
                        }

                        contentItem: PlainLabel {
                            text: familyChoice.displayText
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: familyChoice.font.pixelSize
                            color: settings.paletteText
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 10
                            rightPadding: 24
                        }
                        indicator: PlainLabel {
                            x: familyChoice.width - width - 8
                            y: (familyChoice.height - height) / 2
                            text: "▾"
                            color: settings.paletteMuted
                        }
                        background: ChoiceSurface {
                            hovered: familyChoice.hovered
                            pressed: familyChoice.pressed
                        }
                        delegate: ItemDelegate {
                            id: familyOption
                            required property var modelData
                            required property int index
                            width: familyChoice.width
                            hoverEnabled: true
                            // Each family previews in its own face.
                            contentItem: PlainLabel {
                                text: familyOption.modelData.length > 0 ? familyOption.modelData : qsTr("System default")
                                font.family: familyOption.modelData.length > 0 ? familyOption.modelData :
                                                                                 settings.resolvedFontFamily
                                font.pixelSize: familyChoice.font.pixelSize
                                color: settings.paletteText
                                elide: Text.ElideRight
                                leftPadding: 8
                            }
                            background: Rectangle {
                                color: familyOption.index === familyChoice.currentIndex ? settings.paletteSelected :
                                       familyOption.hovered ? settings.paletteHover : settings.paletteSurface
                            }
                        }
                        popup.background: Rectangle {
                            color: settings.paletteSurface
                            border.color: settings.paletteBorder
                        }
                        popup.onAboutToShow: {
                            const limit = Math.max(120, settings.height - 80)
                            familyChoice.popup.height = Math.min(familyChoice.popup.implicitHeight, limit)
                        }
                    }

                    StepButton {
                        objectName: "fontSmaller"
                        text: "−"
                        Accessible.name: qsTr("Smaller terminal text")
                        enabled: settings.fontSize > settings.fontSizeMinimum
                        onClicked: settings.fontSizeChosen(settings.fontSize - 1)
                    }
                    PlainLabel {
                        objectName: "fontSizeValue"
                        text: qsTr("%1 px").arg(settings.fontSize)
                        color: settings.paletteText
                        font.family: settings.resolvedFontFamily
                        font.pixelSize: Math.max(12, settings.uiFont - 1)
                        horizontalAlignment: Text.AlignHCenter
                        Layout.preferredWidth: Math.max(implicitWidth, settings.uiFont * 3.5)
                    }
                    StepButton {
                        objectName: "fontLarger"
                        text: "+"
                        Accessible.name: qsTr("Larger terminal text")
                        enabled: settings.fontSize < settings.fontSizeMaximum
                        onClicked: settings.fontSizeChosen(settings.fontSize + 1)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    PlainLabel {
                        objectName: "fontPreview"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: "~/lapis $ codex   0O 1lI {}[] ->"
                        color: settings.paletteText
                        font.family: settings.resolvedFontFamily
                        font.pixelSize: settings.fontSize
                        elide: Text.ElideRight
                    }
                    StepButton {
                        objectName: "fontReset"
                        text: qsTr("Default")
                        font.family: Qt.application.font.family
                        font.pixelSize: Math.max(12, settings.uiFont - 1)
                        enabled: settings.fontFamily.length > 0 || settings.fontSize !== settings.fontSizeDefault
                        onClicked: {
                            if (settings.fontFamily.length > 0)
                                settings.fontFamilyChosen("")
                            if (settings.fontSize !== settings.fontSizeDefault)
                                settings.fontSizeChosen(settings.fontSizeDefault)
                        }
                    }
                }

                PlainLabel {
                    objectName: "fontUnavailable"
                    Layout.fillWidth: true
                    visible: settings.fontFamily.length > 0 && settings.fontFamily !== settings.resolvedFontFamily
                    text: qsTr("%1 is not an installed fixed-width font; using %2.")
                          .arg(settings.fontFamily).arg(settings.resolvedFontFamily)
                    color: settings.paletteFault
                    wrapMode: Text.WordWrap
                    font.pixelSize: Math.max(12, settings.uiFont - 1)
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: settings.paletteBorder
                }

                SectionLabel {
                    text: qsTr("Configuration")
                }

                PlainLabel {
                    Layout.fillWidth: true
                    text: settings.configPath
                    color: settings.paletteMuted
                    font.family: settings.resolvedFontFamily
                    font.pixelSize: settings.readoutFont
                    elide: Text.ElideMiddle
                }
                PlainLabel {
                    Layout.fillWidth: true
                    visible: settings.configDiagnostic.length > 0
                    text: settings.configDiagnostic
                    color: settings.paletteText
                    wrapMode: Text.WordWrap
                    font.pixelSize: Math.max(11, settings.uiFont - 2)
                }
                PlainLabel {
                    Layout.fillWidth: true
                    text: qsTr("Changes are saved to this file immediately.")
                    color: settings.paletteMuted
                    font.pixelSize: Math.max(11, settings.uiFont - 2)
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
