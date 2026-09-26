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
    required property bool alertSound
    required property bool finishSound
    required property int alertRepeat
    required property bool keepAwake
    required property bool showUsage
    // Notifications while lapis is in the background; the downloaded app's
    // login item and updates (absent in a developer build).
    property bool notify: true
    property bool loginAvailable: false
    property bool launchAtLogin: false
    property bool updatesAvailable: false
    // Every action that has a key: {label, keys}.
    property var shortcutRows: []

    signal themeChosen(string name)
    signal densityChosen(string name)
    signal fontFamilyChosen(string name)
    signal fontSizeChosen(int pixels)
    signal alertSoundChosen(bool on)
    signal finishSoundChosen(bool on)
    signal alertRepeatChosen(int times)
    signal keepAwakeChosen(bool on)
    signal showUsageChosen(bool on)
    signal notifyChosen(bool on)
    signal launchAtLoginChosen(bool on)
    signal checkUpdates()
    signal chimePlayed(bool needsYou)

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

    // A setting that is on or off, with a line on what it does and an
    // optional Play button to hear it.
    component SwitchRow: RowLayout {
        id: switchRow
        required property string label
        required property string detail
        required property bool on
        property bool playable: false
        signal toggled(bool on)
        signal played()
        spacing: 10
        Layout.fillWidth: true
        ColumnLayout {
            spacing: 2
            Layout.fillWidth: true
            PlainLabel {
                text: switchRow.label
                color: settings.paletteText
                font.pixelSize: settings.uiFont
            }
            PlainLabel {
                Layout.fillWidth: true
                text: switchRow.detail
                color: settings.paletteMuted
                font.pixelSize: Math.max(12, settings.uiFont - 1)
                wrapMode: Text.WordWrap
            }
        }
        StepButton {
            visible: switchRow.playable
            objectName: "play-" + switchRow.label
            text: qsTr("Play")
            onClicked: switchRow.played()
        }
        Button {
            id: toggle
            objectName: "switch-" + switchRow.label
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            text: switchRow.on ? qsTr("On") : qsTr("Off")
            font.pixelSize: Math.max(12, settings.uiFont - 1)
            implicitWidth: Math.max(56, implicitContentWidth + 24)
            implicitHeight: Math.max(32, settings.uiFont + 18)
            onClicked: switchRow.toggled(!switchRow.on)
            contentItem: PlainLabel {
                text: toggle.text
                font: toggle.font
                color: switchRow.on ? settings.paletteText : settings.paletteMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: ChoiceSurface {
                marked: switchRow.on
                hovered: toggle.hovered
                pressed: toggle.down
            }
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
                    // Font matching ignores case, so "menlo" resolves to Menlo.
                    visible: settings.fontFamily.length > 0 &&
                             settings.fontFamily.toLowerCase() !== settings.resolvedFontFamily.toLowerCase()
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
                    text: qsTr("Alerts")
                }

                SwitchRow {
                    label: qsTr("Chime when an agent needs you")
                    detail: qsTr("Two taps, then again every few seconds while the request waits and you are looking elsewhere.")
                    on: settings.alertSound
                    playable: true
                    onToggled: function(on) { settings.alertSoundChosen(on) }
                    onPlayed: settings.chimePlayed(true)
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    enabled: settings.alertSound
                    PlainLabel {
                        Layout.fillWidth: true
                        text: settings.alertRepeat === 1 ? qsTr("Chime once") : qsTr("Chime up to %1 times").arg(settings.alertRepeat)
                        color: settings.alertSound ? settings.paletteText : settings.paletteMuted
                        font.pixelSize: Math.max(12, settings.uiFont - 1)
                    }
                    StepButton {
                        objectName: "repeatDown"
                        text: "−"
                        enabled: settings.alertRepeat > 1
                        onClicked: settings.alertRepeatChosen(settings.alertRepeat - 1)
                    }
                    StepButton {
                        objectName: "repeatUp"
                        text: "+"
                        enabled: settings.alertRepeat < 10
                        onClicked: settings.alertRepeatChosen(settings.alertRepeat + 1)
                    }
                }

                SwitchRow {
                    label: qsTr("Chime when a turn finishes")
                    detail: qsTr("Once, quietly, when a Codex or Claude turn ends out of view.")
                    on: settings.finishSound
                    playable: true
                    onToggled: function(on) { settings.finishSoundChosen(on) }
                    onPlayed: settings.chimePlayed(false)
                }

                SwitchRow {
                    objectName: "notifyRow"
                    label: qsTr("Notify from the background")
                    detail: qsTr("A notification for the same moments while lapis is not the app in front; clicking it shows the agent.")
                    on: settings.notify
                    onToggled: function(on) { settings.notifyChosen(on) }
                }

                SwitchRow {
                    objectName: "launchAtLoginRow"
                    visible: settings.loginAvailable
                    label: qsTr("Keep agents running at login")
                    detail: qsTr("At login lapis restarts agents that were running, resuming their conversations, and keeps them until you open it.")
                    on: settings.launchAtLogin
                    onToggled: function(on) { settings.launchAtLoginChosen(on) }
                }

                RowLayout {
                    objectName: "updatesRow"
                    visible: settings.updatesAvailable
                    Layout.fillWidth: true
                    spacing: 8
                    PlainLabel {
                        Layout.fillWidth: true
                        text: qsTr("lapis checks for updates once a day.")
                        color: settings.paletteMuted
                        font.pixelSize: Math.max(12, settings.uiFont - 1)
                        wrapMode: Text.WordWrap
                    }
                    StepButton {
                        objectName: "checkUpdates"
                        text: qsTr("Check now")
                        onClicked: settings.checkUpdates()
                    }
                }

                SwitchRow {
                    label: qsTr("Keep this Mac awake")
                    detail: qsTr("While it is plugged in, so the phone can reach your agents. The display still sleeps; closing the lid still sleeps.")
                    on: settings.keepAwake
                    onToggled: function(on) { settings.keepAwakeChosen(on) }
                }

                SwitchRow {
                    objectName: "showUsageRow"
                    label: qsTr("Show plan usage")
                    detail: qsTr("Each signed-in plan under the categories (Codex, Claude, Grok, Kimi, and OMP's accounts), checked every five minutes, with a dashboard per machine. usage.meter and usage.machines in the config pick which. Off, lapis asks no CLI.")
                    on: settings.showUsage
                    onToggled: function(on) { settings.showUsageChosen(on) }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: settings.paletteBorder
                }

                SectionLabel {
                    text: qsTr("Keyboard shortcuts")
                }
                GridLayout {
                    objectName: "shortcutList"
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 4
                    Repeater {
                        model: settings.shortcutRows
                        delegate: PlainLabel {
                            required property var modelData
                            required property int index
                            Layout.row: Math.floor(index)
                            Layout.column: 0
                            Layout.fillWidth: true
                            text: modelData.label
                            color: settings.paletteText
                            elide: Text.ElideRight
                        }
                    }
                    Repeater {
                        model: settings.shortcutRows
                        delegate: PlainLabel {
                            required property var modelData
                            required property int index
                            Layout.row: Math.floor(index)
                            Layout.column: 1
                            Layout.alignment: Qt.AlignRight
                            text: modelData.keys
                            color: settings.paletteMuted
                            font.family: settings.resolvedFontFamily
                            font.pixelSize: settings.readoutFont
                        }
                    }
                }
                PlainLabel {
                    Layout.fillWidth: true
                    text: qsTr("Change any of them under \"keybindings\" in the file below, with the action's name and up to four keys, for example \"toggleTerminal\": [\"Meta+`\"].")
                    color: settings.paletteMuted
                    font.pixelSize: Math.max(11, settings.uiFont - 2)
                    wrapMode: Text.WordWrap
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
                    text: qsTr("Changes are saved to this file immediately, and edits to it (by you or an agent) apply at once. Defaults for new agents (newAgent: CLI, folder, per-machine folders, models) live there too.")
                    color: settings.paletteMuted
                    font.pixelSize: Math.max(11, settings.uiFont - 2)
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
