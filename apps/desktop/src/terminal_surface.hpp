#ifndef LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
#define LAPIS_DESKTOP_TERMINAL_SURFACE_HPP

#include "keymap.hpp"
#include <lapis/session/terminal.hpp>

#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QJSValue>
#include <QKeyEvent>
#include <QPoint>
#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

Q_MOC_INCLUDE("workspace.hpp")

namespace lapis::desktop {
class SessionPreview;

// Legacy printable-key encoding; native text/IME remains Unicode.
[[nodiscard]] QByteArray terminal_text_key(const QKeyEvent& event);
// The http(s) URL covering a screen cell, following rows it wraps onto; empty
// when the cell is not part of one.
[[nodiscard]] QString terminal_url_at(const session::TerminalSnapshot& snapshot, int column,
                                      int row);
// A case-insensitive match of `needle` within one screen row.
struct TerminalMatch {
    int row{};
    int first_column{};
    int last_column{};
    bool operator==(const TerminalMatch&) const = default;
};
// What a cell is part of that Command-click opens: an OSC 8 destination, a
// visible http(s)/www link, or text
// that may name a file or folder (checked with resolve_terminal_path), with
// the cells it covers on each row it wraps over.
struct TerminalLink {
    enum class Kind : std::uint8_t { url, path };
    Kind kind{Kind::url};
    QString text; // the URL, or the path as written without its :line
    int line{};   // a path's line (file.cpp:12 or file.cpp:12:3), else 0
    std::vector<TerminalMatch> cells;
};
[[nodiscard]] std::optional<TerminalLink>
terminal_link_at(const session::TerminalSnapshot& snapshot, int column, int row);
// A path as written, as an existing file or folder on this Mac: absolute,
// under ~, or relative to `folder` (none when `folder` is empty); else empty.
[[nodiscard]] QString resolve_terminal_path(const QString& written, const QString& folder);
// The next match after the cell `from` (column, row), or the one before it
// when `backwards`; searching wraps around neither end of the screen.
[[nodiscard]] std::optional<TerminalMatch> terminal_find(const session::TerminalSnapshot& snapshot,
                                                         const QString& needle, QPoint from,
                                                         bool backwards);

// How an offered next prompt lays out after the cursor: the one line shown
// (elided to the room left before the row's last two cells), whether that is
// the whole suggestion (else it ends in an ellipsis), the keys hint, the first column
// and the width drawn. Empty `shown` when there is no room.
struct SuggestionLayout {
    QString shown;
    QString keys;
    bool whole{};
    int column{};
    qreal width{};
};
[[nodiscard]] SuggestionLayout lay_out_suggestion(const session::TerminalSnapshot& snapshot,
                                                  const QFontMetricsF& metrics,
                                                  const QString& suggestion);

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
    // While true the agent keeps its size: a divider or window being dragged
    // resizes the terminal once, when the drag ends, not every step.
    Q_PROPERTY(bool holdResize READ holdResize WRITE setHoldResize NOTIFY holdResizeChanged)
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
    Q_PROPERTY(QString hoveredLink READ hoveredLink NOTIFY hoveredLinkChanged)
    // A next prompt offered for the agent (see NextPrompt), dim after the
    // cursor. With `tabFlow`, Tab sends it (Option-Tab only types it), and
    // Tab with nothing offered and nothing typed moves to the next agent.
    Q_PROPERTY(QString suggestion READ suggestion WRITE setSuggestion NOTIFY suggestionChanged)
    // The offer the suggestion belongs to: one impression per offer, even when
    // two offers have the same words.
    Q_PROPERTY(
        QString suggestionKey READ suggestionKey WRITE setSuggestionKey NOTIFY suggestionChanged)
    Q_PROPERTY(bool tabFlow READ tabFlow WRITE setTabFlow NOTIFY tabFlowChanged)
    // Called by Tab with nothing offered and nothing typed: moves to the next
    // agent that needs you and returns true, or returns false and Tab goes to
    // the program.
    Q_PROPERTY(QJSValue tabAway READ tabAway WRITE setTabAway NOTIFY tabFlowChanged)
  public:
    explicit TerminalSurface(QQuickItem* parent = nullptr);
    // Text as if pasted (bracketed when the agent asked for it): what files
    // dropped on the terminal become.
    Q_INVOKABLE bool pasteText(const QString& text);
    // A valid file URL as QUrl's local or UNC path; empty for other schemes,
    // malformed URLs and NUL bytes. Callers still quote the result for a shell.
    Q_INVOKABLE QString localFilePath(const QString& url) const;
    // Selects the next match on the page shown (older first when
    // `backwards`), starting from the current selection; false when there is
    // none left on this page.
    Q_INVOKABLE bool findText(const QString& text, bool backwards);
    // How many times the page shown contains the text.
    Q_INVOKABLE int countMatches(const QString& text) const;
    Q_INVOKABLE void clearSelectedText() { clearSelection(); }
    ~TerminalSurface() override;
    [[nodiscard]] SessionPreview* document() const;
    void setDocument(SessionPreview* document);
    [[nodiscard]] bool interactive() const { return interactive_; }
    void setInteractive(bool enabled);
    [[nodiscard]] int frameInterval() const { return frame_interval_; }
    [[nodiscard]] qreal minimumScale() const { return minimum_scale_; }
    [[nodiscard]] bool holdResize() const { return hold_resize_; }
    void setHoldResize(bool hold);
    void setMinimumScale(qreal scale);
    void setFrameInterval(int milliseconds);
    [[nodiscard]] bool composing() const { return !preedit_.isEmpty(); }
    [[nodiscard]] bool pasting() const { return pasting_; }
    [[nodiscard]] const QString& suggestion() const { return suggestion_; }
    void setSuggestion(const QString& suggestion);
    [[nodiscard]] const QString& suggestionKey() const { return suggestion_key_; }
    void setSuggestionKey(const QString& key);
    [[nodiscard]] bool tabFlow() const { return tab_flow_; }
    void setTabFlow(bool enabled);
    [[nodiscard]] QJSValue tabAway() const { return tab_away_; }
    void setTabAway(const QJSValue& move);
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
    // What Command-click would open under the pointer while Command is held:
    // a URL or an existing file or folder, else empty.
    [[nodiscard]] const QString& hoveredLink() const { return hovered_target_; }
    // Tests see what would open without opening it.
    void setOpensLinksForTesting(bool opens) { opens_links_ = opens; }
  signals:
    void documentChanged();
    void interactiveChanged();
    void frameIntervalChanged();
    void minimumScaleChanged();
    void holdResizeChanged();
    void inputOwnershipChanged();
    void fontChanged();
    void gridSizeChanged();
    void selectionChanged();
    void hoveredLinkChanged();
    // Command-click opened a URL or a file or folder's path.
    void linkOpened(const QString& target);
    void suggestionChanged();
    void tabFlowChanged();
    // The suggestion is on screen in the active window, once per suggestion.
    void suggestionSeen(const QString& sessionId, const QString& offerKey);
    // The offered suggestion was typed into the agent (`sent` false), or a
    // second Tab right after sent it (`sent` true); `typedFirst` keys went to
    // the agent while it was offered.
    void suggestionUsed(const QString& sessionId, const QString& offerKey, bool sent,
                        int typedFirst);

