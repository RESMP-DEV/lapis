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
#include <QKeySequence>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QVariantMap>
#include <algorithm>
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
// accent, and text has the strongest contrast. Activity, attention and fault
// hues stay distinct from the focus accent and from one another. A theme only
// changes the frame around the terminal; the session's own palette still colours
// program output. A plain std::array of literal pointers keeps this a
// constant-initialized table.
constexpr std::array<Theme, 7> kThemes = {{
    {.name = "lapis",
     .label = "Command",
     .background = "#090f16",
     .surface = "#0e1822",
     .card = "#14222e",
     .hovered_card = "#1b303e",
     .focused = "#193638",
     .border = "#263c48",
     .focused_border = "#70d8c5",
     .text = "#e4eef1",
     .muted_text = "#94aab7",
     .attention = "#ffb76b",
     .attention_text = "#201509",
     .activity = "#74a8ff",
     .fault = "#ee7a8a",
     .plenty = "#7bd88f",
     .scarce = "#ef6b73",
     .heading_tracking = 2.0},
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
     .attention_text = "#1a1408",
     .activity = "#6fa8dc",
     .fault = "#ec7b84",
     .plenty = "#8fc98c",
     .scarce = "#e86a6f",
     .corner_radius = 4,
     .motion_ms = 140,
     .heading_tracking = 0.5},
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
     .attention_text = "#ffffff",
     .activity = "#0a6558",
     .fault = "#803ba0",
     .plenty = "#2f8f4e",
     .scarce = "#b3261e",
     .corner_radius = 6,
     .motion_ms = 140,
     .heading_tracking = 0.5},
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
     .attention_text = "#fdf6e3",
     .activity = "#66d7cd",
     .fault = "#b7bcff",
     .plenty = "#859900",
     .scarce = "#e0443f"},
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
     .attention_text = "#1a1008",
     .activity = "#8fd0dc",
     .fault = "#d09cff",
     .plenty = "#a3c96b",
     .scarce = "#ff5a4a",
     .corner_radius = 0,
     .motion_ms = 80,
     .mono_chrome = true,
     .heading_tracking = 2.0},
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
     .attention_text = "#000000",
     .activity = "#4dd2ff",
     .fault = "#ff7af5",
     .plenty = "#5cff8a",
     .scarce = "#ff3b3b",
     .corner_radius = 0,
     .motion_ms = 0,
     .heading_tracking = 0.0},
    // Pure black for OLED panels: every resting surface leaves pixels off, as
    // the terminal engine's default background already does. Lines and text
    // carry the structure; only hover and selection light a faint fill.
    {.name = "oled",
     .label = "OLED black",
     .background = "#000000",
     .surface = "#000000",
     .card = "#000000",
     .hovered_card = "#0c0c0c",
     .focused = "#0a1614",
     .border = "#222222",
     .focused_border = "#62d6c2",
     .text = "#e6e6e6",
     .muted_text = "#8e8e8e",
     .attention = "#ffb454",
     .attention_text = "#000000",
     .activity = "#6aa6ff",
     .fault = "#ff6f86",
     .plenty = "#6fdc8c",
     .scarce = "#ff5f6d",
     .heading_tracking = 1.0},
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

constexpr qsizetype kMaximumFontFamilyLength = 128;

void append_diagnostic(QString* diagnostic, const QString& message) {
    *diagnostic = diagnostic->isEmpty() ? message : *diagnostic + '\n' + message;
}

// A family is an opaque name matched by the font system later; reject only
// values that could never name a font or would corrupt a hand-edited file.
[[nodiscard]] bool valid_font_family(const QString& family) {
    if (family.size() > kMaximumFontFamilyLength || family != family.trimmed())
        return false;
    return std::ranges::none_of(
        family, [](QChar value) { return value.unicode() < 0x20 || value.unicode() == 0x7f; });
}

