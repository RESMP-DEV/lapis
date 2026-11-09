#include "code_highlight.hpp"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextDocument>

namespace lapis::desktop::code {
namespace {
enum class Family { code, python, shell, markup, json, yaml, ini, css, diff, markdown, tex, sql };

QSet<QString> words(const char* list) {
    const auto parts = QString::fromLatin1(list).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return {parts.begin(), parts.end()};
}

// Open states a line can leave for the next.
constexpr int in_block_comment = 1;
constexpr int in_triple_double = 2;
constexpr int in_triple_single = 3;
constexpr int in_backtick = 4;
} // namespace

struct Language {
    Family family{Family::code};
    QStringList line_comments;
    QString block_open;
    QString block_close;
    QSet<QString> keywords;
    QSet<QString> types;
    QSet<QString> constants;
    QSet<QString> builtins;
    bool capital_types{}; // Capitalized names are types
    bool preprocessor{};  // a line starting with # is a directive
    bool backticks{};     // `...` is a string that can span lines
    bool triple_quotes{}; // Python's """ and '''
    bool single_quotes{true};
    bool lifetimes{};        // Rust's 'a
    bool case_insensitive{}; // SQL
};

namespace {
const char* const c_keywords = "alignas alignof asm auto break case catch class concept const "
                               "consteval constexpr constinit const_cast continue "
                               "co_await co_return co_yield decltype default delete do "
                               "dynamic_cast else enum explicit export extern final for "
                               "friend goto if import inline module mutable namespace new noexcept "
                               "operator override private protected public "
                               "register reinterpret_cast requires return sizeof static "
                               "static_assert static_cast struct switch template this "
                               "thread_local throw try typedef typeid typename union using virtual "
                               "volatile while __global__ __device__ __host__ "
                               "__shared__ __constant__ __restrict__ __forceinline__ uniform in "
                               "out inout layout kernel fn let var import";
const char* const c_types = "void bool char short int long float double signed unsigned size_t "
                            "ssize_t ptrdiff_t intptr_t uintptr_t int8_t "
                            "int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t wchar_t "
                            "char8_t char16_t char32_t half bfloat16 "
                            "float2 float3 float4 int2 int3 int4 uint2 uint3 uint4 vec2 vec3 vec4 "
                            "mat2 mat3 mat4 dim3 std string vector "
                            "auto f32 f16 u32 i32 id instancetype";
const char* const c_constants = "true false nullptr NULL nil YES NO";

const char* const rust_keywords =
    "as async await break const continue crate dyn else enum extern fn for if impl in let loop "
    "match mod move mut "
    "pub ref return static struct super trait type unsafe use where while macro_rules union";
const char* const rust_types = "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool "
                               "char str String Vec Option Result Box Rc Arc "
                               "HashMap HashSet Self";
const char* const rust_constants = "true false None Some Ok Err";

const char* const go_keywords = "break case chan const continue default defer else fallthrough for "
                                "func go goto if import interface map package "
                                "range return select struct switch type var";
const char* const go_types = "int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 uintptr "
                             "float32 float64 complex64 complex128 string "
                             "error any byte rune bool";
const char* const go_constants = "true false nil iota";

const char* const jvm_keywords = "abstract as assert async await break case catch class companion "
                                 "const continue data default defer do else enum "
                                 "extends extension final finally for fun func guard if implements "
                                 "import in init interface internal is let "
                                 "lateinit namespace new object open operator override package "
                                 "private protected public readonly record return "
                                 "sealed static struct super switch throw throws try typealias val "
                                 "var when where while with yield using "
                                 "protocol extension mutating inout some throws rethrows get set "
                                 "willSet didSet lazy weak unowned fileprivate "
                                 "foreach fixed checked unchecked event delegate";
const char* const jvm_types = "void boolean byte char short int long float double String Int Long "
                              "Double Float Boolean Char Byte Unit Any "
                              "Integer Object List Map Set Array Bool Character dynamic decimal";
const char* const jvm_constants = "true false null nil";

const char* const js_keywords = "async await break case catch class const continue debugger "
                                "default delete do else export extends finally for "
                                "from function get if import in instanceof let new of return set "
                                "static super switch throw try typeof var void "
                                "while with yield type interface enum implements namespace declare "
                                "readonly as keyof infer is satisfies property "
                                "signal required component alias pragma";
const char* const js_types = "string number boolean any unknown never object symbol bigint Array "
                             "Promise Map Set Record Partial Date Error "
                             "RegExp JSON Math Object";
const char* const js_constants = "true false null undefined NaN Infinity";

const char* const py_keywords =
    "and as assert async await break class continue def del elif else except finally for from "
    "global if import in "
    "is lambda match case nonlocal not or pass raise return try while with yield type";
const char* const py_types = "int float str bool list dict set tuple bytes bytearray object "
                             "complex frozenset range type Exception "
                             "ValueError TypeError KeyError RuntimeError";
const char* const py_constants = "True False None NotImplemented Ellipsis __name__ __file__";
const char* const py_builtins = "self cls print len open isinstance enumerate zip map filter "
                                "sorted sum min max abs any all super getattr "
                                "setattr hasattr iter next repr round input";

const char* const sh_keywords =
    "if then else elif fi for while until do done case esac in function select return exit export "
    "local readonly "
    "declare typeset unset source alias eval exec set shift trap break continue time";
const char* const sh_constants = "true false";

const char* const ruby_keywords = "def end if elsif else unless while until for in do begin rescue "
                                  "ensure raise return yield class module then "
                                  "case when and or not require require_relative attr_accessor "
                                  "attr_reader lambda proc puts include extend";
const char* const lua_keywords = "and break do else elseif end for function goto if in local not "
                                 "or repeat return then until while";
const char* const misc_keywords =
    "function fn def defn end if else elif elsif then do while for in return let var const import "
    "module where data "
    "type class instance case of match with lambda use package export from begin loop struct enum "
    "impl val mut pub "
    "and or not unless when cond quote require local my our sub foreach";
const char* const misc_constants = "true false nil null none None True False NULL";

const char* const sql_keywords = "SELECT FROM WHERE JOIN LEFT RIGHT INNER OUTER FULL CROSS ON "
                                 "GROUP BY ORDER HAVING LIMIT OFFSET INSERT INTO "
                                 "VALUES UPDATE SET DELETE CREATE TABLE INDEX VIEW DROP ALTER ADD "
                                 "COLUMN PRIMARY KEY FOREIGN REFERENCES NOT AND OR "
                                 "AS DISTINCT UNION ALL CASE WHEN THEN ELSE END IN IS LIKE ILIKE "
                                 "BETWEEN EXISTS WITH RETURNING DEFAULT ASC DESC "
                                 "BEGIN COMMIT ROLLBACK TRANSACTION IF REPLACE CONSTRAINT UNIQUE "
                                 "CHECK CAST OVER PARTITION WINDOW USING EXPLAIN "
                                 "ANALYZE PRAGMA";
const char* const sql_types = "INT INTEGER BIGINT SMALLINT TEXT VARCHAR CHAR BOOLEAN BOOL REAL "
                              "FLOAT DOUBLE NUMERIC DECIMAL DATE TIME "
                              "TIMESTAMP TIMESTAMPTZ JSON JSONB BLOB UUID SERIAL";
const char* const sql_constants = "NULL TRUE FALSE";

Language make(Family family, QStringList comments, const char* keywords = "",
              const char* types = "", const char* constants = "", const char* builtins = "") {
    Language l;
    l.family = family;
    l.line_comments = std::move(comments);
    l.keywords = words(keywords);
    l.types = words(types);
    l.constants = words(constants);
    l.builtins = words(builtins);
    return l;
}

Language c_like(const char* keywords, const char* types, const char* constants) {
    auto l = make(Family::code, {QStringLiteral("//")}, keywords, types, constants, "this self");
    l.block_open = QStringLiteral("/*");
    l.block_close = QStringLiteral("*/");
    l.capital_types = true;
    return l;
}

QHash<QString, Language> table() {
    QHash<QString, Language> t;
    const auto add = [&t](const char* names, const Language& l) {
        for (const auto& name : QString::fromLatin1(names).split(QLatin1Char(' ')))
            t.insert(name, l);
    };
    auto c = c_like(c_keywords, c_types, c_constants);
    c.preprocessor = true;
    add("c h cpp c++ cc cxx hpp hh hxx cuda cu cuh objc objective-c m mm glsl hlsl metal slang "
        "wgsl ptx opencl cl "
        "proto protobuf",
        c);
    auto rust = c_like(rust_keywords, rust_types, rust_constants);
    rust.lifetimes = true;
    add("rust rs", rust);
    auto go = c_like(go_keywords, go_types, go_constants);
    go.backticks = true;
    add("go golang", go);
    add("java kotlin kt kts scala swift cs csharp c# dart zig groovy gradle",
        c_like(jvm_keywords, jvm_types, jvm_constants));
    auto js = c_like(js_keywords, js_types, js_constants);
    js.backticks = true;
    add("js javascript jsx mjs cjs ts typescript tsx qml json5 jsonc graphql gql", js);
    auto py = make(Family::python, {QStringLiteral("#")}, py_keywords, py_types, py_constants,
                   py_builtins);
    py.triple_quotes = true;
    py.capital_types = true;
    add("python py py3 ipython pyi starlark bzl", py);
    add("bash sh zsh shell console shellsession fish ksh terminal sh-session",
        make(Family::shell, {QStringLiteral("#")}, sh_keywords, "", sh_constants));
    add("ruby rb",
        make(Family::code, {QStringLiteral("#")}, ruby_keywords, "", misc_constants, "self"));
    auto lua = make(Family::code, {QStringLiteral("--")}, lua_keywords, "", misc_constants, "self");
    add("lua", lua);
    auto hs = make(Family::code, {QStringLiteral("--")}, misc_keywords, "", misc_constants);
    hs.block_open = QStringLiteral("{-");
    hs.block_close = QStringLiteral("-}");
    hs.capital_types = true;
    add("haskell hs elm purescript", hs);
    auto ml = make(Family::code, {}, misc_keywords, "", misc_constants);
    ml.block_open = QStringLiteral("(*");
    ml.block_close = QStringLiteral("*)");
    add("ocaml ml fsharp fs", ml);
    add("elixir ex exs erlang erl r julia jl perl pl nim crystal cr powershell ps1 pwsh coffee nix "
        "tcl",
        make(Family::code, {QStringLiteral("#")}, misc_keywords, "", misc_constants, "self this"));
    add("clojure clj lisp el elisp scheme racket asm nasm s llvm mlir ini-lisp",
        make(Family::code, {QStringLiteral(";")}, misc_keywords, "", misc_constants));
    auto php = c_like(misc_keywords, "", misc_constants);
    php.line_comments << QStringLiteral("#");
    add("php", php);
    add("vim viml vimscript", make(Family::code, {QStringLiteral("\"")}, misc_keywords));
    add("make makefile mk cmake dockerfile docker containerfile just justfile",
        make(Family::shell, {QStringLiteral("#")}, sh_keywords, "", sh_constants));
    auto sql = make(Family::sql, {QStringLiteral("--")}, sql_keywords, sql_types, sql_constants);
    sql.block_open = QStringLiteral("/*");
    sql.block_close = QStringLiteral("*/");
    sql.case_insensitive = true;
    add("sql mysql postgres postgresql psql sqlite plsql", sql);
    add("json geojson ipynb", make(Family::json, {}, "", "", "true false null"));
    add("yaml yml",
        make(Family::yaml, {QStringLiteral("#")}, "", "", "true false null yes no on off ~"));
    add("toml ini conf cfg properties env dotenv editorconfig gitconfig",
        make(Family::ini, {QStringLiteral("#"), QStringLiteral(";")}, "", "", "true false"));
    auto markup = make(Family::markup, {});
    markup.block_open = QStringLiteral("<!--");
    markup.block_close = QStringLiteral("-->");
    add("html htm xml svg xhtml vue svelte plist xaml jsx-html", markup);
    auto css = make(Family::css, {}, "important media import supports keyframes font-face layer");
    css.block_open = QStringLiteral("/*");
    css.block_close = QStringLiteral("*/");
    add("css scss sass less", css);
    add("diff patch udiff", make(Family::diff, {}));
    add("md markdown mdx", make(Family::markdown, {}));
    add("tex latex bibtex sty", make(Family::tex, {QStringLiteral("%")}));
    return t;
}

const Language& generic() {
    static const Language l = [] {
        auto g =
            make(Family::code, {QStringLiteral("#"), QStringLiteral("//")}, "", "", misc_constants);
        g.single_quotes = false; // prose-like output: "don't" is not a string
        return g;
    }();
    return l;
}

bool word_start(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
bool word_char(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }
bool operator_char(QChar c) { return QStringLiteral("+-*/%=<>!&|^~?:").contains(c); }

// The next character that is not a space, or a null QChar.
QChar next_visible(const QString& s, int from) {
    for (int i = from; i < s.size(); ++i)
        if (!s[i].isSpace())
            return s[i];
    return {};
}
QChar previous_visible(const QString& s, int before) {
    for (int i = before - 1; i >= 0; --i)
        if (!s[i].isSpace())
            return s[i];
    return {};
}

// Where a quoted string that starts at `at` ends (one past the quote), or
// the line's end; backslash escapes unless `raw`.
int string_end(const QString& s, int at, QChar quote, bool raw) {
    for (int i = at + 1; i < s.size(); ++i) {
        if (!raw && s[i] == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (s[i] == quote)
            return i + 1;
    }
    return static_cast<int>(s.size());
}

int number_end(const QString& s, int at) {
    int i = at;
    while (i < s.size()) {
        const auto c = s[i];
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.')) {
            const bool exponent = (c == QLatin1Char('e') || c == QLatin1Char('E')) &&
                                  i + 1 < s.size() &&
                                  (s[i + 1] == QLatin1Char('+') || s[i + 1] == QLatin1Char('-')) &&
                                  !s.mid(at, 2).startsWith(QLatin1String("0x"));
            i += exponent ? 2 : 1;
        } else {
            break;
        }
    }
    return i;
}

struct Spans {
    std::vector<Span> list;
    void add(int start, int end, Role role) {
        if (end > start && role != Role::plain)
            list.push_back({start, end - start, role});
    }
};

void diff_line(const QString& s, Spans& out) {
    if (s.startsWith(QLatin1String("+++")) || s.startsWith(QLatin1String("---")) ||
        s.startsWith(QLatin1String("diff ")) || s.startsWith(QLatin1String("index ")))
        out.add(0, static_cast<int>(s.size()), Role::comment);
    else if (s.startsWith(QLatin1String("@@")))
        out.add(0, static_cast<int>(s.size()), Role::meta);
    else if (s.startsWith(QLatin1Char('+')))
        out.add(0, static_cast<int>(s.size()), Role::added);
    else if (s.startsWith(QLatin1Char('-')))
        out.add(0, static_cast<int>(s.size()), Role::removed);
}

void markdown_line(const QString& s, Spans& out) {
    const auto trimmed = s.trimmed();
    if (trimmed.startsWith(QLatin1Char('#'))) {
        out.add(0, static_cast<int>(s.size()), Role::heading);
        return;
    }
    if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1Char('>'))) {
        out.add(0, static_cast<int>(s.size()), Role::comment);
        return;
    }
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('`')) {
            const int end = string_end(s, i, QLatin1Char('`'), true);
            out.add(i, end, Role::string);
            i = end - 1;
        } else if ((s[i] == QLatin1Char('-') || s[i] == QLatin1Char('*')) && i + 1 < s.size() &&
                   s[i + 1] == QLatin1Char(' ') && s.left(i).trimmed().isEmpty()) {
            out.add(i, i + 1, Role::keyword);
        } else if (s[i] == QLatin1Char('[')) {
            const int close = static_cast<int>(s.indexOf(QLatin1String("]("), i));
            const int paren = close < 0 ? -1 : static_cast<int>(s.indexOf(QLatin1Char(')'), close));
            if (paren > 0) {
                out.add(i, close + 1, Role::function);
                out.add(close + 1, paren + 1, Role::comment);
                i = paren;
            }
        }
    }
}
} // namespace

