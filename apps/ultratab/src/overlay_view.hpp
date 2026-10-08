#ifndef LAPIS_ULTRATAB_OVERLAY_VIEW_HPP
#define LAPIS_ULTRATAB_OVERLAY_VIEW_HPP
#include <QObject>
#include <QRectF>
#include <QSize>

class QQuickView;

namespace lapis::ultratab {
// What the overlay QML tells the window around it: the panel's rectangle
// (where the blur goes) and the size everything drawn needs (the window's
// size). A child of the view, found with findChild.
class OverlayHost final : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    Q_INVOKABLE void setPanel(const QRectF& rect, qreal radius) {
        if (rect == panel_ && radius == radius_)
            return;
        panel_ = rect;
        radius_ = radius;
        emit panelChanged();
    }
    Q_INVOKABLE void setContentSize(int width, int height) {
        const QSize size(width, height);
        if (size == size_)
            return;
        size_ = size;
        emit contentSizeChanged();
    }
    [[nodiscard]] QRectF panel() const { return panel_; }
    [[nodiscard]] qreal radius() const { return radius_; }
    [[nodiscard]] QSize contentSize() const { return size_; }

  signals:
    void panelChanged();
    void contentSizeChanged();

  private:
    QRectF panel_;
    qreal radius_{};
    QSize size_;
};
class Deck;
struct ViewOptions {
    bool backdrop{};       // a stand-in for the blurred desktop, for captures
    bool reduced_motion{}; // no arrival slide or pulsing dots
};
// Loads the overlay QML into a transparent `view` showing `deck` (borrowed;
// it must outlive the view). False, with Qt's errors logged, when it fails.
[[nodiscard]] bool load_overlay(QQuickView& view, Deck& deck, const ViewOptions& options);
} // namespace lapis::ultratab
#endif
