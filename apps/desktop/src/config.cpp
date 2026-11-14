#include "config.hpp"
#include "app_paths.hpp"
#include "category_colors.hpp"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QKeySequence>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QVariantMap>
#include <algorithm>
#include <array>

#include <utility>

namespace lapis::desktop {
namespace {
// main() sets AA_MacDontSwapCtrlAndMeta: Command is Meta and physical
// Control remains Control. Persist shortcuts using that application contract.
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
     .muted_text = "#545d6b", // 4.8:1 even on the selected row
     .attention = "#c8342c",
     .attention_text = "#ffffff",
     .activity = "#0a6558",
     .fault = "#803ba0",
     .plenty = "#2f8f4e",
     .corner_radius = 6,
     .motion_ms = 140,
     .light = true,
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
     .plenty = "#859900"},
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

void Config::setChimeDiagnostic(const QString& diagnostic) {
    if (chime_diagnostic_ == diagnostic)
        return;
    chime_diagnostic_ = diagnostic;
    emit diagnosticChanged();
}

Config::Config(QObject* parent) : QObject(parent) {
    connect(this, &Config::changed, this, &Config::diagnosticChanged);
    apply_defaults();
    source_path_ = default_source_path();
    // Editors and agents often replace the file rather than write in place,
    // so the folder is watched too, and bursts of events settle first.
    settle_.setSingleShot(true);
    settle_.setInterval(150);
    connect(&settle_, &QTimer::timeout, this, &Config::fileTouched);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, &settle_, qOverload<>(&QTimer::start));
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &settle_,
            qOverload<>(&QTimer::start));
    watch();
    follow_system_appearance();
}

// No source path, so nothing below can name a file to read, watch or write.
Config::Config(DefaultsOnly, QObject* parent) : QObject(parent), file_backed_(false) {
    connect(this, &Config::changed, this, &Config::diagnosticChanged);
    apply_defaults();
    follow_system_appearance();
}

