#include "media_actions.hpp"
#include "platform_desktop.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QQuickItem>
#include <QStandardPaths>
#include <QUrl>

namespace lapis::desktop {
namespace {
constexpr int kThumbnail = 96; // the drag's picture under the pointer, in points
constexpr int kToneSample = 64;

// The file's pixels, at most `longest` on a side when it is given (0 reads
// them whole); null when it is not a picture Qt can read.
QImage read_picture(const QString& path, int longest) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (longest > 0) {
        const auto size = reader.size();
        if (size.isValid() && (size.width() > longest || size.height() > longest))
            reader.setScaledSize(size.scaled(longest, longest, Qt::KeepAspectRatio));
        else if (!size.isValid())
            reader.setScaledSize(QSize(longest, longest)); // an SVG without a size
    }
    return reader.read();
}

// A small card with the file's name, for a file that is not a picture.
QImage name_card(const QString& name) {
    QImage card(kThumbnail * 2, kThumbnail / 2, QImage::Format_ARGB32_Premultiplied);
    card.fill(Qt::transparent);
    QPainter painter(&card);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(40, 40, 44, 230));
    painter.setPen(QColor(255, 255, 255, 40));
    painter.drawRoundedRect(QRectF(card.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    QFont font;
    font.setPixelSize(13);
    painter.setFont(font);
    painter.setPen(QColor(235, 235, 235));
    const auto text = painter.fontMetrics().elidedText(name, Qt::ElideMiddle, card.width() - 20);
    painter.drawText(card.rect().adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    return card;
}
} // namespace

QString media_local_path(const QVariant& value) {
    QString text = value.metaType() == QMetaType::fromType<QUrl>() ? value.toUrl().toString()
                                                                   : value.toString();
    if (text.startsWith(QLatin1String("file:")))
        text = QUrl(text).toLocalFile();
    else if (text.startsWith(QLatin1String("~/")))
        text = QDir::home().filePath(text.mid(2));
    if (text.isEmpty() || !QDir::isAbsolutePath(text))
        return {};
    return QDir::cleanPath(text);
}

MediaActions::MediaActions(QObject* parent) : QObject(parent) {}
MediaActions::~MediaActions() = default;

std::unique_ptr<QMimeData> MediaActions::mime_data(const QString& path, bool picture) {
    const QFileInfo info(media_local_path(path));
    if (!info.isFile())
        return nullptr;
    auto data = std::make_unique<QMimeData>();
    data->setUrls({QUrl::fromLocalFile(info.absoluteFilePath())});
    if (picture) {
        const auto image = read_picture(info.absoluteFilePath(), 0);
        if (!image.isNull())
            data->setImageData(image);
    }
    return data;
}

void MediaActions::publish(std::unique_ptr<QMimeData> data) const {
    if (clipboard_) {
        clipboard_(std::move(data));
        return;
    }
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setMimeData(data.release()); // the clipboard owns it
}

bool MediaActions::open(const QString& path) const {
    const auto local = media_local_path(path);
    if (!QFileInfo::exists(local))
        return false;
    if (launcher_)
        return launcher_(QStringLiteral("open"), local);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(local));
}

bool MediaActions::reveal(const QString& path) const {
    const auto local = media_local_path(path);
    if (!QFileInfo::exists(local))
        return false;
    if (launcher_)
        return launcher_(QStringLiteral("reveal"), local);
    // Where the file manager cannot select it, its folder opens.
    return platform::reveal_in_file_manager(local) ||
           QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(local).absolutePath()));
}

bool MediaActions::copyImage(const QString& path) const {
    auto data = mime_data(path, true);
    if (!data)
        return false;
    publish(std::move(data));
    return true;
}

bool MediaActions::copyFile(const QString& path) const {
    auto data = mime_data(path, false);
    if (!data)
        return false;
    publish(std::move(data));
    return true;
}

bool MediaActions::copyPath(const QString& path) const {
    const auto local = media_local_path(path);
    if (local.isEmpty())
        return false;
    auto data = std::make_unique<QMimeData>();
    data->setText(local);
    publish(std::move(data));
    return true;
}

bool MediaActions::saveCopy(const QString& path, const QVariant& destination) const {
    const auto source = media_local_path(path);
    const auto target = media_local_path(destination);
    if (!QFileInfo(source).isFile() || target.isEmpty())
        return false;
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(source).absoluteFilePath())
        return true;
    // The save panel already asked before replacing a file.
    if (QFileInfo::exists(target) && !QFile::remove(target))
        return false;
    return QFile::copy(source, target);
}

bool MediaActions::drag(const QString& path, bool picture, QQuickItem* source) {
    auto data = mime_data(path, picture);
    if (!data)
        return false;
    QImage thumbnail;
    if (picture)
        thumbnail = read_picture(media_local_path(path), kThumbnail * 2);
    if (thumbnail.isNull())
        thumbnail = name_card(QFileInfo(media_local_path(path)).fileName());
    if (dragger_)
        return dragger_(std::move(data), thumbnail);
    auto* drag = new QDrag(source != nullptr ? static_cast<QObject*>(source) : this);
    drag->setMimeData(data.release());
    auto pixmap = QPixmap::fromImage(thumbnail);
    pixmap.setDevicePixelRatio(2.0); // drawn at twice its size, sharp on Retina
    drag->setPixmap(pixmap);
    drag->setHotSpot(
        QPoint(static_cast<int>(pixmap.width() / 4), static_cast<int>(pixmap.height() / 4)));
    // exec runs until the drop; the drag deletes itself afterwards.
    return drag->exec(Qt::CopyAction, Qt::CopyAction) != Qt::IgnoreAction;
}

QString MediaActions::inkTone(const QString& path) const {
    const QFileInfo info(media_local_path(path));
    if (!info.isFile())
        return {};
    const auto key = info.absoluteFilePath() + QLatin1Char('|') +
                     QString::number(info.lastModified().toMSecsSinceEpoch());
    if (const auto known = tones_.constFind(key); known != tones_.constEnd())
        return known.value();
    const auto image =
        read_picture(info.absoluteFilePath(), kToneSample).convertToFormat(QImage::Format_ARGB32);
    QString tone;
    if (!image.isNull()) {
        // The alpha-weighted luminance of what it draws, and how much of it
        // is see-through at all: a picture that is opaque nearly everywhere
        // brings its own background.
        double ink = 0;
        double weight = 0;
        qsizetype clear = 0;
        for (int y = 0; y < image.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const double alpha = qAlpha(row[x]) / 255.0;
                if (alpha < 0.9)
                    ++clear;
                if (alpha <= 0.05)
                    continue;
                ink += alpha *
                       (0.2126 * qRed(row[x]) + 0.7152 * qGreen(row[x]) + 0.0722 * qBlue(row[x])) /
                       255.0;
                weight += alpha;
            }
        }
        const double pixels = static_cast<double>(image.width()) * image.height();
        if (weight > 0 && pixels > 0 && static_cast<double>(clear) / pixels > 0.1) {
            const double luma = ink / weight;
            tone = luma > 0.6   ? QStringLiteral("light")
                   : luma < 0.4 ? QStringLiteral("dark")
                                : QString();
        }
    }
    if (tones_.size() > 512)
        tones_.clear();
    tones_.insert(key, tone);
    return tone;
}

QUrl MediaActions::saveSuggestion(const QString& path) {
    auto folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (folder.isEmpty())
        folder = QDir::homePath();
    return QUrl::fromLocalFile(QDir(folder).filePath(QFileInfo(media_local_path(path)).fileName()));
}
} // namespace lapis::desktop
