#include "keymap.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QVariantMap>
#include <array>

namespace lapis::desktop {
namespace {
// Qt reports macOS Command as Meta, so "Ctrl" keeps working across platforms
// while "Meta" (or "Cmd") reaches the same modifier on this host.
constexpr int kMaximumSequencesPerAction = 4;
constexpr int kMaximumActions = 64;
constexpr qint64 kMaximumConfigBytes = qint64{1024} * 1024;
constexpr int kMaximumPrettyDepth = 64;

// Built-in colour schemes. Every theme keeps the same contrast relationships:
// background is darkest, cards sit above it, focused borders are the brightest
// accent, and text is near-white. A theme only changes the frame around the
// terminal; the session's own palette still colours program output. A plain
// std::array of literal pointers keeps this a constant-initialized table.
constexpr std::array<Theme, 6> kThemes = {{
    {.name = "lapis",
     .label = "Lapis",
     .background = "#0b101a",
     .surface = "#111927",
     .card = "#151c29",
     .hovered_card = "#1a2333",
     .focused = "#1f2838",
     .border = "#2b3546",
     .focused_border = "#6a76e8",
     .text = "#f2f3ea",
     .muted_text = "#98a0ae",
     .attention = "#ff5f56",
     .attention_text = "#fff4f2"},
    {.name = "graphite",
     .label = "Graphite",
     .background = "#111214",
     .surface = "#17191c",
     .card = "#1d2024",
     .hovered_card = "#24282d",
     .focused = "#2a2f35",
     .border = "#33383f",
     .focused_border = "#8a93a3",
     .text = "#eceef1",
     .muted_text = "#969ca6",
     .attention = "#f0a04b",
     .attention_text = "#1a1408"},
    {.name = "daylight",
     .label = "Daylight",
     .background = "#f4f5f7",
     .surface = "#ffffff",
     .card = "#eceef2",
     .hovered_card = "#e2e5eb",
     .focused = "#d8dce4",
     .border = "#c6cbd4",
     .focused_border = "#4a5bd4",
     .text = "#14181f",
     .muted_text = "#5d6675",
     .attention = "#c8342c",
     .attention_text = "#ffffff"},
    {.name = "solarized",
     .label = "Solarized",
     .background = "#002b36",
     .surface = "#073642",
     .card = "#0a4050",
     .hovered_card = "#0d4a5c",
     .focused = "#105468",
     .border = "#1c5f72",
     .focused_border = "#b58900",
     .text = "#eee8d5",
     .muted_text = "#93a1a1",
     .attention = "#dc322f",
     .attention_text = "#fdf6e3"},
    {.name = "amber",
     .label = "Amber",
     .background = "#17120a",
     .surface = "#1f1810",
     .card = "#261d12",
     .hovered_card = "#2e2317",
     .focused = "#372a1b",
     .border = "#453520",
     .focused_border = "#e8a33d",
     .text = "#f6ead2",
     .muted_text = "#a2916f",
     .attention = "#ff6b3d",
     .attention_text = "#1a1008"},
    {.name = "contrast",
     .label = "High contrast",
     .background = "#000000",
     .surface = "#000000",
     .card = "#0a0a0a",
     .hovered_card = "#141414",
     .focused = "#1e1e1e",
     .border = "#4a4a4a",
     .focused_border = "#ffe14d",
     .text = "#ffffff",
     .muted_text = "#c8c8c8",
     .attention = "#ff4d4d",
     .attention_text = "#000000"},
}};

[[nodiscard]] const Theme& find_theme(const QString& name) {
    for (const Theme& theme : kThemes) {
        if (name == QLatin1String(theme.name))
            return theme;
    }
    return kThemes.front();
}

// Keep scalar arrays compact without editing serialized string contents.
// Qt performs every scalar escape; object/array structure comes from parsed JSON.
[[nodiscard]] QByteArray format_config(const QJsonValue& value, int depth = 0) {
    if (!value.isObject() && !value.isArray())
        return value.toJson(QJsonValue::JsonFormat::Compact);
    if (value.isObject() && value.toObject().isEmpty())
        return value.toJson(QJsonValue::JsonFormat::Compact);
    if (value.isArray()) {
        bool scalar_only = true;
        for (const auto& item : value.toArray())
            scalar_only = scalar_only && !item.isArray() && !item.isObject();
        if (scalar_only)
            return value.toJson(QJsonValue::JsonFormat::Compact);
    }
    if (depth > kMaximumPrettyDepth)
        return value.toJson(QJsonValue::JsonFormat::Compact);
    QByteArray output = value.isObject() ? "{\n" : "[\n";
    bool first = true;
    const auto append = [&](const QByteArray& member) {
        if (!first)
            output.append(",\n");
        first = false;
        output.append(QByteArray(qsizetype{depth + 1} * 4, ' '));
        output.append(member);
    };
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            append(QJsonValue(it.key()).toJson(QJsonValue::JsonFormat::Compact) + ": " +
                   format_config(it.value(), depth + 1));
    } else {
        for (const auto& item : value.toArray())
            append(format_config(item, depth + 1));
    }
    output.append('\n');
    output.append(QByteArray(qsizetype{depth} * 4, ' '));
    output.append(value.isObject() ? '}' : ']');
    return output;
}

[[nodiscard]] QString density_name(CardDensity density) {
    switch (density) {
    case CardDensity::Compact:
        return QStringLiteral("compact");
    case CardDensity::Minimal:
        return QStringLiteral("minimal");
    case CardDensity::Comfortable:
        break;
    }
    return QStringLiteral("comfortable");
}

[[nodiscard]] bool config_path_is_regular(const QString& path, QString* reason) {
    const QFileInfo info(path);
    if (info.exists() && !info.isFile()) {
        *reason = QStringLiteral("path is not a regular file");
        return false;
    }
    return true;
}

} // namespace

