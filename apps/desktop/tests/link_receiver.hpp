#ifndef LAPIS_TEST_LINK_RECEIVER_HPP
#define LAPIS_TEST_LINK_RECEIVER_HPP
#include <QDesktopServices>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

// Intercept the OS handoff: exercise QDesktopServices without launching apps.
class LinkReceiver : public QObject {
    Q_OBJECT
  public:
    QList<QUrl> urls;
    LinkReceiver() {
        for (const auto* scheme : {"http", "https", "file"})
            QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), this, "receive");
    }
    ~LinkReceiver() override {
        for (const auto* scheme : {"http", "https", "file"})
            QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
    }
  public slots:
    void receive(const QUrl& url) { urls.append(url); }
};

#endif
