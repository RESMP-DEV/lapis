#ifndef LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
#define LAPIS_DESKTOP_TERMINAL_SURFACE_HPP

#include "workspace.hpp"

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <mutex>

namespace lapis::desktop {

// Legacy printable-key encoding; native text/IME remains Unicode.
[[nodiscard]] QByteArray terminal_text_key(const QKeyEvent& event);

class TerminalSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(lapis::desktop::SessionPreview* document READ document WRITE setDocument NOTIFY
                   documentChanged)
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive NOTIFY interactiveChanged)
    Q_PROPERTY(lapis::desktop::Workspace* focusWorkspace READ focusWorkspace WRITE setFocusWorkspace
                   NOTIFY focusWorkspaceChanged)
  public:
    explicit TerminalSurface(QQuickItem* parent = nullptr);
    ~TerminalSurface() override;
    [[nodiscard]] SessionPreview* document() const { return document_.data(); }
    void setDocument(SessionPreview* document);
    [[nodiscard]] bool interactive() const { return interactive_; }
    void setInteractive(bool enabled);
    [[nodiscard]] Workspace* focusWorkspace() const { return focus_workspace_.data(); }
    void setFocusWorkspace(Workspace* workspace);
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
  signals:
    void documentChanged();
    void interactiveChanged();
    void focusWorkspaceChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

  private:
    [[nodiscard]] bool acceptsTerminalInput() const;
    void requestResize();
    void bindWindow(QQuickWindow* current);
    void publishFrame(bool snapshot_changed);
    void updateInputContext(Qt::InputMethodQueries queries);
    void resetInputContext();
    void updateInteractionBlock();
    struct RenderState;
    std::mutex render_mutex_;
    std::shared_ptr<const RenderState> render_state_;
    QPointer<SessionPreview> document_;
    QMetaObject::Connection window_active_connection_;
    QMetaObject::Connection window_changed_connection_;
    QPointer<Workspace> focus_workspace_;
    QString interaction_reason_;

    QTimer preview_update_;
    bool interactive_{};
    bool paste_in_progress_{};
    QString preedit_;
    quint64 ime_epoch_{};
    bool resetting_input_{};
    enum class CompositionState : std::uint8_t { idle, active, stale };
    CompositionState composition_state_{CompositionState::idle};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
