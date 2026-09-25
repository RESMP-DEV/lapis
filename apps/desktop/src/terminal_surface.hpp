#ifndef LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
#define LAPIS_DESKTOP_TERMINAL_SURFACE_HPP

#include "keymap.hpp"
#include "workspace.hpp"

#include <QFont>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace lapis::desktop {

// Legacy printable-key encoding; native text/IME remains Unicode.
[[nodiscard]] QByteArray terminal_text_key(const QKeyEvent& event);
// The http(s) URL covering a screen cell, following rows it wraps onto; empty
// when the cell is not part of one.
[[nodiscard]] QString terminal_url_at(const session::TerminalSnapshot& snapshot, int column,
                                      int row);

class TerminalSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(lapis::desktop::SessionPreview* document READ document WRITE setDocument NOTIFY
                   documentChanged)
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive NOTIFY interactiveChanged)
    // Minimum milliseconds between redraws caused by output; 0 redraws every
    // snapshot. Previews use it so a noisy agent cannot flood the renderer.
    Q_PROPERTY(
        int frameInterval READ frameInterval WRITE setFrameInterval NOTIFY frameIntervalChanged)
    // Smallest scale used to fit the screen; 0 fits it whole. When fitting
    // would go below this, the screen is drawn at this scale showing the rows
    // that end at the cursor, where agent TUIs keep their prompt and output.
    Q_PROPERTY(
        qreal minimumScale READ minimumScale WRITE setMinimumScale NOTIFY minimumScaleChanged)
    Q_PROPERTY(bool composing READ composing NOTIFY inputOwnershipChanged)
    Q_PROPERTY(bool pasting READ pasting NOTIFY inputOwnershipChanged)
    // Requested family; empty or unavailable/proportional names use the
    // platform's fixed-width system font, reported through resolvedFontFamily.
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(int fontPixelSize READ fontPixelSize WRITE setFontPixelSize NOTIFY fontChanged)
    Q_PROPERTY(QString resolvedFontFamily READ resolvedFontFamily NOTIFY fontChanged)
    // Columns and rows this viewport requests from a live terminal.
    Q_PROPERTY(QSize gridSize READ gridSize NOTIFY gridSizeChanged)
    // Text chosen by dragging or double-clicking; copied with Command-C
    // (Control-Shift-C on Linux) and cleared by typing.
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged)
  public:
    explicit TerminalSurface(QQuickItem* parent = nullptr);
    ~TerminalSurface() override;
    [[nodiscard]] SessionPreview* document() const { return document_.data(); }
    void setDocument(SessionPreview* document);
    [[nodiscard]] bool interactive() const { return interactive_; }
    void setInteractive(bool enabled);
    [[nodiscard]] int frameInterval() const { return frame_interval_; }
    [[nodiscard]] qreal minimumScale() const { return minimum_scale_; }
    void setMinimumScale(qreal scale);
    void setFrameInterval(int milliseconds);
    [[nodiscard]] bool composing() const { return !preedit_.isEmpty(); }
    [[nodiscard]] bool pasting() const { return pasting_; }
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    [[nodiscard]] const QString& fontFamily() const { return font_family_; }
    void setFontFamily(const QString& family);
    [[nodiscard]] int fontPixelSize() const { return font_pixel_size_; }
    void setFontPixelSize(int pixels);
    [[nodiscard]] const QString& resolvedFontFamily() const { return resolved_font_family_; }
    [[nodiscard]] QSize gridSize() const { return grid_size_; }
    [[nodiscard]] const QString& selectedText() const { return selection_text_; }
    // Item-coordinate bounds of a visible cell of the current screen.
    [[nodiscard]] QRectF cellRect(int column, int row) const;
  signals:
    void documentChanged();
    void interactiveChanged();
    void frameIntervalChanged();
    void minimumScaleChanged();
    void inputOwnershipChanged();
    void fontChanged();
    void gridSizeChanged();
    void selectionChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

  private:
    [[nodiscard]] bool acceptsTerminalInput() const;
    void requestResize();
    void claimSize();
    void applyFont();
    [[nodiscard]] QFont cellFont() const;
    void bindWindow(QQuickWindow* current);
    void publishFrame(bool snapshot_changed);
    void updateInputContext(Qt::InputMethodQueries queries);
    void resetInputContext();
    // Cell geometry of the current screen as laid out in this item.
    struct CellGrid {
        qreal width{};
        qreal height{};
        int columns{};
        int rows{};
        qreal first_row{};
        qreal scale{};
    };
    [[nodiscard]] std::optional<CellGrid> cellGrid() const;
    [[nodiscard]] QPoint cellAt(QPointF position) const;
    [[nodiscard]] QString textBetween(QPoint start, QPoint end) const;
    void setSelection(QPoint anchor, QPoint head);
    void clearSelection();
    bool copySelection(const QKeyEvent& event);
    void resumeLiveForTyping(const QKeyEvent& event);
    void commandKey(QKeyEvent& event);
    void scrollHistory(int steps);
    struct RenderState;
    std::mutex render_mutex_;
    std::shared_ptr<const RenderState> render_state_;
    QPointer<SessionPreview> document_;
    QMetaObject::Connection window_active_connection_;
    QMetaObject::Connection window_changed_connection_;
    QPointF last_hover_;

    QString font_family_;
    QString resolved_font_family_;
    bool use_system_font_{true};
    int font_pixel_size_{kTerminalFontSizeDefault};
    QSize grid_size_;
    bool interactive_{};
    int frame_interval_{};
    qreal minimum_scale_{};
    QTimer throttle_;
    bool pasting_{};
    QString preedit_;
    quint64 ime_epoch_{};
    bool resetting_input_{};
    enum class CompositionState : std::uint8_t { idle, active, stale };
    CompositionState composition_state_{CompositionState::idle};
    // Selection endpoints in screen cells, in the order they were chosen.
    std::optional<QPoint> selection_anchor_;
    std::optional<QPoint> selection_head_;
    QString selection_text_;
    QPoint press_cell_;
    bool selecting_{};
    int wheel_remainder_{};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