[[nodiscard]] bool valid_font_size(int pixels) {
    return pixels >= kTerminalFontSizeMinimum && pixels <= kTerminalFontSizeMaximum;
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

const std::array<Theme, 7>& theme_table() { return kThemes; }

bool theme_exists(const QString& name) {
    for (const Theme& theme : kThemes) {
        if (name == QLatin1String(theme.name))
            return true;
    }
    return false;
}

const Theme& theme_for(const QString& name) { return find_theme(name); }

std::optional<QKeyCombination> shifted_punctuation(QKeyCombination combination) {
    static constexpr std::array<std::pair<Qt::Key, Qt::Key>, 11> pairs{{
        {Qt::Key_Comma, Qt::Key_Less},
        {Qt::Key_Period, Qt::Key_Greater},
        {Qt::Key_Slash, Qt::Key_Question},
        {Qt::Key_Semicolon, Qt::Key_Colon},
        {Qt::Key_Apostrophe, Qt::Key_QuoteDbl},
        {Qt::Key_BracketLeft, Qt::Key_BraceLeft},
        {Qt::Key_BracketRight, Qt::Key_BraceRight},
        {Qt::Key_Minus, Qt::Key_Underscore},
        {Qt::Key_Equal, Qt::Key_Plus},
        {Qt::Key_QuoteLeft, Qt::Key_AsciiTilde},
        {Qt::Key_Backslash, Qt::Key_Bar},
    }};
    if (!combination.keyboardModifiers().testFlag(Qt::ShiftModifier))
        return std::nullopt;
    const auto* pair = std::find_if(pairs.begin(), pairs.end(), [&](const auto& candidate) {
        return candidate.first == combination.key();
    });
    if (pair == pairs.end())
        return std::nullopt;
    return QKeyCombination(combination.keyboardModifiers(), pair->second);
}

QStringList default_settings_shortcuts() {
#ifdef Q_OS_MACOS
    return {QStringLiteral("Meta+,")};
#else
    return {QStringLiteral("Ctrl+Shift+,")};
#endif
}

namespace {

[[nodiscard]] QStringList normalise(const QJsonValue& value, QString* diagnostic,
                                    const QString& action) {
    QStringList sequences;
    QStringList entries;
    if (value.isString())
        entries.append(value.toString());
    else if (value.isArray()) {
        for (const auto& entry : value.toArray())
            entries.append(entry.toString());
    }
    const auto add = [&sequences, diagnostic, &action](const QString& text) {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
            return;
        const QKeySequence sequence(trimmed);
        const bool single_chord_only = action == QStringLiteral("openSettings");
        bool invalid = sequence.isEmpty() || (single_chord_only && sequence.count() != 1);
        for (uint chord = 0; chord < static_cast<uint>(sequence.count()); ++chord)
            invalid = invalid || sequence[chord].key() == Qt::Key_unknown;
        if (invalid) {
            const QString message =
                QStringLiteral("%1: invalid key sequence '%2'").arg(action, trimmed);
            *diagnostic = diagnostic->isEmpty() ? message : *diagnostic + '\n' + message;
            return;
        }
        if (sequences.size() >= kMaximumSequencesPerAction) {
            const QString message = QStringLiteral("%1: at most %2 sequences are used")
                                        .arg(action)
                                        .arg(kMaximumSequencesPerAction);
            *diagnostic = diagnostic->isEmpty() ? message : *diagnostic + '\n' + message;
            return;
        }
        sequences.append(trimmed);
    };
    for (const auto& entry : entries)
        add(entry);
    return sequences;
}
} // namespace

KeyMap::KeyMap(QObject* parent) : QObject(parent) {
    apply_defaults();
    source_path_ = default_source_path();
    // Editors and agents often replace the file rather than write in place,
    // so the folder is watched too, and bursts of events settle first.
    settle_.setSingleShot(true);
    settle_.setInterval(150);
    connect(&settle_, &QTimer::timeout, this, &KeyMap::fileTouched);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, &settle_, qOverload<>(&QTimer::start));
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &settle_,
            qOverload<>(&QTimer::start));
    watch();
}

