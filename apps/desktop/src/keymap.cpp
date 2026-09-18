#include "keymap.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace lapis::desktop {
namespace {
// Qt reports macOS Command as Meta, so "Ctrl" keeps working across platforms
// while "Meta" (or "Cmd") reaches the same modifier on this host.
constexpr int kMaximumSequencesPerAction = 4;
constexpr int kMaximumActions = 64;

[[nodiscard]] QStringList normalise(const QJsonValue& value, QString* diagnostic,
                                    const QString& action) {
    QStringList sequences;
    const auto add = [&sequences, diagnostic, &action](const QString& text) {
        if (text.trimmed().isEmpty())
            return;
        if (sequences.size() >= kMaximumSequencesPerAction) {
            *diagnostic = QStringLiteral("%1: at most %2 sequences are used")
                              .arg(action)
                              .arg(kMaximumSequencesPerAction);
            return;
        }
        sequences.append(text.trimmed());
    };
    if (value.isString())
        add(value.toString());
    else if (value.isArray()) {
        for (const auto& entry : value.toArray())
            add(entry.toString());
    }
    return sequences;
}
} // namespace

KeyMap::KeyMap(QObject* parent) : QObject(parent) {
    apply_defaults();
    source_path_ = default_source_path();
}

QString KeyMap::default_source_path() {
    return QDir(QStringLiteral(LAPIS_PROJECT_ROOT)).filePath(QStringLiteral("lapis.json"));
}

void KeyMap::apply_defaults() {
    bindings_ = {
        {QStringLiteral("quit"), {QStringLiteral("Ctrl+Q")}},
        {QStringLiteral("detachWindow"), {QStringLiteral("Ctrl+W")}},
        {QStringLiteral("nextCategory"), {QStringLiteral("Ctrl+Tab")}},
        {QStringLiteral("previousCategory"), {QStringLiteral("Ctrl+Shift+Tab")}},
        {QStringLiteral("category1"), {QStringLiteral("Ctrl+1")}},
        {QStringLiteral("category2"), {QStringLiteral("Ctrl+2")}},
        {QStringLiteral("category3"), {QStringLiteral("Ctrl+3")}},
        {QStringLiteral("category4"), {QStringLiteral("Ctrl+4")}},
        {QStringLiteral("nextWindow"), {QStringLiteral("Ctrl+Shift+]")}},
        {QStringLiteral("previousWindow"), {QStringLiteral("Ctrl+Shift+[")}},
        {QStringLiteral("focusLeft"), {QStringLiteral("Ctrl+Left")}},
        {QStringLiteral("focusRight"), {QStringLiteral("Ctrl+Right")}},
        {QStringLiteral("cycleLayout"), {QStringLiteral("Ctrl+L")}},
        {QStringLiteral("reloadConfig"), {QStringLiteral("Ctrl+R")}},
    };
    layout_ = WorkspaceLayout::Focus;
}

bool KeyMap::load() {
    apply_defaults();
    diagnostic_.clear();
    loaded_ = false;
    const QFileInfo info(source_path_);
    if (!info.isFile()) {
        diagnostic_ = QStringLiteral("%1 not found; using built-in defaults").arg(source_path_);
        emit changed();
        return false;
    }
    QFile file(source_path_);
    if (!file.open(QIODevice::ReadOnly)) {
        diagnostic_ = QStringLiteral("Could not read %1: %2").arg(source_path_, file.errorString());
        emit changed();
        return false;
    }
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    file.close();
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        diagnostic_ = QStringLiteral("%1 is not a valid JSON object: %2")
                          .arg(source_path_, parse_error.errorString());
        emit changed();
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonObject keys = root.value(QStringLiteral("keybindings")).toObject();
    int applied = 0;
    for (auto it = keys.begin(); it != keys.end() && applied < kMaximumActions; ++it, ++applied) {
        const QStringList sequences = normalise(it.value(), &diagnostic_, it.key());
        if (!sequences.isEmpty())
            bindings_.insert(it.key(), sequences);
    }
    const QString requested_layout = root.value(QStringLiteral("layout")).toString();
    if (requested_layout == QStringLiteral("blocks"))
        layout_ = WorkspaceLayout::Blocks;
    else if (requested_layout == QStringLiteral("focus"))
        layout_ = WorkspaceLayout::Focus;
    else if (!requested_layout.isEmpty())
        diagnostic_ = QStringLiteral("Unknown layout '%1'; keeping focus").arg(requested_layout);
    loaded_ = true;
    emit changed();
    return true;
}

QStringList KeyMap::sequences(const QString& action) const { return bindings_.value(action); }

bool KeyMap::reload() {
    const bool ok = load();
    qInfo().noquote() << "lapis keymap reloaded:" << (ok ? "ok" : "failed")
                      << (diagnostic_.isEmpty() ? QString() : diagnostic_);
    return ok;
}

void KeyMap::toggleLayout() {
    layout_ = layout_ == WorkspaceLayout::Blocks ? WorkspaceLayout::Focus : WorkspaceLayout::Blocks;
    qInfo().noquote() << "lapis layout:" << layoutName();
    emit changed();
}
} // namespace lapis::desktop