const std::array<Theme, 6>& theme_table() { return kThemes; }

bool theme_exists(const QString& name) {
    for (const Theme& theme : kThemes) {
        if (name == QLatin1String(theme.name))
            return true;
    }
    return false;
}

const Theme& theme_for(const QString& name) { return find_theme(name); }

QStringList default_settings_shortcuts() {
    return {QStringLiteral("Ctrl+,"), QStringLiteral("Meta+,")};
}

namespace {

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

// Parse a requested layout. Returns false for an unrecognised name so the
// caller can report it rather than silently switching.
[[nodiscard]] bool parse_layout(const QString& name, WorkspaceLayout* out) {
    if (name == QStringLiteral("focus")) {
        *out = WorkspaceLayout::Focus;
        return true;
    }
    if (name == QStringLiteral("blocks")) {
        *out = WorkspaceLayout::Blocks;
        return true;
    }
    if (name == QStringLiteral("columns")) {
        *out = WorkspaceLayout::Columns;
        return true;
    }
    if (name == QStringLiteral("stack")) {
        *out = WorkspaceLayout::Stack;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_density(const QString& name, CardDensity* out) {
    if (name == QStringLiteral("comfortable")) {
        *out = CardDensity::Comfortable;
        return true;
    }
    if (name == QStringLiteral("compact")) {
        *out = CardDensity::Compact;
        return true;
    }
    if (name == QStringLiteral("minimal")) {
        *out = CardDensity::Minimal;
        return true;
    }
    return false;
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
        {QStringLiteral("openSettings"), default_settings_shortcuts()},
        {QStringLiteral("reloadConfig"), {QStringLiteral("Ctrl+R")}},
    };
    layout_ = WorkspaceLayout::Focus;
    density_ = CardDensity::Comfortable;
    theme_ = QStringLiteral("lapis");
}

bool KeyMap::load() {
    apply_defaults();
    diagnostic_.clear();
    loaded_ = false;
    QString path_reason;
    if (!config_path_is_regular(source_path_, &path_reason)) {
        diagnostic_ = QStringLiteral("%1 not read: %2; using built-in defaults")
                          .arg(source_path_, path_reason);
        emit changed();
        return false;
    }
    const QFileInfo info(source_path_);
    if (!info.exists()) {
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
    if (file.size() > kMaximumConfigBytes) {
        diagnostic_ = QStringLiteral("Config exceeds 1 MiB; using defaults");
        emit changed();
        return false;
    }
    QJsonParseError parse_error{};
    const QByteArray contents = file.read(kMaximumConfigBytes + 1);
    file.close();
    if (contents.size() > kMaximumConfigBytes) {
        diagnostic_ = QStringLiteral("Config exceeds 1 MiB; using defaults");
        emit changed();
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parse_error);
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
    if (!requested_layout.isEmpty() && !parse_layout(requested_layout, &layout_))
        diagnostic_ = QStringLiteral("Unknown layout '%1'; keeping focus").arg(requested_layout);

    const QString requested_theme = root.value(QStringLiteral("theme")).toString();
    if (requested_theme.isEmpty()) {
        // Tolerate a theme object as well as a plain name, so a hand-written
        // config can spell the window colour as structured data.
        const QJsonObject theme_object = root.value(QStringLiteral("theme")).toObject();
        const QString named = theme_object.value(QStringLiteral("name")).toString();
        if (!named.isEmpty()) {
            if (theme_exists(named))
                theme_ = named;
            else
                diagnostic_ = QStringLiteral("Unknown theme '%1'; keeping lapis").arg(named);
        }
    } else if (theme_exists(requested_theme)) {
        theme_ = requested_theme;
    } else {
        diagnostic_ = QStringLiteral("Unknown theme '%1'; keeping lapis").arg(requested_theme);
    }

    const QString requested_density = root.value(QStringLiteral("density")).toString();
    if (!requested_density.isEmpty() && !parse_density(requested_density, &density_))
        diagnostic_ =
            QStringLiteral("Unknown density '%1'; keeping comfortable").arg(requested_density);

    loaded_ = true;
    emit changed();
    return true;
}

QString KeyMap::layoutName() const {
    switch (layout_) {
    case WorkspaceLayout::Blocks:
        return QStringLiteral("blocks");
    case WorkspaceLayout::Columns:
        return QStringLiteral("columns");
    case WorkspaceLayout::Stack:
        return QStringLiteral("stack");
    case WorkspaceLayout::Focus:
        break;
    }
    return QStringLiteral("focus");
}

QString KeyMap::densityName() const { return density_name(density_); }

QVariantList KeyMap::themes() const {
    QVariantList list;
    for (const Theme& theme : kThemes) {
        list.append(QVariantMap{
            {QStringLiteral("name"), QString::fromLatin1(theme.name)},
            {QStringLiteral("label"), QString::fromLatin1(theme.label)},
            {QStringLiteral("background"), QString::fromLatin1(theme.background)},
            {QStringLiteral("surface"), QString::fromLatin1(theme.surface)},
            {QStringLiteral("card"), QString::fromLatin1(theme.card)},
            {QStringLiteral("hoveredCard"), QString::fromLatin1(theme.hovered_card)},
            {QStringLiteral("focused"), QString::fromLatin1(theme.focused)},
            {QStringLiteral("border"), QString::fromLatin1(theme.border)},
            {QStringLiteral("focusedBorder"), QString::fromLatin1(theme.focused_border)},
            {QStringLiteral("text"), QString::fromLatin1(theme.text)},
            {QStringLiteral("mutedText"), QString::fromLatin1(theme.muted_text)},
            {QStringLiteral("attention"), QString::fromLatin1(theme.attention)},
            {QStringLiteral("attentionText"), QString::fromLatin1(theme.attention_text)},
        });
    }
    return list;
}

QStringList KeyMap::layouts() const {
    return {QStringLiteral("focus"), QStringLiteral("columns"), QStringLiteral("blocks"),
            QStringLiteral("stack")};
}

QStringList KeyMap::densities() const {
    return {QStringLiteral("comfortable"), QStringLiteral("compact"), QStringLiteral("minimal")};
}

QStringList KeyMap::sequences(const QString& action) const { return bindings_.value(action); }

QVariantMap KeyMap::shortcutBindings() const {
    QVariantMap result;
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

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

bool KeyMap::setLayout(const QString& name) {
    WorkspaceLayout requested = layout_;
    if (!parse_layout(name, &requested)) {
        diagnostic_ = QStringLiteral("Unknown layout '%1'").arg(name);
        emit changed();
        return false;
    }
    if (requested == layout_)
        return true;
    layout_ = requested;
    diagnostic_.clear();
    qInfo().noquote() << "lapis layout:" << layoutName();
    emit changed();
    return save();
}

bool KeyMap::setTheme(const QString& name) {
    if (!theme_exists(name)) {
        diagnostic_ = QStringLiteral("Unknown theme '%1'").arg(name);
        emit changed();
        return false;
    }
    if (name == theme_)
        return true;
    theme_ = name;
    diagnostic_.clear();
    qInfo().noquote() << "lapis theme:" << theme_;
    emit changed();
    return save();
}

bool KeyMap::setDensity(const QString& name) {
    CardDensity requested = density_;
    if (!parse_density(name, &requested)) {
        diagnostic_ = QStringLiteral("Unknown density '%1'").arg(name);
        emit changed();
        return false;
    }
    if (requested == density_)
        return true;
    density_ = requested;
    diagnostic_.clear();
    qInfo().noquote() << "lapis density:" << densityName();
    emit changed();
    return save();
}

bool KeyMap::save() { return persist(); }

bool KeyMap::persist() {
    const auto fail = [this](const QString& reason) {
        diagnostic_ = QStringLiteral("Could not save %1: %2").arg(source_path_, reason);
        qWarning().noquote() << "lapis config:" << diagnostic_;
        emit changed();
        return false;
    };
    QString path_reason;
    if (!config_path_is_regular(source_path_, &path_reason))
        return fail(path_reason);
    QJsonObject root;
    if (QFileInfo::exists(source_path_)) {
        QFile existing(source_path_);
        if (!existing.open(QIODevice::ReadOnly))
            return fail(existing.errorString());
        if (existing.size() > kMaximumConfigBytes)
            return fail(QStringLiteral("config exceeds 1 MiB"));
        const QByteArray contents = existing.read(kMaximumConfigBytes + 1);
        if (existing.error() != QFileDevice::NoError)
            return fail(existing.errorString());
        existing.close();
        if (contents.size() > kMaximumConfigBytes)
            return fail(QStringLiteral("config exceeds 1 MiB"));
        QJsonParseError parse_error{};
        const QJsonDocument document = QJsonDocument::fromJson(contents, &parse_error);
        if (!contents.isEmpty() && !document.isObject())
            return fail(
                QStringLiteral("existing config is not a valid JSON object; left unchanged"));
        root = document.object();
    }
    root.insert(QStringLiteral("layout"), layoutName());
    root.insert(QStringLiteral("theme"), theme_);
    root.insert(QStringLiteral("density"), densityName());
    if (!root.contains(QStringLiteral("version")))
        root.insert(QStringLiteral("version"), 1);
    QByteArray contents = format_config(root) + '\n';
    if (contents.size() > kMaximumConfigBytes)
        contents = QJsonDocument(root).toJson(QJsonDocument::Compact) + '\n';
    if (contents.size() > kMaximumConfigBytes)
        return fail(QStringLiteral("updated config exceeds 1 MiB; left unchanged"));
    QSaveFile file(source_path_);
    if (!file.open(QIODevice::WriteOnly))
        return fail(file.errorString());
    if (file.write(contents) != contents.size() || !file.commit())
        return fail(file.errorString());
    diagnostic_.clear();
    emit changed();
    return true;
}

} // namespace lapis::desktop