void KeyMap::watch() {
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    const QFileInfo info(source_path_);
    if (info.dir().exists())
        watcher_.addPath(info.absolutePath());
    if (info.exists())
        watcher_.addPath(info.absoluteFilePath());
}

void KeyMap::fileTouched() {
    watch(); // a replaced file is a new file to watch
    QFile file(source_path_);
    QByteArray contents;
    if (file.open(QIODevice::ReadOnly))
        contents = file.read(kMaximumConfigBytes + 1);
    if (contents == known_contents_)
        return;
    qInfo().noquote() << "lapis config changed on disk; reloading";
    static_cast<void>(reload());
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
#ifdef Q_OS_MACOS
    const QString modifier = QStringLiteral("Meta+");
#else
    const QString modifier = QStringLiteral("Ctrl+Shift+");
#endif
    // Command-W closes the focused agent, as in an IDE. Closing the window is
    // the window's own control or Quit. As in a browser, Command-T opens an
    // agent (a tab) and Command-N a category (a window). Categories answer to
    // both the original Command-Option-left/right and the vertical
    // Command-Shift-up/down.
    bindings_ = {
        {QStringLiteral("quit"), {modifier + QStringLiteral("Q")}},
        {QStringLiteral("closeAgent"), {modifier + QStringLiteral("W")}},
        {QStringLiteral("category1"), {modifier + QStringLiteral("1")}},
        {QStringLiteral("category2"), {modifier + QStringLiteral("2")}},
        {QStringLiteral("category3"), {modifier + QStringLiteral("3")}},
        {QStringLiteral("category4"), {modifier + QStringLiteral("4")}},
        {QStringLiteral("openSettings"), default_settings_shortcuts()},
        {QStringLiteral("reloadConfig"), {modifier + QStringLiteral("R")}},
        {QStringLiteral("newAgent"), {modifier + QStringLiteral("T")}},
        {QStringLiteral("newCategory"), {modifier + QStringLiteral("N")}},
        {QStringLiteral("searchAgents"), {modifier + QStringLiteral("K")}},
        {QStringLiteral("nextAttention"), {modifier + QStringLiteral("J")}},
        {QStringLiteral("toggleSidebar"), {modifier + QStringLiteral("B")}},
    };
#ifdef Q_OS_MACOS
    bindings_.insert(QStringLiteral("nextCategory"),
                     {QStringLiteral("Meta+Alt+Right"), QStringLiteral("Meta+Shift+Down")});
    bindings_.insert(QStringLiteral("previousCategory"),
                     {QStringLiteral("Meta+Alt+Left"), QStringLiteral("Meta+Shift+Up")});
    bindings_.insert(QStringLiteral("nextWindow"), {QStringLiteral("Meta+Shift+]")});
    bindings_.insert(QStringLiteral("previousWindow"), {QStringLiteral("Meta+Shift+[")});
    bindings_.insert(QStringLiteral("openCommands"), {QStringLiteral("Meta+Shift+P")});
#else
    bindings_.insert(QStringLiteral("nextCategory"),
                     {modifier + QStringLiteral("Alt+Right"), modifier + QStringLiteral("Down")});
    bindings_.insert(QStringLiteral("previousCategory"),
                     {modifier + QStringLiteral("Alt+Left"), modifier + QStringLiteral("Up")});
    bindings_.insert(QStringLiteral("nextWindow"), {QStringLiteral("Ctrl+Shift+]")});
    bindings_.insert(QStringLiteral("previousWindow"), {QStringLiteral("Ctrl+Shift+[")});
    bindings_.insert(QStringLiteral("openCommands"), {QStringLiteral("Ctrl+Shift+P")});
#endif
    sidebar_visible_ = true;
    previews_visible_ = true;
    alert_sound_ = true;
    finish_sound_ = true;
    alert_repeat_ = 3;
    keep_awake_ = true;
    show_usage_ = true;
    usage_meter_.clear();
    usage_machines_.clear();
    // Each CLI lists its own models (HarnessModels) unless the config names some.
    agent_defaults_ = {};
    terminal_font_family_.clear();
    harness_arguments_.clear();
    terminal_font_size_ = kTerminalFontSizeDefault;
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
    sidebar_visible_ = root.value(QStringLiteral("sidebarVisible")).toBool(true);
    previews_visible_ = root.value(QStringLiteral("previewsVisible")).toBool(true);
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

    load_terminal_font(root.value(QStringLiteral("terminalFont")));
    load_harness_arguments(root.value(QStringLiteral("harnessArguments")));
    load_alerts(root);
    load_usage(root);
    load_agent_defaults(root.value(QStringLiteral("newAgent")));

    known_contents_ = contents;
    loaded_ = true;
    emit changed();
    return true;
}