void Config::watch() {
    if (!file_backed_)
        return;
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

void Config::fileTouched() {
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

// lapis.json at the project root, or LAPIS_CONFIG's file (checks keep theirs
// apart from the person's).
QString Config::default_source_path() {
    if (const auto set = qEnvironmentVariable("LAPIS_CONFIG"); !set.isEmpty())
        return QFileInfo(set).absoluteFilePath();
    return QDir(data_directory()).filePath(QStringLiteral("lapis.json"));
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

void Config::apply_defaults() {
#ifdef Q_OS_MACOS
    const QString modifier = QStringLiteral("Meta+");
#else
    const QString modifier = QStringLiteral("Ctrl+Shift+");
#endif
    // As in a browser, Command-W closes what is in front (the side terminal's
    // panel, else the focused agent) and the window only when the category has
    // no agent left; Command-Shift-W closes the window. On the Mac a closed
    // window only hides and lapis keeps running. Command-M minimizes. Command-T
    // opens an agent and Command-N a category. Categories answer to both the
    // original Command-Option-left/right and the vertical Command-Shift-up/down.
    bindings_ = {
        {QStringLiteral("quit"), {modifier + QStringLiteral("Q")}},
        {QStringLiteral("category1"), {modifier + QStringLiteral("1")}},
        {QStringLiteral("category2"), {modifier + QStringLiteral("2")}},
        {QStringLiteral("category3"), {modifier + QStringLiteral("3")}},
        {QStringLiteral("category4"), {modifier + QStringLiteral("4")}},
        {QStringLiteral("category5"), {modifier + QStringLiteral("5")}},
        {QStringLiteral("category6"), {modifier + QStringLiteral("6")}},
        {QStringLiteral("category7"), {modifier + QStringLiteral("7")}},
        {QStringLiteral("category8"), {modifier + QStringLiteral("8")}},
        {QStringLiteral("category9"), {modifier + QStringLiteral("9")}},
        {QStringLiteral("openSettings"), default_settings_shortcuts()},
        {QStringLiteral("reloadConfig"), {modifier + QStringLiteral("R")}},
        {QStringLiteral("newAgent"), {modifier + QStringLiteral("T")}},
        {QStringLiteral("newCategory"), {modifier + QStringLiteral("N")}},
        {QStringLiteral("toggleSidebar"), {modifier + QStringLiteral("B")}},
        // Text size.
        {QStringLiteral("textBigger"),
         {modifier + QStringLiteral("="), modifier + QStringLiteral("+")}},
        {QStringLiteral("textSmaller"), {modifier + QStringLiteral("-")}},
        {QStringLiteral("textReset"), {modifier + QStringLiteral("0")}},
        // A past Claude or Codex conversation, as a new agent.
        {QStringLiteral("resumeConversation"), {modifier + QStringLiteral("O")}},
    };
#ifdef Q_OS_MACOS
    bindings_.insert(QStringLiteral("reopenAgent"), {QStringLiteral("Meta+Shift+T")});
    bindings_.insert(QStringLiteral("closeAgent"), {QStringLiteral("Meta+W")});
    bindings_.insert(QStringLiteral("closeWindow"), {QStringLiteral("Meta+Shift+W")});
    bindings_.insert(QStringLiteral("minimizeWindow"), {QStringLiteral("Meta+M")});
    // The side terminal: Command-` (and Control-`, as in VS Code) shows or hides
    // it, Command-~ picks its machine.
    bindings_.insert(QStringLiteral("toggleTerminal"),
                     {QStringLiteral("Meta+`"), QStringLiteral("Ctrl+`")});
    bindings_.insert(QStringLiteral("chooseTerminal"),
                     {QStringLiteral("Meta+Shift+`"), QStringLiteral("Meta+~")});
#else
    bindings_.insert(QStringLiteral("reopenAgent"), {QStringLiteral("Ctrl+Alt+Shift+T")});
    // Without a dock to bring a closed window back, closing it stays unbound.
    bindings_.insert(QStringLiteral("closeAgent"), {modifier + QStringLiteral("W")});
    bindings_.insert(QStringLiteral("toggleTerminal"), {QStringLiteral("Ctrl+`")});
    bindings_.insert(QStringLiteral("chooseTerminal"),
                     {QStringLiteral("Ctrl+Shift+~"), QStringLiteral("Ctrl+~")});
#endif
#ifdef Q_OS_MACOS
    // Command-[ and ] walk the category rail up and down, beside
    // Command-Shift-[ and ] for the agents across; Command-Shift-J/K do the
    // same from the home row, as in vi.
    bindings_.insert(QStringLiteral("nextCategory"),
                     {QStringLiteral("Meta+]"), QStringLiteral("Meta+Alt+Right"),
                      QStringLiteral("Meta+Shift+Down"), QStringLiteral("Meta+Shift+J")});
    bindings_.insert(QStringLiteral("previousCategory"),
                     {QStringLiteral("Meta+["), QStringLiteral("Meta+Alt+Left"),
                      QStringLiteral("Meta+Shift+Up"), QStringLiteral("Meta+Shift+K")});
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
    previews_visible_ = false;
    finish_sound_ = true;
    alert_sound_file_.clear();
    finish_sound_file_.clear();
    keep_awake_ = true;
    // The window theme first: light mode remembers it as its kind's.
    theme_ = QStringLiteral("lapis");
    load_visual({});
    // Each CLI lists its own models (HarnessModels) unless the config names some.
    sections_ = {};
    touched_.clear();
    touched_keys_.clear();
    parse_sections();
    terminal_font_family_.clear();
    harness_arguments_.clear();
    terminal_font_size_ = kTerminalFontSizeDefault;
    scroll_to_bottom_on_typing_ = true;
    density_ = CardDensity::Comfortable;
}

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool Config::load() {
    apply_defaults();
    diagnostic_.clear();
    loaded_ = false;
    if (!file_backed_) {
        emit changed();
        return false;
    }
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
    // The sidebar navigates; the strip of live previews is opt-in. A file
    // that already says either way keeps its choice.
    previews_visible_ = root.value(QStringLiteral("previewsVisible")).toBool(false);
    scroll_to_bottom_on_typing_ = root.value(QStringLiteral("scrollToBottomOnTyping")).toBool(true);
    const QJsonObject keys = root.value(QStringLiteral("keybindings")).toObject();
    int applied = 0;
    for (auto it = keys.begin(); it != keys.end() && applied < kMaximumActions; ++it, ++applied) {
        const QStringList sequences = normalise(it.value(), &diagnostic_, it.key());
        if (!sequences.isEmpty())
            bindings_.insert(it.key(), sequences);
    }
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
    load_visual(root);
    // Retired features' settings: "usage" and the older top-level "showUsage"
    // (the plan usage meter), "accounts" (lapis-managed plans) and
    // "limitResets" (automatic limit resets). A file that still has them
    // loads, and saving leaves them as written.
    for (const char* name :
         {"newAgent", "harnessUpdates", "nextPrompt", "interactionLog", "tabAway"})
        if (root.contains(QLatin1String(name)))
            sections_.insert(QLatin1String(name), root.value(QLatin1String(name)));
    parse_sections();

    known_contents_ = contents;
    loaded_ = true;
    emit changed();
    return true;
}

void Config::load_harness_arguments(const QJsonValue& value) {
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
            !it.key().isEmpty() && it.key().size() <= 32 && it.value().isArray() &&
            list.size() <= 32 &&
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

void Config::load_harness_updates(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull())
        return;
    if (!value.isObject()) {
        append_diagnostic(&diagnostic_,
                          QStringLiteral("harnessUpdates must map harness names to true or false"));
        return;
    }
    const auto harnesses = value.toObject();
    for (auto it = harnesses.begin(); it != harnesses.end(); ++it) {
        if (it.key().isEmpty() || it.key().size() > 32 || !it.value().isBool()) {
            append_diagnostic(&diagnostic_,
                              QStringLiteral("Ignoring harnessUpdates for '%1': use a harness "
                                             "name of at least one and at most 32 characters "
                                             "and true or false")
                                  .arg(it.key().left(32)));
            continue;
        }
        if (it.value().toBool())
            continue;
        if (harness_updates_off_.size() >= 16) {
            append_diagnostic(&diagnostic_,
                              QStringLiteral("Ignoring harnessUpdates for '%1': at most 16 "
                                             "harnesses are kept")
                                  .arg(it.key().left(32)));
            continue;
        }
        harness_updates_off_.insert(it.key().toLower());
    }
}

void Config::load_alerts(const QJsonObject& root) {
    const auto alerts = root.value(QStringLiteral("alerts")).toObject();
    // "sound" and "repeat" belonged to a retired repeating request chime, and
    // "notify", "awayAfter", "remindAfter" and "judge" to the retired
    // notifications and ping judge; a file that still has them loads, and
    // saving leaves them as written.
    finish_sound_ = alerts.value(QStringLiteral("finished")).toBool(true);
    // "~/" is the home folder and a relative path starts beside lapis.json.
    // File checks and reads belong to ChimeSounds' bounded background work.
    const auto sound_file = [this, &alerts](const QString& key) {
        const QJsonValue value = alerts.value(key);
        if (value.isUndefined() || value.isNull())
            return QString();
        if (!value.isString()) {
            append_diagnostic(
                &diagnostic_,
                QStringLiteral("alerts.%1 must be a path string; playing the built-in chime")
                    .arg(key));
            return QString();
        }
        auto path = value.toString().trimmed();
        if (path.size() > 4096) {
            append_diagnostic(&diagnostic_, QStringLiteral("alerts.%1 path is too long").arg(key));
            return QString();
        }
        if (path.isEmpty())
            return path;
        if (path.startsWith(QLatin1String("~/")))
            path = QDir::home().filePath(path.mid(2));
        path = QDir::cleanPath(QFileInfo(source_path_).dir().absoluteFilePath(path));
        return path;
    };
    alert_sound_file_ = sound_file(QStringLiteral("soundFile"));
    finish_sound_file_ = sound_file(QStringLiteral("finishedFile"));
    keep_awake_ = root.value(QStringLiteral("keepAwake")).toBool(true);
    // A top-level "editor" opened an agent's folder, a retired command; a
    // file that still has it loads, and saving leaves it as written.
}

namespace {
// A theme or font choice is a short lowercase name; the view keeps the table
// and falls back to its default for a name it does not know.
bool visual_name(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9-]{0,31}$"));
    return pattern.match(name).hasMatch();
}
bool visual_size(int size) { return size == 0 || (size >= 10 && size <= 40); }
} // namespace

void Config::load_visual(const QJsonObject& root) {
    const auto visual = root.value(QStringLiteral("visual")).toObject();
    const auto name = [&visual](const char* key, const char* fallback) {
        const auto value = visual.value(QLatin1String(key)).toString();
        return visual_name(value) ? value : QString::fromUtf8(fallback);
    };
    visual_theme_ = name("theme", "terminal");
    visual_font_ = name("font", "system");
    visual_code_font_ = name("codeFont", "system");
    const int size = visual.value(QStringLiteral("size")).toInt();
    visual_size_ = visual_size(size) ? size : 0;
    visual_all_ = visual.value(QStringLiteral("all")).toBool();
    visual_factory_ = visual.value(QStringLiteral("factory")).toBool();
    visual_editor_ = name("editor", "auto");
    // A missing or out-of-range number keeps the default.
    const auto number = [&visual](const char* key, int fallback, int most) {
        const auto value = visual.value(QLatin1String(key));
        const int n = value.toInt(-1);
        return value.isDouble() && n >= 0 && n <= most ? n : fallback;
    };
    visual_keep_mb_ = number("keepMB", 1024, 65536);
    visual_keep_threads_ = number("keepThreads", 32, 1000);

    // Light mode. Each kind's theme must be of that kind; a missing or wrong
    // one falls back to the built-in choice, or to what shows now.
    const bool showing_light = visualThemeLight(visual_theme_);
    const bool window_light = theme_for(theme_).light;
    const auto slot = [&visual](const char* key, bool light, const QString& fallback) {
        const auto value = visual.value(QLatin1String(key)).toString();
        return visual_name(value) && visualThemeLight(value) == light ? value : fallback;
    };
    visual_dark_theme_ =
        slot("darkTheme", false, showing_light ? QStringLiteral("terminal") : visual_theme_);
    visual_light_theme_ =
        slot("lightTheme", true, showing_light ? visual_theme_ : QStringLiteral("claude-light"));
    const auto window_slot = [&visual](const char* key, bool light, const QString& fallback) {
        const auto value = visual.value(QLatin1String(key)).toString();
        return theme_exists(value) && theme_for(value).light == light ? value : fallback;
    };
    window_dark_theme_ =
        window_slot("darkWindowTheme", false, window_light ? QStringLiteral("lapis") : theme_);
    window_light_theme_ =
        window_slot("lightWindowTheme", true, window_light ? theme_ : QStringLiteral("daylight"));
    const auto mode = visual.value(QStringLiteral("appearance")).toString();
    if (mode == QLatin1String("dark") || mode == QLatin1String("light") ||
        mode == QLatin1String("system")) {
        appearance_ = mode;
        appearance_chosen_ = true;
        apply_appearance();
    } else {
        // A file from before light mode shows exactly what it did; the mode
        // is the visual view's kind until it is chosen.
        appearance_ = showing_light ? QStringLiteral("light") : QStringLiteral("dark");
        appearance_chosen_ = false;
    }
}

bool Config::visualThemeLight(const QString& name) {
    return name.endsWith(QLatin1String("-light"));
}

bool Config::lightShown() const {
    return appearance_ == QLatin1String("system") ? !system_dark_
                                                  : appearance_ == QLatin1String("light");
}

void Config::apply_appearance() {
    const bool light = lightShown();
    visual_theme_ = light ? visual_light_theme_ : visual_dark_theme_;
    theme_ = light ? window_light_theme_ : window_dark_theme_;
}

void Config::choose_kind(bool light) {
    if (lightShown() != light) {
        appearance_ = light ? QStringLiteral("light") : QStringLiteral("dark");
        appearance_chosen_ = true;
    }
}

bool Config::setAppearance(const QString& mode) {
    if (mode != QLatin1String("dark") && mode != QLatin1String("light") &&
        mode != QLatin1String("system"))
        return false;
    const bool was_chosen = appearance_chosen_;
    appearance_chosen_ = true;
    if (mode == appearance_) {
        if (!was_chosen) {
            apply_appearance();
            emit changed();
            return save();
        }
        return true;
    }
    appearance_ = mode;
    apply_appearance();
    qInfo().noquote() << "lapis appearance:" << appearance_;
    emit changed();
    return save();
}

void Config::setSystemDark(bool dark) {
    if (system_dark_ == dark)
        return;
    system_dark_ = dark;
    if (appearance_ != QLatin1String("system"))
        return;
    // Followed live and not saved: the file keeps "system".
    apply_appearance();
    emit changed();
}

void Config::follow_system_appearance() {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr)
        return;
    auto* hints = QGuiApplication::styleHints();
    system_dark_ = hints->colorScheme() != Qt::ColorScheme::Light;
    connect(hints, &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme scheme) { setSystemDark(scheme != Qt::ColorScheme::Light); });
}

