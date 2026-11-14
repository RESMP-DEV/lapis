// What the visual view hands other apps for a file it shows: the mime data a
// drag or a copy carries, the Finder and open requests, and saved copies.
// The system clipboard, the Finder and drags are replaced by recorders, so
// nothing leaves the process.
#include "media_actions.hpp"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using lapis::desktop::MediaActions;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QString write_picture(const QTemporaryDir& folder, const char* name, QColor ink, bool clear) {
    QImage image(40, 30, QImage::Format_ARGB32);
    image.fill(clear ? Qt::transparent : QColor(Qt::black));
    for (int x = 4; x < 36; ++x)
        for (int y = 12; y < 16; ++y)
            image.setPixelColor(x, y, ink); // a stroke across it
    const auto path = folder.filePath(QString::fromLatin1(name));
    require(image.save(path), "picture written");
    return path;
}

void mime_data(const QTemporaryDir& folder) {
    const auto picture = write_picture(folder, "diagram.png", Qt::white, true);
    const auto data = MediaActions::mime_data(picture, true);
    require(data != nullptr, "a picture has mime data");
    require(data->urls() == QList<QUrl>{QUrl::fromLocalFile(picture)}, "the file's URL");
    require(data->hasFormat(QStringLiteral("text/uri-list")), "as text/uri-list");
    require(data->hasImage(), "a picture carries its pixels");
    require(qvariant_cast<QImage>(data->imageData()).size() == QSize(40, 30), "at its size");
    require(!data->formats().contains(QStringLiteral("text/plain")),
            "no path text, so a chat attaches rather than pastes the path");

    const auto file_url = QUrl::fromLocalFile(picture).toString();
    require(MediaActions::mime_data(file_url, true) != nullptr, "a file URL is taken too");

    QFile pdf(folder.filePath(QStringLiteral("report.pdf")));
    require(pdf.open(QIODevice::WriteOnly) && pdf.write("%PDF-1.4\n") > 0, "file written");
    pdf.close();
    const auto card = MediaActions::mime_data(pdf.fileName(), false);
    require(card != nullptr && card->urls().size() == 1 && !card->hasImage(), "a file: URL only");
    require(MediaActions::mime_data(folder.filePath(QStringLiteral("gone.png")), true) == nullptr,
            "nothing for a missing file");
    require(MediaActions::mime_data(QStringLiteral("relative.png"), true) == nullptr,
            "nothing for a relative path");
}

void clipboard_and_finder(const QTemporaryDir& folder) {
    const auto picture = write_picture(folder, "chart.png", Qt::black, false);
    MediaActions actions;
    std::vector<std::unique_ptr<QMimeData>> copied;
    actions.setClipboardForTesting(
        [&copied](std::unique_ptr<QMimeData> data) { copied.push_back(std::move(data)); });
    std::vector<std::pair<QString, QString>> launched;
    actions.setLauncherForTesting([&launched](const QString& action, const QString& path) {
        launched.emplace_back(action, path);
        return true;
    });

    require(actions.copyImage(picture), "Copy Image");
    require(copied.back()->hasImage() && copied.back()->urls().size() == 1,
            "Copy Image puts the picture and its file on the clipboard");
    require(actions.copyFile(picture), "Copy File");
    require(!copied.back()->hasImage() &&
                copied.back()->urls() == QList<QUrl>{QUrl::fromLocalFile(picture)},
            "Copy File puts only the file there");
    require(actions.copyPath(QUrl::fromLocalFile(picture).toString()), "Copy Path");
    require(copied.back()->text() == picture && copied.back()->urls().isEmpty(),
            "Copy Path copies the path as text");
    require(!actions.copyImage(folder.filePath(QStringLiteral("gone.png"))) && copied.size() == 3,
            "a missing file copies nothing");

    require(actions.reveal(picture) &&
                launched.back() == std::pair(QStringLiteral("reveal"), picture),
            "Show in Finder selects the file");
    require(actions.open(picture) && launched.back() == std::pair(QStringLiteral("open"), picture),
            "Open opens it");
    require(!actions.reveal(folder.filePath(QStringLiteral("gone.png"))) && launched.size() == 2,
            "a missing file reveals nothing");

    // A drag carries the same data as Copy Image, with a thumbnail.
    std::unique_ptr<QMimeData> dragged;
    QImage thumbnail;
    actions.setDraggerForTesting([&](std::unique_ptr<QMimeData> data, const QImage& image) {
        dragged = std::move(data);
        thumbnail = image;
        return true;
    });
    require(actions.drag(picture, true, nullptr), "a picture drags");
    require(dragged && dragged->hasImage() && dragged->urls().size() == 1, "the drag's data");
    require(!thumbnail.isNull() && thumbnail.width() <= 192 && thumbnail.height() <= 192,
            "a small thumbnail");
    QFile notes(folder.filePath(QStringLiteral("notes.html")));
    require(notes.open(QIODevice::WriteOnly) && notes.write("<p>hi</p>") > 0, "file written");
    notes.close();
    require(actions.drag(notes.fileName(), false, nullptr) && !dragged->hasImage() &&
                !thumbnail.isNull(),
            "a file drags as its URL with a name card");
}