void KeyMap::load_harness_arguments(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull())
        return;
    if (!value.isObject()) {
        append_diagnostic(&diagnostic_,
                          QStringLiteral("harnessArguments must map harness names to lists"));
        return;
    }
    const auto harnesses = value.toObject();
    for (auto it = harnesses.begin(); it != harnesses.end(); ++it) {
        const auto list = it.value().toArray();
        QStringList arguments;
        const bool valid =
            it.key().size() <= 32 && it.value().isArray() && list.size() <= 32 &&
            std::all_of(list.begin(), list.end(), [&arguments](const QJsonValue& item) {
                const auto text = item.toString();
                if (!item.isString() || text.isEmpty() || text.size() > 1024 ||
                    text.contains(QChar::Null))
                    return false;
                arguments.append(text);
                return true;
            });
        if (!valid) {
            append_diagnostic(&diagnostic_,
                              QStringLiteral("Ignoring harnessArguments for '%1': use a harness "
                                             "name of at most 32 characters and a list of up to "
                                             "32 non-empty strings of at most 1024 characters "
                                             "without NUL")
                                  .arg(it.key().left(32)));
            continue;
        }
        if (harness_arguments_.size() >= 16) {
            append_diagnostic(&diagnostic_,
                              QStringLiteral("Ignoring harnessArguments for '%1': at most 16 "
                                             "harnesses are kept")
                                  .arg(it.key().left(32)));
            continue;
        }
        harness_arguments_.insert(it.key(), arguments);
    }
}

void KeyMap::load_alerts(const QJsonObject& root) {
    const auto alerts = root.value(QStringLiteral("alerts")).toObject();
    alert_sound_ = alerts.value(QStringLiteral("sound")).toBool(true);
    finish_sound_ = alerts.value(QStringLiteral("finished")).toBool(true);
    alert_repeat_ = std::clamp(alerts.value(QStringLiteral("repeat")).toInt(3), 1, 10);
    keep_awake_ = root.value(QStringLiteral("keepAwake")).toBool(true);
}

// {"usage": {"show": true, "meter": ["codex", "claude", "grok"], "machines":
// ["devbox"]}}; an older top-level "showUsage" still counts.
void KeyMap::load_usage(const QJsonObject& root) {
    const auto usage = root.value(QStringLiteral("usage")).toObject();
    show_usage_ = usage.value(QStringLiteral("show"))
                      .toBool(root.value(QStringLiteral("showUsage")).toBool(true));
    const auto names = [](const QJsonValue& value, qsizetype longest) {
        QStringList list;
        for (const auto& item : value.toArray()) {
            const auto name = item.toString().trimmed();
            // Names only: nothing that could be read as an option.
            if (!name.isEmpty() && name.size() <= longest && !name.startsWith(QLatin1Char('-')) &&
                !name.contains(QLatin1Char(' ')) && !list.contains(name) && list.size() < 32)
                list << name;
        }
        return list;
    };
    usage_meter_ = names(usage.value(QStringLiteral("meter")), 32);
    usage_machines_ = names(usage.value(QStringLiteral("machines")), 128);
}