QVariantMap Config::visual() const {
    return {{QStringLiteral("theme"), visual_theme_},
            {QStringLiteral("font"), visual_font_},
            {QStringLiteral("codeFont"), visual_code_font_},
            {QStringLiteral("size"), visual_size_},
            {QStringLiteral("all"), visual_all_},
            {QStringLiteral("factory"), visual_factory_},
            {QStringLiteral("editor"), visual_editor_},
            {QStringLiteral("appearance"), appearance_},
            {QStringLiteral("light"), lightShown()},
            {QStringLiteral("darkTheme"), visual_dark_theme_},
            {QStringLiteral("lightTheme"), visual_light_theme_},
            {QStringLiteral("darkWindowTheme"), window_dark_theme_},
            {QStringLiteral("lightWindowTheme"), window_light_theme_}};
}

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool Config::setVisual(const QString& key, const QVariant& value) {
    QString* name = key == QLatin1String("theme")      ? &visual_theme_
                    : key == QLatin1String("font")     ? &visual_font_
                    : key == QLatin1String("codeFont") ? &visual_code_font_
                    : key == QLatin1String("editor")   ? &visual_editor_
                                                       : nullptr;
    if (name == &visual_theme_) {
        const auto text = value.toString();
        if (!visual_name(text))
            return false;
        if (visual_theme_ == text)
            return true;
        // Remembered as its kind's theme; choosing one of the other kind
        // switches light mode with it, window included.
        const bool light = visualThemeLight(text);
        (light ? visual_light_theme_ : visual_dark_theme_) = text;
        choose_kind(light);
        apply_appearance();
    } else if (name != nullptr) {
        const auto text = value.toString();
        if (!visual_name(text))
            return false;
        if (*name == text)
            return true;
        *name = text;
    } else if (key == QLatin1String("size")) {
        const int size = value.toInt();
        if (!visual_size(size))
            return false;
        if (visual_size_ == size)
            return true;
        visual_size_ = size;
    } else if (key == QLatin1String("all")) {
        if (visual_all_ == value.toBool())
            return true;
        visual_all_ = value.toBool();
    } else if (key == QLatin1String("factory")) {
        if (visual_factory_ == value.toBool())
            return true;
        visual_factory_ = value.toBool();
    } else {
        return false;
    }
    emit changed();
    return save();
}