const Language& language(const QString& name) {
    static const QHash<QString, Language> languages = table();
    const auto key = name.trimmed().toLower();
    const auto found = languages.constFind(key);
    return found == languages.constEnd() ? generic() : *found;
}

std::vector<Span> tokenize(const QString& s, const Language& l, int& state) {
    Spans out;
    const int n = static_cast<int>(s.size());
    if (l.family == Family::diff) {
        diff_line(s, out);
        return out.list;
    }
    if (l.family == Family::markdown) {
        markdown_line(s, out);
        return out.list;
    }
    int i = 0;
    // Carry on what the previous line left open.
    const auto close_open = [&](const QString& close, Role role, int open_state) {
        const int end = static_cast<int>(s.indexOf(close, i));
        if (end < 0) {
            out.add(i, n, role);
            i = n;
            return;
        }
        out.add(i, end + static_cast<int>(close.size()), role);
        i = end + static_cast<int>(close.size());
        if (state == open_state)
            state = 0;
    };
    if (state == in_block_comment)
        close_open(l.block_close, Role::comment, in_block_comment);
    else if (state == in_triple_double)
        close_open(QStringLiteral("\"\"\""), Role::string, in_triple_double);
    else if (state == in_triple_single)
        close_open(QStringLiteral("'''"), Role::string, in_triple_single);
    else if (state == in_backtick)
        close_open(QStringLiteral("`"), Role::string, in_backtick);
    if (i >= n)
        return out.list;

    // Line-level forms.
    const auto first = next_visible(s, 0);
    if (l.preprocessor && first == QLatin1Char('#')) {
        const int hash = static_cast<int>(s.indexOf(QLatin1Char('#')));
        int end = hash + 1;
        while (end < n && (s[end].isSpace() || word_char(s[end])) &&
               !(s[end].isSpace() && end > hash + 1 && word_char(s[end - 1])))
            ++end;
        out.add(hash, end, Role::meta);
        i = end;
    }
    if (l.family == Family::ini && first == QLatin1Char('[')) {
        out.add(0, n, Role::type);
        return out.list;
    }
    bool command_word = l.family == Family::shell; // the next word runs something
    bool in_tag = false;
    bool after_definition = false; // def / class / fn: the next name is defined

    while (i < n) {
        const QChar c = s[i];
        // Comments.
        bool comment = false;
        for (const auto& prefix : l.line_comments)
            if (QStringView(s).mid(i).startsWith(prefix)) {
                // A shell # needs a space or the line start before it.
                const bool hash_ok = prefix != QLatin1String("#") || l.family != Family::shell ||
                                     i == 0 || s[i - 1].isSpace();
                comment = hash_ok;
                break;
            }
        if (comment && l.family == Family::yaml && i > 0 && !s[i - 1].isSpace())
            comment = false;
        if (comment) {
            out.add(i, n, Role::comment);
            break;
        }
        if (!l.block_open.isEmpty() && QStringView(s).mid(i).startsWith(l.block_open)) {
            const int end = static_cast<int>(s.indexOf(l.block_close, i + l.block_open.size()));
            if (end < 0) {
                out.add(i, n, Role::comment);
                state = in_block_comment;
                break;
            }
            out.add(i, end + static_cast<int>(l.block_close.size()), Role::comment);
            i = end + static_cast<int>(l.block_close.size());
            continue;
        }
        // Markup tags and attributes.
        if (l.family == Family::markup) {
            if (c == QLatin1Char('<')) {
                int j = i + 1;
                if (j < n && (s[j] == QLatin1Char('/') || s[j] == QLatin1Char('!') ||
                              s[j] == QLatin1Char('?')))
                    ++j;
                int end = j;
                while (end < n && (word_char(s[end]) || s[end] == QLatin1Char('-') ||
                                   s[end] == QLatin1Char(':') || s[end] == QLatin1Char('.')))
                    ++end;
                out.add(i, j, Role::op);
                out.add(j, end, Role::tag);
                in_tag = true;
                i = end;
                continue;
            }
            if (c == QLatin1Char('>') ||
                (c == QLatin1Char('/') && i + 1 < n && s[i + 1] == QLatin1Char('>'))) {
                out.add(i, c == QLatin1Char('>') ? i + 1 : i + 2, Role::op);
                i += c == QLatin1Char('>') ? 1 : 2;
                in_tag = false;
                continue;
            }
            if (!in_tag) {
                if (c == QLatin1Char('&')) {
                    const int semi = static_cast<int>(s.indexOf(QLatin1Char(';'), i));
                    if (semi > i && semi - i < 10) {
                        out.add(i, semi + 1, Role::constant);
                        i = semi + 1;
                        continue;
                    }
                }
                ++i;
                continue;
            }
        }
        // Strings.
        const bool quote = c == QLatin1Char('"') || (c == QLatin1Char('\'') && l.single_quotes) ||
                           (c == QLatin1Char('`') && l.backticks);
        if (quote) {
            if (l.lifetimes && c == QLatin1Char('\'') && i + 2 < n && word_start(s[i + 1]) &&
                s[i + 2] != QLatin1Char('\'')) {
                int end = i + 1;
                while (end < n && word_char(s[end]))
                    ++end;
                out.add(i, end, Role::meta);
                i = end;
                continue;
            }
            if (l.triple_quotes && QStringView(s).mid(i, 3) == QString(3, c)) {
                const auto close = QString(3, c);
                const int end = static_cast<int>(s.indexOf(close, i + 3));
                if (end < 0) {
                    out.add(i, n, Role::string);
                    state = c == QLatin1Char('"') ? in_triple_double : in_triple_single;
                    break;
                }
                out.add(i, end + 3, Role::string);
                i = end + 3;
                continue;
            }
            const bool raw = l.family == Family::shell && c == QLatin1Char('\'');
            const int end = string_end(s, i, c, raw);
            if (c == QLatin1Char('`') && (end > n || (end == n && s.back() != QLatin1Char('`')))) {
                out.add(i, n, Role::string);
                state = in_backtick;
                break;
            }
            // A JSON key, or a CSS/YAML quoted key, reads as a property.
            const bool key = (l.family == Family::json || l.family == Family::yaml) &&
                             next_visible(s, end) == QLatin1Char(':');
            out.add(i, end, key ? Role::property : Role::string);
            i = end;
            command_word = false;
            continue;
        }
        // Shell variables and flags.
        if (l.family == Family::shell && c == QLatin1Char('$')) {
            int end = i + 1;
            if (end < n && (s[end] == QLatin1Char('{') || s[end] == QLatin1Char('('))) {
                const QChar close =
                    s[end] == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(')');
                const int found = static_cast<int>(s.indexOf(close, end));
                if (s[end] == QLatin1Char('(')) {
                    out.add(i, end + 1, Role::op);
                    i = end + 1;
                    command_word = true;
                    continue;
                }
                end = found < 0 ? n : found + 1;
            } else {
                while (end < n &&
                       (word_char(s[end]) || QStringLiteral("@#?$!*-").contains(s[end])) &&
                       !(end > i + 1 && !word_char(s[end])))
                    ++end;
            }
            out.add(i, end, Role::property);
            i = end;
            continue;
        }
        if (l.family == Family::shell && c == QLatin1Char('-') && (i == 0 || s[i - 1].isSpace()) &&
            i + 1 < n && (s[i + 1] == QLatin1Char('-') || s[i + 1].isLetter()) && !command_word) {
            int end = i + 1;
            while (end < n && !s[end].isSpace() && s[end] != QLatin1Char('='))
                ++end;
            out.add(i, end, Role::parameter);
            i = end;
            continue;
        }
        // Numbers.
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && s[i + 1].isDigit() &&
                            (i == 0 || !word_char(s[i - 1])))) {
            if (i > 0 && word_char(s[i - 1])) {
                ++i;
                continue;
            }
            const int end = number_end(s, i);
            out.add(i, end, Role::number);
            i = end;
            command_word = false;
            continue;
        }
        // CSS colors and selectors' hashes.
        if (l.family == Family::css && c == QLatin1Char('#')) {
            int end = i + 1;
            while (end < n && word_char(s[end]))
                ++end;
            out.add(i, end, Role::number);
            i = end;
            continue;
        }
        // Decorators and annotations, TeX commands, CSS at-rules.
        if ((c == QLatin1Char('@') &&
             (l.family == Family::python || l.family == Family::code || l.family == Family::css)) ||
            (c == QLatin1Char('\\') && l.family == Family::tex)) {
            int end = i + 1;
            while (end < n &&
                   (word_char(s[end]) || s[end] == QLatin1Char('.') || s[end] == QLatin1Char('-')))
                ++end;
            out.add(i, end, l.family == Family::tex ? Role::keyword : Role::meta);
            i = end;
            continue;
        }
        if (l.family == Family::tex && c == QLatin1Char('$')) {
            const int end = string_end(s, i, QLatin1Char('$'), true);
            out.add(i, end, Role::string);
            i = end;
            continue;
        }
        // Words.
        if (word_start(c) || (c == QLatin1Char('$') && l.family == Family::code)) {
            int end = i + 1;
            while (end < n &&
                   (word_char(s[end]) || (l.family == Family::css && s[end] == QLatin1Char('-')) ||
                    (l.family == Family::shell && command_word &&
                     (s[end] == QLatin1Char('-') || s[end] == QLatin1Char('.') ||
                      s[end] == QLatin1Char('/')))))
                ++end;
            const auto word = s.mid(i, end - i);
            const auto lookup = l.case_insensitive ? word.toUpper() : word;
            const QChar after = next_visible(s, end);
            const QChar before = previous_visible(s, i);
            Role role = Role::plain;
            // Python's string prefixes: r"...", f'...'.
            if (l.family == Family::python && end < n &&
                (s[end] == QLatin1Char('"') || s[end] == QLatin1Char('\'')) &&
                QStringList{QStringLiteral("r"), QStringLiteral("f"), QStringLiteral("b"),
                            QStringLiteral("rb"), QStringLiteral("br"), QStringLiteral("fr"),
                            QStringLiteral("rf"), QStringLiteral("u")}
                    .contains(word.toLower())) {
                out.add(i, end, Role::string);
                i = end;
                continue;
            }
            if (in_tag) {
                role = Role::property;
            } else if (l.family == Family::json) {
                role = l.constants.contains(word) ? Role::constant : Role::plain;
            } else if (l.family == Family::yaml || l.family == Family::ini) {
                const bool key = (after == QLatin1Char(':') && l.family == Family::yaml) ||
                                 (after == QLatin1Char('=') && l.family == Family::ini);
                role = key                          ? Role::property
                       : l.constants.contains(word) ? Role::constant
                                                    : Role::plain;
                if (key) {
                    // A dotted or dashed key is one key.
                    int key_end = end;
                    while (key_end < n && s[key_end] != QLatin1Char(':') &&
                           s[key_end] != QLatin1Char('=') && !s[key_end].isSpace())
                        ++key_end;
                    end = key_end;
                }
            } else if (l.family == Family::css) {
                role = after == QLatin1Char(':') && before != QLatin1Char(':') ? Role::property
                       : before == QLatin1Char('.')                            ? Role::type
                       : l.keywords.contains(word)                             ? Role::keyword
                                                                               : Role::plain;
            } else if (l.family == Family::shell) {
                if (l.keywords.contains(word)) {
                    role = Role::keyword;
                    command_word = word != QLatin1String("in") && word != QLatin1String("case");
                } else if (l.constants.contains(word) && !command_word) {
                    role = Role::constant;
                } else if (command_word) {
                    // VAR=value before a command is an assignment.
                    if (end < n && s[end] == QLatin1Char('=')) {
                        role = Role::property;
                    } else {
                        role = Role::function;
                        command_word = false;
                    }
                }
            } else {
                if (after_definition) {
                    role = before == QLatin1Char('.') ? Role::property : Role::function;
                    if (l.capital_types && word.front().isUpper())
                        role = Role::type;
                    after_definition = false;
                } else if (l.keywords.contains(lookup)) {
                    role = Role::keyword;
                    after_definition =
                        lookup == QLatin1String("def") || lookup == QLatin1String("fn") ||
                        lookup == QLatin1String("func") || lookup == QLatin1String("function") ||
                        lookup == QLatin1String("fun") || lookup == QLatin1String("class") ||
                        lookup == QLatin1String("struct") || lookup == QLatin1String("enum") ||
                        lookup == QLatin1String("trait") || lookup == QLatin1String("interface");
                } else if (l.constants.contains(lookup)) {
                    role = Role::constant;
                } else if (l.builtins.contains(word)) {
                    role = after == QLatin1Char('(') ? Role::function : Role::builtin;
                } else if (l.types.contains(lookup)) {
                    role = Role::type;
                } else if (after == QLatin1Char('(') ||
                           (after == QLatin1Char('!') && l.lifetimes)) {
                    role = Role::function;
                } else if (before == QLatin1Char('.') ||
                           (before == QLatin1Char('>') && i >= 2 && s[i - 2] == QLatin1Char('-'))) {
                    role = Role::property;
                } else if (word.size() > 1 && word == word.toUpper() && word.front().isLetter() &&
                           l.family != Family::sql) {
                    role = Role::constant;
                } else if (l.capital_types && word.front().isUpper()) {
                    role = Role::type;
                }
            }
            out.add(i, end, role);
            i = end;
            continue;
        }
        if (operator_char(c)) {
            int end = i + 1;
            while (end < n && operator_char(s[end]))
                ++end;
            out.add(i, end, Role::op);
            if (l.family == Family::shell)
                command_word = s.mid(i, end - i).contains(QLatin1Char('|')) ||
                               s.mid(i, end - i).contains(QLatin1Char('&'));
            i = end;
            continue;
        }
        if (l.family == Family::shell &&
            (c == QLatin1Char(';') || c == QLatin1Char('(') || c == QLatin1Char('`')))
            command_word = true;
        ++i;
    }
    return out.list;
}