// Paths are kept as written (~ is the machine's home); the new-agent forms
// check them where they are used.
void KeyMap::load_agent_defaults(const QJsonValue& value) {
    const auto section = value.toObject();
    const auto text = [](const QJsonValue& item) {
        const auto string = item.toString().trimmed();
        return string.size() <= 4096 && !string.contains(QChar::Null) ? string : QString();
    };
    agent_defaults_.harness = text(section.value(QStringLiteral("harness")));
    agent_defaults_.folder = text(section.value(QStringLiteral("folder")));
    const auto mode = text(section.value(QStringLiteral("mode")));
    if (mode == QLatin1String("edits") || mode == QLatin1String("auto") ||
        mode == QLatin1String("full"))
        agent_defaults_.mode = mode;
    const auto machines = section.value(QStringLiteral("machines")).toObject();
    for (auto it = machines.begin(); it != machines.end() && it.key().size() <= 128; ++it) {
        const auto folder = text(it.value().toObject().value(QStringLiteral("folder")));
        if (!folder.isEmpty())
            agent_defaults_.machineFolders.insert(it.key(), folder);
    }
    const auto models = section.value(QStringLiteral("models")).toObject();
    for (auto it = models.begin(); it != models.end(); ++it) {
        QStringList names;
        for (const auto& item : it.value().toArray()) {
            const auto name = text(item);
            if (!name.isEmpty() && !name.startsWith(QLatin1Char('-')) && names.size() < 24)
                names << name;
        }
        agent_defaults_.models.insert(it.key(), names);
    }
}

void KeyMap::load_terminal_font(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull())
        return;
    if (!value.isObject()) {
        append_diagnostic(&diagnostic_,
                          QStringLiteral("terminalFont must be an object; using the default"));
        return;
    }
    const QJsonObject font = value.toObject();
    const QJsonValue family = font.value(QStringLiteral("family"));
    if (!family.isUndefined() && !family.isNull()) {
        if (family.isString() && valid_font_family(family.toString()))
            terminal_font_family_ = family.toString();
        else
            append_diagnostic(&diagnostic_,
                              QStringLiteral("terminalFont.family is not a usable font name; "
                                             "using the system fixed-width font"));
    }
    const QJsonValue size = font.value(QStringLiteral("size"));
    if (!size.isUndefined() && !size.isNull()) {
        // toInt() returns the fallback for fractional or non-numeric values.
        const int pixels = size.toInt(-1);
        if (size.isDouble() && valid_font_size(pixels))
            terminal_font_size_ = pixels;
        else
            append_diagnostic(&diagnostic_, QStringLiteral("terminalFont.size must be %1–%2 "
                                                           "pixels; keeping %3")
                                                .arg(kTerminalFontSizeMinimum)
                                                .arg(kTerminalFontSizeMaximum)
                                                .arg(kTerminalFontSizeDefault));
    }
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
            {QStringLiteral("activity"), QString::fromLatin1(theme.activity)},
            {QStringLiteral("fault"), QString::fromLatin1(theme.fault)},
            {QStringLiteral("plenty"), QString::fromLatin1(theme.plenty)},
            {QStringLiteral("scarce"), QString::fromLatin1(theme.scarce)},
            {QStringLiteral("cornerRadius"), theme.corner_radius},
            {QStringLiteral("motionDuration"), theme.motion_ms},
            {QStringLiteral("monoChrome"), theme.mono_chrome},
            {QStringLiteral("headingTracking"), theme.heading_tracking},
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
    const QStringList available = layouts();
    const qsizetype next = (available.indexOf(layoutName()) + 1) % available.size();
    static_cast<void>(setLayout(available.at(next)));
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

bool KeyMap::setSidebarVisible(bool visible) {
    if (sidebar_visible_ == visible)
        return true;
    const bool previous = sidebar_visible_;
    sidebar_visible_ = visible;
    if (!save_without_tentative_change()) {
        sidebar_visible_ = previous;
        emit changed();
        return false;
    }
    emit changed();
    return true;
}