// Paths are kept as written (~ is the machine's home); the new-agent forms
// check them where they are used.
void Config::load_agent_defaults(const QJsonValue& value) {
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

void Config::parse_sections() {
    agent_defaults_ = {};
    load_agent_defaults(sections_.value(QStringLiteral("newAgent")));
    harness_updates_off_.clear();
    load_harness_updates(sections_.value(QStringLiteral("harnessUpdates")));
    next_prompt_ = parse_next_prompt(sections_.value(QStringLiteral("nextPrompt")));
    interaction_log_ = parse_interaction_log(sections_.value(QStringLiteral("interactionLog")));
    tab_away_ = parse_tab_away(sections_.value(QStringLiteral("tabAway")));
}

namespace {
constexpr double kMaximumOverdueHours = 168;
bool plain_text(const QVariant& value, qsizetype limit) {
    const auto text = value.toString();
    return value.canConvert<QString>() && text.size() <= limit && !text.contains(QChar::Null);
}
} // namespace

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool Config::setSetting(const QString& path, const QVariant& value) {
    const auto dot = path.indexOf(QLatin1Char('.'));
    if (dot <= 0)
        return false;
    const auto section_name = path.left(dot);
    const auto key = path.mid(dot + 1);
    // What the file gets for the key: a value, or Undefined to remove it.
    QJsonValue stored;
    if (section_name == QLatin1String("newAgent") &&
        (key == QLatin1String("harness") || key == QLatin1String("folder"))) {
        if (!plain_text(value, key == QLatin1String("folder") ? 4096 : 64))
            return false;
        const auto text = value.toString().trimmed();
        stored = text.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(text);
    } else if (section_name == QLatin1String("newAgent") && key == QLatin1String("mode")) {
        const auto mode = value.toString();
        if (!mode.isEmpty() && mode != QLatin1String("edits") && mode != QLatin1String("auto") &&
            mode != QLatin1String("full"))
            return false;
        stored = mode.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(mode);
    } else if ((section_name == QLatin1String("nextPrompt") && key == QLatin1String("auto")) ||
               (section_name == QLatin1String("interactionLog") &&
                key == QLatin1String("enabled"))) {
        if (value.typeId() != QMetaType::Bool)
            return false;
        stored = value.toBool();
    } else if (section_name == QLatin1String("tabAway") && key == QLatin1String("overdueHours")) {
        bool number = false;
        const double hours = value.toDouble(&number);
        if (!number || hours < 0 || hours > kMaximumOverdueHours)
            return false;
        stored = hours;
    } else if (section_name == QLatin1String("harnessUpdates")) {
        static const QRegularExpression cli(QStringLiteral("^[a-z][a-z0-9-]{0,31}$"));
        if (!cli.match(key).hasMatch() || value.typeId() != QMetaType::Bool)
            return false;
        // Absent means updating, as true does; only false is written.
        stored = value.toBool() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(false);
    } else {
        return false;
    }
    auto section = sections_.value(section_name).toObject();
    if (section_name == QLatin1String("harnessUpdates")) {
        // Loader-normalized names can differ in case from the stored key.
        QStringList duplicates;
        for (auto it = section.begin(); it != section.end(); ++it)
            if (key.compare(it.key(), Qt::CaseInsensitive) == 0)
                duplicates.append(it.key());
        for (const auto& duplicate : duplicates)
            section.remove(duplicate);
        const bool normalized = !duplicates.isEmpty();
        const bool enabling = stored.isUndefined();
        if (!normalized && !enabling && section.value(key) == stored)
            return true;
    } else if (section.value(key) == stored) {
        return true;
    }
    if (stored.isUndefined())
        section.remove(key);
    else
        section.insert(key, stored);
    sections_.insert(section_name, section);
    touched_.insert(section_name);
    touched_keys_[section_name].insert(key);
    // Parsing the section again must not repeat its diagnostics.
    const auto diagnostic = diagnostic_;
    parse_sections();
    diagnostic_ = diagnostic;
    emit changed();
    return save();
}

QVariantMap Config::settings() const {
    QStringList updates_off(harness_updates_off_.begin(), harness_updates_off_.end());
    updates_off.sort();
    return {{QStringLiteral("newAgent.harness"), agent_defaults_.harness},
            {QStringLiteral("newAgent.folder"), agent_defaults_.folder},
            {QStringLiteral("newAgent.mode"), agent_defaults_.mode},
            {QStringLiteral("nextPrompt.auto"), next_prompt_.automatic},
            {QStringLiteral("interactionLog.enabled"), interaction_log_.enabled},
            {QStringLiteral("tabAway.overdueHours"), tab_away_.overdueHours},
            {QStringLiteral("harnessUpdatesOff"), updates_off}};
}

QVariantMap Config::defaults() {
    const Config built_in(DefaultsOnly{});
    QVariantMap values = built_in.settings();
    const auto visual = built_in.visual();
    for (auto it = visual.begin(); it != visual.end(); ++it)
        values.insert(QStringLiteral("visual.") + it.key(), it.value());
    values.insert(QStringLiteral("theme"), built_in.themeName());
    values.insert(QStringLiteral("density"), built_in.densityName());
    values.insert(QStringLiteral("sidebarVisible"), built_in.sidebarVisible());
    values.insert(QStringLiteral("previewsVisible"), built_in.previewsVisible());
    values.insert(QStringLiteral("terminalFont.family"), built_in.terminalFontFamily());
    values.insert(QStringLiteral("terminalFont.size"), built_in.terminalFontSize());
    values.insert(QStringLiteral("alerts.finished"), built_in.finishSound());
    values.insert(QStringLiteral("keepAwake"), built_in.keepAwake());
    values.insert(QStringLiteral("scrollToBottomOnTyping"), built_in.scrollToBottomOnTyping());
    return values;
}

void Config::load_terminal_font(const QJsonValue& value) {
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

QString Config::densityName() const { return density_name(density_); }

QVariantList Config::themes() const {
    QVariantList list;
    for (const Theme& theme : kThemes) {
        // The sidebar's category hues by name, for this theme's surfaces.
        // "categoryQuietColors" holds the quieter names of the groups not shown.
        QVariantMap category_colors;
        QVariantMap category_quiet_colors;
        const QColor muted(QString::fromLatin1(theme.muted_text));
        for (const auto& color : kCategoryColors) {
            const auto hex = theme.light ? color.light : color.dark;
            const auto name =
                QString::fromLatin1(color.name.data(), static_cast<qsizetype>(color.name.size()));
            const QColor full(QString::fromLatin1(hex.data(), static_cast<qsizetype>(hex.size())));
            const auto mix = [&](float hue, float ink) {
                return (hue * static_cast<float>(1.0 - kCategoryQuietMix)) +
                       (ink * static_cast<float>(kCategoryQuietMix));
            };
            const auto quiet =
                QColor::fromRgbF(mix(full.redF(), muted.redF()), mix(full.greenF(), muted.greenF()),
                                 mix(full.blueF(), muted.blueF()));
            category_colors.insert(name, full.name());
            category_quiet_colors.insert(name, quiet.name());
        }
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
            {QStringLiteral("cornerRadius"), theme.corner_radius},
            {QStringLiteral("motionDuration"), theme.motion_ms},
            {QStringLiteral("monoChrome"), theme.mono_chrome},
            {QStringLiteral("headingTracking"), theme.heading_tracking},
            {QStringLiteral("categoryColors"), category_colors},
            {QStringLiteral("categoryQuietColors"), category_quiet_colors},
            {QStringLiteral("light"), theme.light},
        });
    }
    return list;
}

