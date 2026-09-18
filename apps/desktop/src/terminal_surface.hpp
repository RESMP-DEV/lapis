#ifndef LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
#define LAPIS_DESKTOP_TERMINAL_SURFACE_HPP

#include "workspace.hpp"

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QQuickItem>

namespace lapis::desktop {

class TerminalSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(lapis::desktop::SessionPreview* document READ document WRITE setDocument NOTIFY
                   documentChanged)
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive NOTIFY interactiveChanged)
  public:
    explicit TerminalSurface(QQuickItem* parent = nullptr);
    [[nodiscard]] SessionPreview* document() const { return document_.data(); }
    void setDocument(SessionPreview* document);
    [[nodiscard]] bool interactive() const { return interactive_; }
    void setInteractive(bool enabled);
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
  signals:
    void documentChanged();
    void interactiveChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

  private:
    void requestResize();
    QPointer<SessionPreview> document_;
    bool content_dirty_{true};
    bool interactive_{};
    QString preedit_;
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
