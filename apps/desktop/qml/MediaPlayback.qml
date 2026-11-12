import QtQuick
import QtMultimedia

// Audio or video named in a message, played in place in the visual view
// through the system's player (AVFoundation). The view loads this file
// separately, so a build without Qt Multimedia still shows the rest of the
// thread and falls back to a card that opens the file in its app.
Item {
    id: playback
    objectName: "playback"
    // For review tools: what the system player found in the file.
    readonly property int duration: player.duration
    readonly property string problem: player.errorString
    readonly property int status: player.mediaStatus
    property int frames: 0 // video frames decoded
    Connections {
        target: output.videoSink
        function onVideoFrameChanged() { ++playback.frames }
    }
    readonly property size frame: Qt.size(output.sourceRect.width, output.sourceRect.height)
    // The visual view, for its colors, fonts and sizes.
    property var view
    // {url, kind, name, type, size} from TranscriptModel.media().
    property var media
    // The column's width.
    property real room
    readonly property bool video: media !== undefined && media !== null && media.kind === "video"
    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState
    readonly property bool failed: player.error !== MediaPlayer.NoError
    // Dragging along a line seeks; the view does not take it for dragging
    // the file out.
    readonly property bool seeking: videoScrub.seeking || audioScrub.seeking
    // The frame's own size once the file is open; 16:9 until then.
    readonly property real frameWidth: output.sourceRect.width > 0 ? output.sourceRect.width : 16
    readonly property real frameHeight: output.sourceRect.height > 0 ? output.sourceRect.height : 9

    width: video ? Math.min(room, output.sourceRect.width > 0 ? output.sourceRect.width : room)
                 : Math.min(room, 440)
    height: video ? Math.round(width * frameHeight / frameWidth) : 58

    function clock(ms) {
        const s = Math.max(0, Math.floor(ms / 1000))
        const m = Math.floor(s / 60)
        const h = Math.floor(m / 60)
        const pad = n => (n < 10 ? "0" : "") + n
        return h > 0 ? h + ":" + pad(m % 60) + ":" + pad(s % 60) : m + ":" + pad(s % 60)
    }
    function toggle() {
        if (failed) {
            Qt.openUrlExternally(media.url)
            return
        }
        if (playing)
            player.pause()
        else {
            if (player.duration > 0 && player.position >= player.duration - 50)
                player.position = 0
            player.play()
        }
    }

    MediaPlayer {
        id: player
        source: playback.media ? playback.media.url : ""
        audioOutput: AudioOutput {}
        videoOutput: playback.video ? output : null
        // Paused at the start shows a video's first frame instead of black.
        onMediaStatusChanged: if (mediaStatus === MediaPlayer.LoadedMedia && playback.video
                                  && playbackState === MediaPlayer.StoppedState)
                                  pause()
    }

    // Play and pause, drawn so they sit centered at any size.
    component Glyph: Canvas {
        property bool pausing: false
        property color color: "white"
        onPausingChanged: requestPaint()
        onColorChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const g = getContext("2d")
            g.reset()
            g.fillStyle = color
            const s = Math.min(width, height)
            const x = (width - s) / 2, y = (height - s) / 2
            if (pausing) {
                g.fillRect(x + s * 0.2, y + s * 0.12, s * 0.2, s * 0.76)
                g.fillRect(x + s * 0.6, y + s * 0.12, s * 0.2, s * 0.76)
            } else {
                g.beginPath()
                g.moveTo(x + s * 0.24, y + s * 0.1)
                g.lineTo(x + s * 0.9, y + s * 0.5)
                g.lineTo(x + s * 0.24, y + s * 0.9)
                g.closePath()
                g.fill()
            }
        }
    }

    // A thin line that fills as it plays; click or drag along it to seek.
    component Scrub: Item {
        id: scrub
        property color track
        property color fill
        readonly property bool seeking: scrubMouse.pressed
        implicitHeight: 14
        readonly property real done: player.duration > 0 ? player.position / player.duration : 0
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: scrubMouse.containsMouse || scrubMouse.pressed ? 5 : 3
            radius: height / 2
            color: scrub.track
            Behavior on height { NumberAnimation { duration: 90 } }
            Rectangle {
                width: parent.width * scrub.done
                height: parent.height
                radius: parent.radius
                color: scrub.fill
            }
        }
        MouseArea {
            id: scrubMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            enabled: player.seekable && player.duration > 0
            function seek(x) { player.position = Math.max(0, Math.min(1, x / width)) * player.duration }
            onPressed: mouse => seek(mouse.x)
            onPositionChanged: mouse => { if (pressed) seek(mouse.x) }
        }
    }

    // Video: the frame at the column's width, a play button over it while
    // stopped, and the controls along its bottom while the pointer is on it.
    Rectangle {
        visible: playback.video
        anchors.fill: parent
        radius: 6
        color: "black"
        clip: true
        VideoOutput {
            id: output
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
        }
        MouseArea {
            id: frameMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: playback.toggle()
        }
        Rectangle {
            anchors.centerIn: parent
            visible: !playback.playing
            width: 54
            height: 54
            radius: 27
            // Light enough to read on a black frame, dark enough on a bright one.
            color: Qt.rgba(0.25, 0.25, 0.25, 0.7)
            border.color: Qt.rgba(1, 1, 1, 0.25)
            Glyph {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2
                width: 22
                height: 22
                visible: !playback.failed
            }
            Text {
                anchors.centerIn: parent
                visible: playback.failed
                text: "↗"
                color: "white"
                font.pixelSize: 22
            }
        }
        Rectangle {
            id: controls
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 34
            color: Qt.rgba(0, 0, 0, 0.6)
            opacity: (frameMouse.containsMouse || controlsMouse.containsMouse) && player.duration > 0 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
            MouseArea { id: controlsMouse; anchors.fill: parent; hoverEnabled: true }
            Glyph {
                id: videoPlay
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                pausing: playback.playing
                MouseArea { anchors.fill: parent; anchors.margins: -8; cursorShape: Qt.PointingHandCursor; onClicked: playback.toggle() }
            }
            Text {
                id: videoClock
                anchors.left: videoPlay.right
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: playback.clock(player.position) + " / " + playback.clock(player.duration)
                color: "white"
                font.family: playback.view ? playback.view.monoFamily : ""
                font.pixelSize: playback.view ? playback.view.codeSize - 1 : 12
            }
            Scrub {
                id: videoScrub
                anchors.left: videoClock.right
                anchors.leftMargin: 12
                anchors.right: videoOpen.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                track: Qt.rgba(1, 1, 1, 0.3)
                fill: "white"
            }
            Text {
                id: videoOpen
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "↗"
                color: "white"
                font.pixelSize: playback.view ? playback.view.stepSize : 14
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Qt.openUrlExternally(playback.media.url)
                }
            }
        }
    }

    // Audio: a card with a play button, the file's name, and a line to seek
    // along with the time.
    Rectangle {
        visible: !playback.video
        anchors.fill: parent
        radius: playback.view ? playback.view.radius - 2 : 6
        color: playback.view ? playback.view.raisedColor : "#24283b"
        Rectangle {
            id: audioButton
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 34
            height: 34
            radius: 17
            color: playback.view ? playback.view.accentColor : "#7aa2f7"
            Glyph {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: playback.playing ? 0 : 1
                width: 13
                height: 13
                pausing: playback.playing
                color: playback.view ? playback.view.backgroundColor : "black"
                visible: !playback.failed
            }
            Text {
                anchors.centerIn: parent
                visible: playback.failed
                text: "↗"
                color: playback.view ? playback.view.backgroundColor : "black"
                font.pixelSize: 15
            }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: playback.toggle() }
        }
        Text {
            id: audioName
            anchors.left: audioButton.right
            anchors.leftMargin: 12
            anchors.right: audioClock.left
            anchors.rightMargin: 10
            y: 10
            text: playback.media ? playback.media.name : ""
            elide: Text.ElideMiddle
            color: playback.view ? playback.view.textColor : "white"
            font.family: playback.view ? playback.view.sansFamily : ""
            font.pixelSize: playback.view ? playback.view.stepSize : 14
        }
        Text {
            id: audioClock
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.baseline: audioName.baseline
            text: playback.failed ? qsTr("opens in its app")
                : player.duration > 0 ? playback.clock(player.position) + " / " + playback.clock(player.duration)
                : ""
            color: playback.view ? playback.view.mutedTextColor : "gray"
            font.family: playback.view ? playback.view.monoFamily : ""
            font.pixelSize: playback.view ? playback.view.codeSize - 1 : 12
        }
        Scrub {
            id: audioScrub
            anchors.left: audioName.left
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            track: playback.view ? playback.view.tint(playback.view.textColor, 0.15) : "gray"
            fill: playback.view ? playback.view.accentColor : "white"
        }
    }
}