QStringList Config::densities() const {
    return {QStringLiteral("comfortable"), QStringLiteral("compact"), QStringLiteral("minimal")};
}

QStringList Config::sequences(const QString& action) const { return bindings_.value(action); }

QVariantMap Config::shortcutBindings() const {
    QVariantMap result;
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

bool Config::reload() {
    const bool ok = load();
    qInfo().noquote() << "lapis config reloaded:" << (ok ? "ok" : "failed")
                      << (diagnostic_.isEmpty() ? QString() : diagnostic_);
    return ok;
}

bool Config::setTheme(const QString& name) {
    if (!theme_exists(name)) {
        diagnostic_ = QStringLiteral("Unknown theme '%1'").arg(name);
        emit changed();
        return false;
    }
    if (name == theme_)
        return true;
    // As with the visual view's themes: remembered as its kind's, and a
    // theme of the other kind switches light mode, the thread included.
    const bool light = theme_for(name).light;
    (light ? window_light_theme_ : window_dark_theme_) = name;
    choose_kind(light);
    apply_appearance();
    diagnostic_.clear();
    qInfo().noquote() << "lapis theme:" << theme_;
    emit changed();
    return save();
}

bool Config::setDensity(const QString& name) {
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

bool Config::setSidebarVisible(bool visible) {
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

bool Config::setScrollToBottomOnTyping(bool on) {
    if (scroll_to_bottom_on_typing_ == on)
        return true;
    const bool previous = scroll_to_bottom_on_typing_;
    scroll_to_bottom_on_typing_ = on;
    if (!save_without_tentative_change()) {
        scroll_to_bottom_on_typing_ = previous;
        emit changed();
        return false;
    }
    emit changed();
    return true;
}

bool Config::setPreviewsVisible(bool visible) {
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

bool Config::setTerminalFontFamily(const QString& family) {
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

bool Config::setTerminalFontSize(int pixels) {
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
bool Config::save_without_tentative_change() {
    const QSignalBlocker blocker(this);
    return persist();
}

bool Config::save() { return persist(); }

namespace {
QString config_lock_path(const QString& path) {
    const QFileInfo source(path);
    const auto canonical = source.canonicalFilePath();
    return (canonical.isEmpty() ? source.absoluteFilePath() : canonical) + QStringLiteral(".lock");
}
bool read_config_for_update(const QString& path, QJsonObject& root, QString* reason) {
    const auto fail = [reason](const QString& why) {
        *reason = why;
        return false;
    };
    QString path_reason;
    if (!config_path_is_regular(path, &path_reason))
        return fail(path_reason);
    if (QFileInfo::exists(path)) {
        QFile existing(path);
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
    return true;
}
} // namespace

// One serialized writer pass; branching stays local to persistence ordering.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool Config::persist() {
    const auto fail = [this](const QString& reason) {
        diagnostic_ = QStringLiteral("Could not save %1: %2").arg(source_path_, reason);
        qWarning().noquote() << "lapis config:" << diagnostic_;
        emit changed();
        return false;
    };
    // A defaults-only config keeps the change in memory, as a save would.
    if (!file_backed_) {
        diagnostic_.clear();
        emit changed();
        return true;
    }
    QLockFile writer(config_lock_path(source_path_));
    if (!writer.tryLock(0))
        return fail(QStringLiteral("configuration is busy or its lock is not writable"));
    QJsonObject root;
    QString read_reason;
    if (!read_config_for_update(source_path_, root, &read_reason))
        return fail(read_reason);
    root.insert(QStringLiteral("theme"), theme_);
    root.insert(QStringLiteral("density"), densityName());
    root.insert(QStringLiteral("sidebarVisible"), sidebar_visible_);
    root.insert(QStringLiteral("previewsVisible"), previews_visible_);
    root.insert(QStringLiteral("scrollToBottomOnTyping"), scroll_to_bottom_on_typing_);
    // A malformed hand-written value must survive an unrelated appearance save.
    if (root.contains(QStringLiteral("terminalFont")) &&
        !root.value(QStringLiteral("terminalFont")).isObject())
        return fail(QStringLiteral("terminalFont must be an object; left unchanged"));
    // Merge into any existing object so unrelated hand-written keys survive.
    QJsonObject font = root.value(QStringLiteral("terminalFont")).toObject();
    font.insert(QStringLiteral("size"), terminal_font_size_);
    if (terminal_font_family_.isEmpty())
        font.remove(QStringLiteral("family"));
    else
        font.insert(QStringLiteral("family"), terminal_font_family_);
    root.insert(QStringLiteral("terminalFont"), font);
    QJsonObject alerts = root.value(QStringLiteral("alerts")).toObject();
    alerts.insert(QStringLiteral("finished"), finish_sound_);
    root.insert(QStringLiteral("alerts"), alerts);
    root.insert(QStringLiteral("keepAwake"), keep_awake_);
    QJsonObject visual = root.value(QStringLiteral("visual")).toObject();
    visual.insert(QStringLiteral("theme"), visual_theme_);
    visual.insert(QStringLiteral("font"), visual_font_);
    visual.insert(QStringLiteral("codeFont"), visual_code_font_);
    visual.insert(QStringLiteral("size"), visual_size_);
    visual.insert(QStringLiteral("all"), visual_all_);
    // Written only while it is on, so files without it stay as they were.
    if (visual_factory_)
        visual.insert(QStringLiteral("factory"), true);
    else
        visual.remove(QStringLiteral("factory"));
    visual.insert(QStringLiteral("editor"), visual_editor_);
    if (appearance_chosen_)
        visual.insert(QStringLiteral("appearance"), appearance_);
    else
        visual.remove(QStringLiteral("appearance"));
    visual.insert(QStringLiteral("darkTheme"), visual_dark_theme_);
    visual.insert(QStringLiteral("lightTheme"), visual_light_theme_);
    visual.insert(QStringLiteral("darkWindowTheme"), window_dark_theme_);
    visual.insert(QStringLiteral("lightWindowTheme"), window_light_theme_);
    visual.remove(QStringLiteral("sendOnReturn")); // Return always sends now
    root.insert(QStringLiteral("visual"), visual);
    // Replay only Settings' changed keys onto the freshly read section, so a
    // concurrent edit to another key in the same section survives.
    for (const auto& name : std::as_const(touched_)) {
        auto section = root.value(name).toObject();
        const auto changes = sections_.value(name).toObject();
        const auto keys = touched_keys_.value(name);
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (name == QLatin1String("harnessUpdates")) {
                QStringList aliases;
                for (auto stored = section.begin(); stored != section.end(); ++stored)
                    if (it->compare(stored.key(), Qt::CaseInsensitive) == 0)
                        aliases.append(stored.key());
                for (const auto& alias : aliases)
                    section.remove(alias);
            }
            if (changes.contains(*it))
                section.insert(*it, changes.value(*it));
            else
                section.remove(*it);
        }
        root.insert(name, section);
    }
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
    for (const auto& name : std::as_const(touched_))
        sections_.insert(name, root.value(name));
    {
        const auto diagnostic = diagnostic_;
        parse_sections();
        diagnostic_ = diagnostic;
    }
    known_contents_ = std::move(contents);
    touched_.clear();
    touched_keys_.clear();
    watch();
    diagnostic_.clear();
    emit changed();
    return true;
}

bool Config::setFinishSound(bool on) {
    finish_sound_ = on;
    return save();
}
bool Config::setKeepAwake(bool on) {
    keep_awake_ = on;
    return save();
}

Config::RemoteSettings Config::remoteSettings() const {
    return {.keepAwake = keep_awake_, .finishSound = finish_sound_};
}

bool Config::applyRemoteSettings(const RemoteSettingsPatch& changes, QString* diagnostic) {
    const bool has_change = changes.keepAwake || changes.finishSound;
    if (!has_change)
        return true;

    const auto previous = remoteSettings();
    if (changes.keepAwake)
        keep_awake_ = *changes.keepAwake;
    if (changes.finishSound)
        finish_sound_ = *changes.finishSound;

    // Observers must not see a batch that could still be rolled back. On
    // failure the file remains unchanged, so restore the fields and keep the
    // reason persist() produced.
    if (save_without_tentative_change()) {
        emit changed();
        return true;
    }
    if (diagnostic)
        *diagnostic = diagnostic_;
    keep_awake_ = previous.keepAwake;
    finish_sound_ = previous.finishSound;
    emit changed();
    return false;
}

} // namespace lapis::desktop