QColor color(Role role, bool light) {
    // Tokyo Night (Night) and Tokyo Night Day, as folke/tokyonight.nvim.
    switch (role) {
    case Role::plain:
        return light ? QColor("#3760bf") : QColor("#c0caf5");
    case Role::comment:
        return light ? QColor("#848cb5") : QColor("#565f89");
    case Role::keyword:
        return light ? QColor("#9854f1") : QColor("#bb9af7");
    case Role::string:
        return light ? QColor("#587539") : QColor("#9ece6a");
    case Role::number:
    case Role::constant:
        return light ? QColor("#b15c00") : QColor("#ff9e64");
    case Role::function:
        return light ? QColor("#2e7de9") : QColor("#7aa2f7");
    case Role::type:
        return light ? QColor("#188092") : QColor("#2ac3de");
    case Role::op:
        return light ? QColor("#006a83") : QColor("#89ddff");
    case Role::property:
        return light ? QColor("#387068") : QColor("#73daca");
    case Role::parameter:
        return light ? QColor("#8c6c3e") : QColor("#e0af68");
    case Role::builtin:
    case Role::tag:
    case Role::removed:
        return light ? QColor("#f52a65") : QColor("#f7768e");
    case Role::meta:
        return light ? QColor("#007197") : QColor("#7dcfff");
    case Role::added:
        return light ? QColor("#587539") : QColor("#9ece6a");
    case Role::heading:
        return light ? QColor("#2e7de9") : QColor("#7aa2f7");
    }
    return {};
}

Highlighter::Highlighter(QTextDocument* document, const QString& name, bool light)
    : QSyntaxHighlighter(document), language_(language(name)), light_(light) {}

void Highlighter::highlightBlock(const QString& text) {
    int state = std::max(0, previousBlockState());
    for (const auto& span : tokenize(text, language_, state)) {
        QTextCharFormat format;
        format.setForeground(color(span.role, light_));
        if (span.role == Role::comment)
            format.setFontItalic(true);
        if (span.role == Role::heading)
            format.setFontWeight(QFont::DemiBold);
        if (span.role == Role::added || span.role == Role::removed) {
            QColor back = color(span.role, light_);
            back.setAlphaF(0.12f);
            format.setBackground(back);
        }
        setFormat(span.start, span.length, format);
    }
    setCurrentBlockState(state);
}
} // namespace lapis::desktop::code
