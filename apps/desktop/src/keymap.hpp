#ifndef LAPIS_DESKTOP_KEYMAP_HPP
#define LAPIS_DESKTOP_KEYMAP_HPP

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace lapis::desktop {
// Layout of the session area. "focus" keeps one large pane with a strip of
// cards; "blocks" gives every session an equal tile.
enum class WorkspaceLayout : std::uint8_t { Focus, Blocks };

// User-editable keybindings and layout, loaded from lapis.json at the project
// root. Missing or malformed input falls back to built-in defaults, so a bad
// edit degrades rather than bricking the window. Every action maps to a list of
// Qt key sequences, which QML binds through Shortcut.
class KeyMap final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString layoutName READ layoutName NOTIFY changed)
    Q_PROPERTY(bool blocks READ blocks NOTIFY changed)
  public:
    explicit KeyMap(QObject* parent = nullptr);

    // Re-read the file, keeping defaults for anything missing or invalid.
    // Returns true when the file parsed and was applied.
    bool load();
    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] const QString& sourcePath() const { return source_path_; }
    [[nodiscard]] const QString& diagnostic() const { return diagnostic_; }

    [[nodiscard]] QStringList sequences(const QString& action) const;
    Q_INVOKABLE [[nodiscard]] QStringList actionSequences(const QString& action) const {
        return sequences(action);
    }

    [[nodiscard]] WorkspaceLayout layout() const { return layout_; }
    [[nodiscard]] QString layoutName() const {
        return layout_ == WorkspaceLayout::Blocks ? QStringLiteral("blocks")
                                                  : QStringLiteral("focus");
    }
    [[nodiscard]] bool blocks() const { return layout_ == WorkspaceLayout::Blocks; }
    Q_INVOKABLE void toggleLayout();
    Q_INVOKABLE bool reload();

  signals:
    void changed();

  private:
    void apply_defaults();
    [[nodiscard]] static QString default_source_path();
    QHash<QString, QStringList> bindings_;
    QString source_path_;
    QString diagnostic_;
    WorkspaceLayout layout_{WorkspaceLayout::Focus};
    bool loaded_{};
};
} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_KEYMAP_HPP