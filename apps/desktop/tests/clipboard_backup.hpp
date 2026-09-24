#ifndef LAPIS_TEST_CLIPBOARD_BACKUP_HPP
#define LAPIS_TEST_CLIPBOARD_BACKUP_HPP
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <memory>
namespace lapis::desktop::test {
// All MIME formats survive a fixture, including failure unwinding.
struct ClipboardBackup {
    std::unique_ptr<QMimeData> saved{std::make_unique<QMimeData>()};
    ClipboardBackup() {
        if (const auto* current = QGuiApplication::clipboard()->mimeData())
            for (const auto& type : current->formats())
                saved->setData(type, current->data(type));
    }
    ~ClipboardBackup() { QGuiApplication::clipboard()->setMimeData(saved.release()); }
};
} // namespace lapis::desktop::test
#endif
