#ifndef LAPIS_DESKTOP_MEDIA_ACTIONS_HPP
#define LAPIS_DESKTOP_MEDIA_ACTIONS_HPP
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <functional>
#include <memory>

class QMimeData;
class QQuickItem;

namespace lapis::desktop {
// What the visual view does with a file it shows (a picture, a player or a
// card): open it, show it in the Finder, put it on the clipboard, save a copy
// and drag it out to another app. QML sees it as `mediaActions`. Paths are
// local files; a file URL is taken too. Nothing here changes the file.
class MediaActions final : public QObject {
    Q_OBJECT
  public:
    explicit MediaActions(QObject* parent = nullptr);
    ~MediaActions() override;

    // What another app receives for `path`, on the clipboard or in a drag:
    // the file's URL (text/uri-list, which Finder, Discord, Slack, Mail and
    // browser uploads take as the file) and, for a picture, its pixels, so
    // an app that pastes images gets the image. Null for a missing file.
    [[nodiscard]] static std::unique_ptr<QMimeData> mime_data(const QString& path, bool picture);

    // The file's default app, as a click does.
    Q_INVOKABLE bool open(const QString& path) const;
    // The Finder with the file selected (`open -R`).
    Q_INVOKABLE bool reveal(const QString& path) const;
    // The picture and its file on the clipboard (Copy Image).
    Q_INVOKABLE bool copyImage(const QString& path) const;
    // The file alone on the clipboard, as the Finder copies one (Copy File).
    Q_INVOKABLE bool copyFile(const QString& path) const;
    // The path as plain text (Copy Path).
    Q_INVOKABLE bool copyPath(const QString& path) const;
    // Copies the file to `destination` (a path or file URL from a save
    // panel), replacing a file already there; the original is untouched.
    Q_INVOKABLE bool saveCopy(const QString& path, const QVariant& destination) const;
    // Starts a drag of the file out of the window from `source`, with a small
    // thumbnail under the pointer for a picture. Returns once it is dropped
    // or abandoned; true when something took it.
    Q_INVOKABLE bool drag(const QString& path, bool picture, QQuickItem* source);
    // For a picture that can be see-through: "light" when what it draws is
    // mostly light (white strokes for a dark page), "dark" when mostly dark,
    // else empty. The view puts such a picture on a card that keeps it
    // readable on a page of the same tone. Read once per file version.
    Q_INVOKABLE [[nodiscard]] QString inkTone(const QString& path) const;
    // Where a save panel first suggests a copy: the file's name in Downloads.
    Q_INVOKABLE [[nodiscard]] static QUrl saveSuggestion(const QString& path);

    // Tests keep the system clipboard, the Finder and drags out of reach.
    using ClipboardSink = std::function<void(std::unique_ptr<QMimeData>)>;
    using Launcher = std::function<bool(const QString& action, const QString& path)>;
    using Dragger = std::function<bool(std::unique_ptr<QMimeData>, const QImage& thumbnail)>;
    void setClipboardForTesting(ClipboardSink sink) { clipboard_ = std::move(sink); }
    void setLauncherForTesting(Launcher launcher) { launcher_ = std::move(launcher); }
    void setDraggerForTesting(Dragger dragger) { dragger_ = std::move(dragger); }

  private:
    void publish(std::unique_ptr<QMimeData> data) const;
    ClipboardSink clipboard_;
    Launcher launcher_;
    Dragger dragger_;
    mutable QHash<QString, QString> tones_; // path + modified time -> tone
};
// A local path for a path or a file URL (text or QUrl), or empty.
[[nodiscard]] QString media_local_path(const QVariant& value);
} // namespace lapis::desktop
#endif
