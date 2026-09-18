#ifndef LAPIS_DESKTOP_UI_CAPTURE_HPP
#define LAPIS_DESKTOP_UI_CAPTURE_HPP
#include <QString>
class QQuickWindow;
namespace lapis::desktop {
class Workspace;
class UiPreview;
struct CaptureOptions {
    QString image_path;
    QString trace_path;
    QString scenario;
    int delay_ms{2000};
    bool smoke_input{};
};
void capture_window(QQuickWindow& window, Workspace& workspace, UiPreview& preview,
                    CaptureOptions options);
} // namespace lapis::desktop
#endif
