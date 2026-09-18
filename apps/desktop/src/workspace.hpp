#ifndef LAPIS_DESKTOP_WORKSPACE_HPP
#define LAPIS_DESKTOP_WORKSPACE_HPP

#include <lapis/session/terminal.hpp>

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <memory>
#include <vector>

namespace lapis::desktop {

class SessionPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QString activity READ activity CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
  public:
    SessionPreview(QString title, QString directory, QString activity, QColor accent,
                   std::string_view content);
    [[nodiscard]] QString title() const { return title_; }
    [[nodiscard]] QString directory() const { return directory_; }
    [[nodiscard]] QString activity() const { return activity_; }
    [[nodiscard]] QColor accent() const { return accent_; }
    [[nodiscard]] const session::TerminalSnapshot& snapshot() const { return snapshot_; }

  private:
    QString title_;
    QString directory_;
    QString activity_;
    QColor accent_;
    session::TerminalSnapshot snapshot_;
};

class Workspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sessions READ sessions CONSTANT)
    Q_PROPERTY(int focusedIndex READ focusedIndex WRITE setFocusedIndex NOTIFY focusChanged)
    Q_PROPERTY(
        lapis::desktop::SessionPreview* focusedSession READ focusedSession NOTIFY focusChanged)
  public:
    Workspace();
    [[nodiscard]] QVariantList sessions() const;
    [[nodiscard]] int focusedIndex() const { return focused_index_; }
    [[nodiscard]] SessionPreview* focusedSession() const;
    void setFocusedIndex(int index);
  signals:
    void focusChanged();

  private:
    std::vector<std::unique_ptr<SessionPreview>> sessions_;
    int focused_index_{};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_WORKSPACE_HPP