    void pasteRefused(const QString& reason);

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

  private:
    [[nodiscard]] bool acceptsTerminalInput() const;
    void requestResize();
    void claimSize();
    void applyFont();
    [[nodiscard]] QFont cellFont() const;
    void bindWindow(QQuickWindow* current);
    void armSuggestionObservation();
    void cancelSuggestionObservation();
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
    void scrollProgram(int steps, QPoint cell);
    struct RenderState;
    struct FrameHandoff;
    // Render callbacks own this independently of the item and validate its epoch.
    std::shared_ptr<FrameHandoff> handoff_;
    QPointer<SessionPreview> document_;
    QMetaObject::Connection window_active_connection_;
    QMetaObject::Connection window_visible_connection_;
    QMetaObject::Connection suggestion_frame_connection_;
    std::uint64_t suggestion_watch_ticket_{};
    bool suggestion_watch_pending_{};
    QMetaObject::Connection presentation_connection_;
    // The agent this view is showing, registered as its viewer while this
    // view and its window are visible, so unseen agents' screens stay encoded.
    QPointer<SessionPreview> viewed_;
    int viewed_interval_{};
    void updateViewing();
    void screenChanged();
    // After a key is typed here, frames keep coming at the display's rate
    // for a moment. macOS shows the first frame after a quiet spell late
    // (about 30 ms more, measured on a ProMotion display), and a typed key's
    // echo is always such a frame; frames that follow frames show in about 6.
    void keepFramesComing();
    QElapsedTimer since_typed_;
    QMetaObject::Connection warm_connection_;
    QMetaObject::Connection window_changed_connection_;
    QPointF last_hover_;
    // The link under the pointer while Command is held, underlined, and what
    // it opens.
    void updateLink(QPointF position, Qt::KeyboardModifiers modifiers);
    void clearLink();
    [[nodiscard]] QString linkTarget(const TerminalLink& link) const;
    QPointF hover_position_;
    std::optional<TerminalLink> hovered_link_;
    QString hovered_target_;
    bool opens_links_{true};

    QString font_family_;
    QString resolved_font_family_;
    bool use_system_font_{true};
    int font_pixel_size_{kTerminalFontSizeDefault};
    QSize grid_size_;
    bool interactive_{};
    bool hold_resize_{};
    int frame_interval_{};
    qreal minimum_scale_{};
    QTimer throttle_;
    QElapsedTimer since_frame_; // since the last throttled frame
    bool pasting_{};
    QString preedit_;
    QString suggestion_;
    QString suggestion_key_;
    QString seen_;
    QJSValue tab_away_;
    [[nodiscard]] SuggestionLayout presentedSuggestion() const;
    bool tab_flow_{};
    bool typed_since_arrival_{};
    int typed_while_offered_{};
    // The guess Tab just typed in, until another key: a second Tab sends it.
    struct Filled {
        QPointer<SessionPreview> owner;
        QString session;
        QString offer;
        QString typed; // the suggestion Tab pasted, before edits
        int typed_first{};
        quint64 request{};
        bool admitted{};     // the service queued the typed guess
        bool send_pending{}; // the second Tab came before that
    };
    std::optional<Filled> filled_;
    [[nodiscard]] bool fillSuggestion();
    void filledAdmitted(const Filled& made, bool queued);
    void sendFilled();
    [[nodiscard]] bool takeFilled(bool plain_tab);
    bool takeSuggestion(const QKeyEvent& event);
    quint64 pasteTextRequest(const QString& text, std::optional<bool> submit);
    void reportSeen();
    void noteTyped();
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
    bool wheel_program_{}; // wheel units change between program and history scrolling
    int wheel_remainder_{};
    qreal pixel_remainder_{}; // a trackpad's scroll, short of a row
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_TERMINAL_SURFACE_HPP