bool KeyMap::setPreviewsVisible(bool visible) {
    if (previews_visible_ == visible)
        return true;
    const bool previous = previews_visible_;
    previews_visible_ = visible;
    if (!save_without_tentative_change()) {
        previews_visible_ = previous;
        emit changed();
        return false;
    }
    emit changed();
    return true;
}

bool KeyMap::setTerminalFontFamily(const QString& family) {
    const QString requested = family.trimmed();
    if (!valid_font_family(requested)) {
        diagnostic_ = QStringLiteral("Font family is not a usable name");
        emit changed();
        return false;
    }
    if (requested == terminal_font_family_)
        return true;
    const QString previous = terminal_font_family_;
    terminal_font_family_ = requested;
    if (!save_without_tentative_change()) {
        terminal_font_family_ = previous;
        emit changed();
        return false;
    }
    qInfo().noquote() << "lapis terminal font:"
                      << (requested.isEmpty() ? QStringLiteral("system default") : requested);
    emit changed();
    return true;
}

bool KeyMap::setTerminalFontSize(int pixels) {
    if (!valid_font_size(pixels)) {
        diagnostic_ = QStringLiteral("Font size must be %1–%2 pixels")
                          .arg(kTerminalFontSizeMinimum)
                          .arg(kTerminalFontSizeMaximum);
        emit changed();
        return false;
    }
    if (pixels == terminal_font_size_)
        return true;
    const int previous = terminal_font_size_;
    terminal_font_size_ = pixels;
    if (!save_without_tentative_change()) {
        terminal_font_size_ = previous;
        emit changed();
        return false;
    }
    emit changed();
    return true;
}

// Observers must not apply a value that is about to be rolled back: a font
// change resizes the live terminal, so the caller emits once with the outcome.
bool KeyMap::save_without_tentative_change() {
    const QSignalBlocker blocker(this);
    return persist();
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
    root.insert(QStringLiteral("sidebarVisible"), sidebar_visible_);
    root.insert(QStringLiteral("previewsVisible"), previews_visible_);
    // Merge into any existing object so unrelated hand-written keys survive.
    QJsonObject font = root.value(QStringLiteral("terminalFont")).toObject();
    font.insert(QStringLiteral("size"), terminal_font_size_);
    if (terminal_font_family_.isEmpty())
        font.remove(QStringLiteral("family"));
    else
        font.insert(QStringLiteral("family"), terminal_font_family_);
    root.insert(QStringLiteral("terminalFont"), font);
    QJsonObject alerts = root.value(QStringLiteral("alerts")).toObject();
    alerts.insert(QStringLiteral("sound"), alert_sound_);
    alerts.insert(QStringLiteral("finished"), finish_sound_);
    alerts.insert(QStringLiteral("repeat"), alert_repeat_);
    root.insert(QStringLiteral("alerts"), alerts);
    root.insert(QStringLiteral("keepAwake"), keep_awake_);
    root.remove(QStringLiteral("showUsage"));
    QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    usage.insert(QStringLiteral("show"), show_usage_);
    root.insert(QStringLiteral("usage"), usage);
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
    known_contents_ = contents;
    watch();
    diagnostic_.clear();
    emit changed();
    return true;
}

bool KeyMap::setAlertSound(bool on) {
    alert_sound_ = on;
    return save();
}
bool KeyMap::setFinishSound(bool on) {
    finish_sound_ = on;
    return save();
}
bool KeyMap::setAlertRepeat(int times) {
    alert_repeat_ = std::clamp(times, 1, 10);
    return save();
}
bool KeyMap::setKeepAwake(bool on) {
    keep_awake_ = on;
    return save();
}
bool KeyMap::setShowUsage(bool on) {
    show_usage_ = on;
    return save();
}

} // namespace lapis::desktop