void save_copy(const QTemporaryDir& folder) {
    const auto picture = write_picture(folder, "plot.png", Qt::red, false);
    MediaActions actions;
    const auto target = folder.filePath(QStringLiteral("saved/plot copy.png"));
    require(QDir().mkpath(folder.filePath(QStringLiteral("saved"))), "folder made");
    require(actions.saveCopy(picture, QUrl::fromLocalFile(target)), "Save As writes a copy");
    QFile original(picture);
    QFile copy(target);
    require(original.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly) &&
                original.readAll() == copy.readAll(),
            "the copy matches");
    copy.close();
    // Saving over a file the panel agreed to replace.
    const auto other = write_picture(folder, "other.png", Qt::blue, false);
    require(actions.saveCopy(other, target), "Save As replaces");
    require(QImage(target).pixelColor(10, 13) == QColor(Qt::blue), "with the new file");
    require(QFile::exists(picture), "the original stays");
    const QFile::Permissions readable = QFile::permissions(picture);
    QFile::setPermissions(picture, QFileDevice::Permissions());
    const bool unreadable_save = actions.saveCopy(picture, target);
    QFile::setPermissions(picture, readable);
    require(!unreadable_save && QImage(target).pixelColor(10, 13) == QColor(Qt::blue),
            "a failed replacement leaves the saved file");
    require(!actions.saveCopy(folder.filePath(QStringLiteral("gone.png")), target),
            "a missing file saves nothing");
    require(MediaActions::saveSuggestion(picture).fileName() == QStringLiteral("plot.png"),
            "the panel suggests the file's name");
}

void ink_tone(const QTemporaryDir& folder) {
    MediaActions actions;
    require(actions.inkTone(write_picture(folder, "white.png", Qt::white, true)) ==
                QStringLiteral("light"),
            "white strokes on nothing are light ink");
    require(actions.inkTone(write_picture(folder, "black.png", Qt::black, true)) ==
                QStringLiteral("dark"),
            "black strokes are dark ink");
    require(actions.inkTone(write_picture(folder, "solid.png", Qt::white, false)).isEmpty(),
            "an opaque picture brings its own background");
    require(actions.inkTone(folder.filePath(QStringLiteral("gone.png"))).isEmpty(),
            "a missing file has no tone");
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "temporary folder");
        mime_data(folder);
        clipboard_and_finder(folder);
        save_copy(folder);
        ink_tone(folder);
    } catch (const std::exception& error) {
        std::cerr << "media_actions_test: FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "media_actions_test: PASS\n";
    return 0;
}
