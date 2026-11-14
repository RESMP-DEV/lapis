#include "transcript_math.hpp"

#include <QFont>
#include <QHash>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>

namespace lapis::desktop::transcript::math {
namespace {

// Spans longer than this are not looked for: a stray dollar never makes the
// scan walk a whole reply more than once.
constexpr qsizetype max_span = 4000;
// Placeholders are private-use characters from here, one per span.
constexpr char16_t first_placeholder = 0xE000;
constexpr char16_t last_placeholder = 0xF8FF;

bool is_space(QChar c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; }
bool ascii_letter(QChar c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
bool ascii_digit(QChar c) { return c >= u'0' && c <= u'9'; }

qsizetype next_line(const QString& text, qsizetype from) {
    const auto at = text.indexOf(u'\n', from);
    return at < 0 ? text.size() : at + 1;
}

// Whether the newline at `at` ends a paragraph: only spaces, tabs and quote
// markers stand between it and the next newline.
bool blank_line_after(const QString& text, qsizetype at) {
    for (qsizetype i = at + 1; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == u'\n')
            return true;
        if (c != u' ' && c != u'\t' && c != u'>' && c != u'\r')
            return false;
    }
    return true;
}

// Shell and money that pandoc's rule alone would take for math.
// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool looks_like_math(QStringView content) {
    if (content.startsWith(u'{') || content.contains(u"://"))
        return false; // ${VAR}, a URL
    // $HOME/bin, $PATH:..., $VAR. and a bare $VAR$.
    if (!content.isEmpty() && ((content[0] >= u'A' && content[0] <= u'Z') || content[0] == u'_')) {
        qsizetype k = 1;
        while (k < content.size() && ((content[k] >= u'A' && content[k] <= u'Z') ||
                                      ascii_digit(content[k]) || content[k] == u'_'))
            ++k;
        if (k >= 2 && (k == content.size() || content[k] == u'/' || content[k] == u':' ||
                       content[k] == u'.' || content[k] == u'}'))
            return false;
    }
    // "$42.50 for the cables ... $": opens on a number and runs into words
    // (three letters or more, outside commands and braces).
    if (!content.isEmpty() && ascii_digit(content[0])) {
        int depth = 0;
        int letters = 0;
        for (qsizetype i = 0; i < content.size(); ++i) {
            const QChar c = content[i];
            if (c == u'\\') {
                while (i + 1 < content.size() && ascii_letter(content[i + 1]))
                    ++i;
                letters = 0;
                continue;
            }
            if (c == u'{')
                ++depth;
            else if (c == u'}')
                depth = std::max(0, depth - 1);
            if (depth == 0 && ascii_letter(c)) {
                if (++letters >= 3)
                    return false;
            } else {
                letters = 0;
            }
        }
    }
    return true;
}

// The closing index of a single-dollar span opening at `open`, or -1.
qsizetype single_dollar(const QString& text, qsizetype open) {
    const qsizetype n = text.size();
    if (open + 1 >= n || is_space(text[open + 1]))
        return -1;
    if (open > 0 && (text[open - 1].isLetterOrNumber()))
        return -1; // US$5
    for (qsizetype j = open + 1; j < n && j - open < max_span; ++j) {
        const QChar c = text[j];
        if (c == u'\\') {
            ++j;
            continue;
        }
        if (c == u'\n' && blank_line_after(text, j))
            return -1;
        if (c == u'$') {
            if (is_space(text[j - 1]) || (j + 1 < n && ascii_digit(text[j + 1])))
                return -1;
            return looks_like_math(QStringView(text).mid(open + 1, j - open - 1)) ? j : -1;
        }
    }
    return -1;
}

// The index of `close` after `from`, within one paragraph, or -1.
qsizetype closing(const QString& text, qsizetype from, QStringView close) {
    const qsizetype n = text.size();
    for (qsizetype j = from; j < n && j - from < max_span; ++j) {
        if (QStringView(text).mid(j).startsWith(close))
            return j;
        if (text[j] == u'\n' && blank_line_after(text, j))
            return -1;
        if (text[j] == u'\\' && close[0] == u'$')
            ++j; // \$ inside $$..$$
    }
    return -1;
}

// ---- rendering ---------------------------------------------------------

enum class Class : std::uint8_t { ordinary, binary, relation, big, function, open, close, punct };
struct Symbol {
    const char16_t* text;
    Class kind;
};

const QHash<QString, Symbol>& symbols() {
    static const QHash<QString, Symbol> table = [] {
        QHash<QString, Symbol> t;
        const auto add = [&t](const char* name, const char16_t* text, Class kind) {
            t.insert(QString::fromLatin1(name), {text, kind});
        };
        const auto ord = Class::ordinary;
        // Greek.
        add("alpha", u"α", ord);
        add("beta", u"β", ord);
        add("gamma", u"γ", ord);
        add("delta", u"δ", ord);
        add("epsilon", u"ϵ", ord);
        add("varepsilon", u"ε", ord);
        add("zeta", u"ζ", ord);
        add("eta", u"η", ord);
        add("theta", u"θ", ord);
        add("vartheta", u"ϑ", ord);
        add("iota", u"ι", ord);
        add("kappa", u"κ", ord);
        add("lambda", u"λ", ord);
        add("mu", u"μ", ord);
        add("nu", u"ν", ord);
        add("xi", u"ξ", ord);
        add("omicron", u"ο", ord);
        add("pi", u"π", ord);
        add("varpi", u"ϖ", ord);
        add("rho", u"ρ", ord);
        add("varrho", u"ϱ", ord);
        add("sigma", u"σ", ord);
        add("varsigma", u"ς", ord);
        add("tau", u"τ", ord);
        add("upsilon", u"υ", ord);
        add("phi", u"ϕ", ord);
        add("varphi", u"φ", ord);
        add("chi", u"χ", ord);
        add("psi", u"ψ", ord);
        add("omega", u"ω", ord);
        add("Gamma", u"Γ", ord);
        add("Delta", u"Δ", ord);
        add("Theta", u"Θ", ord);
        add("Lambda", u"Λ", ord);
        add("Xi", u"Ξ", ord);
        add("Pi", u"Π", ord);
        add("Sigma", u"Σ", ord);
        add("Upsilon", u"Υ", ord);
        add("Phi", u"Φ", ord);
        add("Psi", u"Ψ", ord);
        add("Omega", u"Ω", ord);
        // Letter-like and miscellaneous.
        add("infty", u"∞", ord);
        add("partial", u"∂", ord);
        add("nabla", u"∇", ord);
        add("emptyset", u"∅", ord);
        add("varnothing", u"∅", ord);
        add("ell", u"ℓ", ord);
        add("hbar", u"ℏ", ord);
        add("Re", u"ℜ", ord);
        add("Im", u"ℑ", ord);
        add("aleph", u"ℵ", ord);
        add("forall", u"∀", ord);
        add("exists", u"∃", ord);
        add("nexists", u"∄", ord);
        add("neg", u"¬", ord);
        add("lnot", u"¬", ord);
        add("prime", u"′", ord);
        add("top", u"⊤", ord);
        add("bot", u"⊥", ord);
        add("angle", u"∠", ord);
        add("triangle", u"△", ord);
        add("ldots", u"…", ord);
        add("dots", u"…", ord);
        add("dotsc", u"…", ord);
        add("cdots", u"⋯", ord);
        add("dotsb", u"⋯", ord);
        add("vdots", u"⋮", ord);
        add("ddots", u"⋱", ord);
        add("dagger", u"†", ord);
        add("checkmark", u"✓", ord);
        add("degree", u"°", ord);
        add("vert", u"|", ord);
        add("Vert", u"‖", ord);
        add("|", u"‖", ord);
        add("%", u"%", ord);
        add("_", u"_", ord);
        add("&", u"&", ord);
        add("#", u"#", ord);
        add("$", u"$", ord);
        // Binary operators.
        const auto bin = Class::binary;
        add("pm", u"±", bin);
        add("mp", u"∓", bin);
        add("times", u"×", bin);
        add("cdot", u"·", bin);
        add("div", u"÷", bin);
        add("ast", u"∗", bin);
        add("star", u"⋆", bin);
        add("circ", u"∘", bin);
        add("bullet", u"•", bin);
        add("oplus", u"⊕", bin);
        add("ominus", u"⊖", bin);
        add("otimes", u"⊗", bin);
        add("odot", u"⊙", bin);
        add("oslash", u"⊘", bin);
        add("cup", u"∪", bin);
        add("cap", u"∩", bin);
        add("setminus", u"∖", bin);
        add("land", u"∧", bin);
        add("wedge", u"∧", bin);
        add("lor", u"∨", bin);
        add("vee", u"∨", bin);
        add("bmod", u"mod", bin);
        add("mod", u"mod", bin);
        // Relations and arrows.
        const auto rel = Class::relation;
        add("leq", u"≤", rel);
        add("le", u"≤", rel);
        add("leqslant", u"≤", rel);
        add("geq", u"≥", rel);
        add("ge", u"≥", rel);
        add("geqslant", u"≥", rel);
        add("neq", u"≠", rel);
        add("ne", u"≠", rel);
        add("approx", u"≈", rel);
        add("equiv", u"≡", rel);
        add("sim", u"∼", rel);
        add("simeq", u"≃", rel);
        add("cong", u"≅", rel);
        add("propto", u"∝", rel);
        add("lesssim", u"≲", rel);
        add("gtrsim", u"≳", rel);
        add("ll", u"≪", rel);
        add("gg", u"≫", rel);
        add("triangleq", u"≜", rel);
        add("coloneqq", u"≔", rel);
        add("to", u"→", rel);
        add("rightarrow", u"→", rel);
        add("longrightarrow", u"⟶", rel);
        add("leftarrow", u"←", rel);
        add("gets", u"←", rel);
        add("longleftarrow", u"⟵", rel);
        add("leftrightarrow", u"↔", rel);
        add("Rightarrow", u"⇒", rel);
        add("Leftarrow", u"⇐", rel);
        add("Leftrightarrow", u"⇔", rel);
        add("implies", u"⟹", rel);
        add("impliedby", u"⟸", rel);
        add("iff", u"⟺", rel);
        add("mapsto", u"↦", rel);
        add("longmapsto", u"⟼", rel);
        add("uparrow", u"↑", rel);
        add("downarrow", u"↓", rel);
        add("in", u"∈", rel);
        add("notin", u"∉", rel);
        add("ni", u"∋", rel);
        add("subset", u"⊂", rel);
        add("subseteq", u"⊆", rel);
        add("supset", u"⊃", rel);
        add("supseteq", u"⊇", rel);
        add("mid", u"∣", rel);
        add("parallel", u"∥", rel);
        add("perp", u"⊥", rel);
        add("models", u"⊨", rel);
        add("vdash", u"⊢", rel);
        // Large operators.
        const auto big = Class::big;
        add("sum", u"∑", big);
        add("prod", u"∏", big);
        add("coprod", u"∐", big);
        add("int", u"∫", big);
        add("iint", u"∬", big);
        add("iiint", u"∭", big);
        add("oint", u"∮", big);
        add("bigcup", u"⋃", big);
        add("bigcap", u"⋂", big);
        add("bigoplus", u"⨁", big);
        add("bigotimes", u"⨂", big);
        add("bigvee", u"⋁", big);
        add("bigwedge", u"⋀", big);
        // Function names, set upright.
        for (const char* name :
             {"sin",  "cos",  "tan",  "cot",  "sec",    "csc",    "arcsin", "arccos", "arctan",
              "sinh", "cosh", "tanh", "coth", "log",    "ln",     "lg",     "exp",    "max",
              "min",  "sup",  "inf",  "lim",  "liminf", "limsup", "det",    "arg",    "deg",
              "gcd",  "dim",  "ker",  "hom",  "Pr",     "argmax", "argmin", "tr",     "sgn"})
            t.insert(QString::fromLatin1(name), {nullptr, Class::function});
        // Delimiters.
        add("langle", u"⟨", Class::open);
        add("rangle", u"⟩", Class::close);
        add("lfloor", u"⌊", Class::open);
        add("rfloor", u"⌋", Class::close);
        add("lceil", u"⌈", Class::open);
        add("rceil", u"⌉", Class::close);
        add("lvert", u"|", Class::open);
        add("rvert", u"|", Class::close);
        add("lVert", u"‖", Class::open);
        add("rVert", u"‖", Class::close);
        add("{", u"{", Class::open);
        add("}", u"}", Class::close);
        add("lbrace", u"{", Class::open);
        add("rbrace", u"}", Class::close);
        return t;
    }();
    return table;
}

QString double_struck(QChar letter) {
    switch (letter.unicode()) {
    case u'C':
        return QStringLiteral("ℂ");
    case u'H':
        return QStringLiteral("ℍ");
    case u'N':
        return QStringLiteral("ℕ");
    case u'P':
        return QStringLiteral("ℙ");
    case u'Q':
        return QStringLiteral("ℚ");
    case u'R':
        return QStringLiteral("ℝ");
    case u'Z':
        return QStringLiteral("ℤ");
    default:
        break;
    }
    char32_t code = 0;
    if (letter >= u'A' && letter <= u'Z')
        code = 0x1D538 + (letter.unicode() - u'A');
    else if (letter >= u'a' && letter <= u'z')
        code = 0x1D552 + (letter.unicode() - u'a');
    else if (ascii_digit(letter))
        code = 0x1D7D8 + (letter.unicode() - u'0');
    else
        return QString(letter);
    return QString::fromUcs4(&code, 1);
}

// Characters in a rendered argument that make it need parentheses when it
// becomes a fraction's numerator or denominator or a root's radicand.
bool compound(const QString& text) {
    static const QString operators = QStringLiteral(" +−±∓×·÷=<>≤≥≠≈/,∑∏∫ ∖∪∩∧∨⊕⊗⊙→←");
    return std::any_of(text.begin(), text.end(), [](QChar c) { return operators.contains(c); });
}

class Parser {
  public:
    explicit Parser(const QString& source) : s_(source) {}

    Rendered run() {
        sequence(false);
        if (failed_ || p_ < s_.size())
            return {};
        // Trim the spaces operators put at either end.
        while (!runs_.empty() && runs_.front().text.trimmed().isEmpty())
            runs_.erase(runs_.begin());
        while (!runs_.empty() && runs_.back().text.trimmed().isEmpty())
            runs_.pop_back();
        if (runs_.empty())
            return {};
        auto& first = runs_.front().text;
        while (!first.isEmpty() && first[0] == u' ')
            first.remove(0, 1);
        auto& last = runs_.back().text;
        while (!last.isEmpty() && last.back() == u' ')
            last.chop(1);
        return {true, std::move(runs_)};
    }

  private:
    enum class Last : std::uint8_t { none, operand, operation };

    void fail() { failed_ = true; }

    void put(const QString& text, bool italic = false) {
        if (text.isEmpty())
            return;
        // Never two spaces in a row where an operator follows a space.
        QString out = text;
        if (out.startsWith(u' ') && !runs_.empty() && runs_.back().text.endsWith(u' '))
            out.remove(0, 1);
        const bool bold = bold_;
        if (!runs_.empty() && runs_.back().shift == shift_ && runs_.back().italic == italic &&
            runs_.back().bold == bold) {
            runs_.back().text += out;
            return;
        }
        runs_.push_back({out, shift_, italic, bold});
    }

    void operand(const QString& text, bool italic = false) {
        if (pending_space_ && shift_ == Run::Shift::none)
            put(QStringLiteral(" "));
        pending_space_ = false;
        put(text, italic);
        last_ = Last::operand;
    }

    void place(const QString& text, Class kind) {
        const bool top = shift_ == Run::Shift::none;
        switch (kind) {
        case Class::ordinary:
            operand(text);
            return;
        case Class::binary:
            if (last_ == Last::operand && top)
                put(QLatin1Char(' ') + text + QLatin1Char(' '));
            else
                put(text);
            break;
        case Class::relation:
            if (!top)
                put(text);
            else if (last_ == Last::none)
                put(text + QLatin1Char(' '));
            else
                put(QLatin1Char(' ') + text + QLatin1Char(' '));
            break;
        case Class::punct:
            put(top ? text + QLatin1Char(' ') : text);
            break;
        case Class::big:
            operand(text);
            pending_space_ = true; // after its limits
            last_ = Last::operation;
            return;
        case Class::function:
            operand(text);
            pending_space_ = true;
            return;
        case Class::open:
            if (pending_space_ && text != u"(" && top)
                put(QStringLiteral(" "));
            pending_space_ = false;
            put(text);
            break;
        case Class::close:
            put(text);
            last_ = Last::operand;
            return;
        }
        pending_space_ = false;
        last_ = Last::operation;
    }

    void skip_spaces() {
        while (p_ < s_.size() && is_space(s_[p_]))
            ++p_;
    }

    void sequence(bool group) {
        if (++depth_ > 48) {
            fail();
            return;
        }
        while (!failed_ && p_ < s_.size()) {
            if (s_[p_] == u'}') {
                if (!group)
                    fail();
                break;
            }
            atom();
        }
        --depth_;
    }

    void group() { // at '{'
        ++p_;
        sequence(true);
        if (p_ >= s_.size() || s_[p_] != u'}') {
            fail();
            return;
        }
        ++p_;
    }

    // One argument: a group, a command with its own arguments, or a character.
    void argument() {
        skip_spaces();
        if (p_ >= s_.size()) {
            fail();
            return;
        }
        if (s_[p_] == u'{')
            group();
        else
            atom();
    }

    // An argument's raw text, braces stripped: \text{...} and friends.
    std::optional<QString> raw_argument() {
        skip_spaces();
        if (p_ >= s_.size())
            return std::nullopt;
        if (s_[p_] != u'{')
            return QString(s_[p_++]);
        int depth = 0;
        const qsizetype start = p_ + 1;
        for (; p_ < s_.size(); ++p_) {
            if (s_[p_] == u'\\') {
                ++p_;
                continue;
            }
            if (s_[p_] == u'{')
                ++depth;
            else if (s_[p_] == u'}' && --depth == 0) {
                ++p_;
                return s_.mid(start, p_ - 1 - start);
            }
        }
        return std::nullopt;
    }

    // Renders an argument on its own, for fractions, roots and accents.
    std::vector<Run> rendered_argument() {
        auto saved = std::exchange(runs_, {});
        const auto saved_last = std::exchange(last_, Last::none);
        const auto saved_pending = std::exchange(pending_space_, false);
        argument();
        auto out = std::exchange(runs_, std::move(saved));
        last_ = saved_last;
        pending_space_ = saved_pending;
        // Spaces an operator left at either end belong to nothing now.
        while (!out.empty() && out.front().text.startsWith(u' ')) {
            out.front().text.remove(0, 1);
            if (out.front().text.isEmpty())
                out.erase(out.begin());
        }
        while (!out.empty() && out.back().text.endsWith(u' ')) {
            out.back().text.chop(1);
            if (out.back().text.isEmpty())
                out.pop_back();
        }
        return out;
    }

    static QString text_of(const std::vector<Run>& runs, bool unshifted_only = false) {
        QString out;
        for (const auto& run : runs)
            if (!unshifted_only || run.shift == Run::Shift::none)
                out += run.text;
        return out;
    }

    void append(std::vector<Run> runs, bool parenthesize) {
        if (pending_space_ && shift_ == Run::Shift::none)
            put(QStringLiteral(" "));
        pending_space_ = false;
        if (parenthesize)
            put(QStringLiteral("("));
        for (auto& run : runs) {
            if (!runs_.empty() && runs_.back().shift == run.shift &&
                runs_.back().italic == run.italic && runs_.back().bold == run.bold)
                runs_.back().text += run.text;
            else
                runs_.push_back(std::move(run));
        }
        if (parenthesize)
            put(QStringLiteral(")"));
        last_ = Last::operand;
    }

    void script(QChar mark) {
        if (shift_ != Run::Shift::none) {
            // Qt sets one level of scripts: a nested one keeps its mark.
            put(QString(mark));
            argument();
            return;
        }
        const auto saved_last = last_;
        const auto saved_pending = pending_space_;
        pending_space_ = false;
        last_ = Last::none;
        shift_ = mark == u'^' ? Run::Shift::super : Run::Shift::sub;
        argument();
        shift_ = Run::Shift::none;
        last_ = saved_last == Last::none ? Last::operand : saved_last;
        pending_space_ = saved_pending;
    }

    void atom() {
        const QChar c = s_[p_];
        if (is_space(c)) {
            ++p_;
            return;
        }
        if (c == u'{') {
            group();
            return;
        }
        if (c == u'^' || c == u'_') {
            ++p_;
            script(c);
            return;
        }
        if (c == u'\\') {
            command();
            return;
        }
        ++p_;
        if (c == u'&' || c == u'#' || c == u'%' || c == u'$') {
            fail(); // alignment, parameters, comments: not inline math
            return;
        }
        if (c.isLetter()) {
            operand(QString(c), !upright_ && !bold_);
            return;
        }
        if (ascii_digit(c) || c == u'.') {
            QString number(c);
            // Digits group with "," and "." between them: 1,157,600.
            while (p_ < s_.size() &&
                   (ascii_digit(s_[p_]) || ((s_[p_] == u',' || s_[p_] == u'.') &&
                                            p_ + 1 < s_.size() && ascii_digit(s_[p_ + 1]))))
                number += s_[p_++];
            operand(number);
            return;
        }
        switch (c.unicode()) {
        case u'+':
            place(QStringLiteral("+"), Class::binary);
            return;
        case u'-':
            place(QStringLiteral("−"), Class::binary);
            return;
        case u'*':
            place(QStringLiteral("∗"), Class::binary);
            return;
        case u'/':
            put(QStringLiteral("/"));
            last_ = Last::operation;
            return;
        case u'=':
        case u'<':
        case u'>':
            place(QString(c), Class::relation);
            return;
        case u',':
        case u';':
            place(QString(c), Class::punct);
            return;
        case u':':
            place(QStringLiteral(":"), Class::punct);
            return;
        case u'(':
        case u'[':
            place(QString(c), Class::open);
            return;
        case u')':
        case u']':
            place(QString(c), Class::close);
            return;
        case u'\'':
            put(QStringLiteral("′"));
            last_ = Last::operand;
            return;
        case u'~':
            put(QStringLiteral(" "));
            return;
        default:
            operand(QString(c));
        }
    }

    // Font commands: \mathbf{..}, \mathbb{..}, \mathrm{..}, ...
    void styled(bool bold, bool upright, bool struck) {
        const auto saved = std::make_pair(bold_, upright_);
        bold_ = bold_ || bold;
        upright_ = upright_ || upright;
        if (struck) {
            // Each letter as its double-struck character.
            const auto raw = raw_argument();
            if (!raw || raw->contains(u'\\')) {
                fail();
            } else {
                QString text;
                for (const QChar letter : raw->trimmed())
                    if (!is_space(letter))
                        text += double_struck(letter);
                operand(text);
            }
        } else {
            argument();
        }
        std::tie(bold_, upright_) = saved;
    }

    // \text{..}, \operatorname{..}: words as written, upright.
    void text_command(bool function, bool bold, bool italic) {
        const auto raw = raw_argument();
        if (!raw) {
            fail();
            return;
        }
        if (raw->contains(u'\\') || raw->contains(u'$') || raw->contains(u'{')) {
            // Commands inside the text: set it as math, upright.
            Parser inner(*raw);
            inner.upright_ = true;
            inner.bold_ = bold_ || bold;
            inner.shift_ = shift_;
            auto rendered = inner.run();
            if (!rendered.ok) {
                fail();
                return;
            }
            append(std::move(rendered.runs), false);
        } else {
            const auto saved = bold_;
            bold_ = bold_ || bold;
            operand(*raw, italic);
            bold_ = saved;
        }
        if (function)
            pending_space_ = true;
    }

    void fraction(bool binomial) {
        if (++fractions_ > 2) {
            fail(); // three deep reads worse than its source
            return;
        }
        auto top = rendered_argument();
        auto bottom = rendered_argument();
        --fractions_;
        if (failed_ || top.empty() || bottom.empty()) {
            fail();
            return;
        }
        if (binomial) {
            operand(QStringLiteral("C("));
            append(std::move(top), false);
            put(QStringLiteral(", "));
            append(std::move(bottom), false);
            put(QStringLiteral(")"));
            last_ = Last::operand;
            return;
        }
        const bool wrap_top = compound(text_of(top));
        const bool wrap_bottom = compound(text_of(bottom));
        append(std::move(top), wrap_top);
        put(QStringLiteral("/"));
        append(std::move(bottom), wrap_bottom);
    }

    void root() {
        skip_spaces();
        std::vector<Run> index;
        if (p_ < s_.size() && s_[p_] == u'[') {
            const auto close = s_.indexOf(u']', p_);
            if (close < 0) {
                fail();
                return;
            }
            const auto written = s_.mid(p_ + 1, close - p_ - 1);
            Parser inner(written);
            auto rendered = inner.run();
            if (!rendered.ok) {
                fail();
                return;
            }
            index = std::move(rendered.runs);
            p_ = close + 1;
        }
        auto radicand = rendered_argument();
        if (failed_ || radicand.empty()) {
            fail();
            return;
        }
        const auto plain_text = text_of(radicand, true);
        bool simple = !compound(text_of(radicand)) &&
                      (plain_text.size() <= 1 ||
                       std::all_of(plain_text.begin(), plain_text.end(),
                                   [](QChar c) { return ascii_digit(c) || c == u'.'; }));
        if (!index.empty()) {
            if (shift_ != Run::Shift::none) {
                fail();
                return;
            }
            for (auto& run : index)
                run.shift = Run::Shift::super;
            append(std::move(index), false);
        }
        operand(QStringLiteral("√"));
        append(std::move(radicand), !simple);
    }

    void accent(char16_t mark, bool each) {
        auto inner = rendered_argument();
        if (failed_ || inner.size() != 1 || inner.front().shift != shift_) {
            fail();
            return;
        }
        auto& run = inner.front();
        const auto characters = run.text.toUcs4();
        if (characters.size() != 1 && !each) {
            fail();
            return;
        }
        QString text;
        for (const char32_t character : characters) {
            text += QString::fromUcs4(&character, 1);
            text += QChar(mark);
        }
        run.text = std::move(text);
        append(std::move(inner), false);
    }

    // Cohesive parser/test flow; branching stays local.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void command() {
        ++p_; // the backslash
        if (p_ >= s_.size()) {
            fail();
            return;
        }
        QString name;
        if (!ascii_letter(s_[p_])) {
            const QChar c = s_[p_++];
            switch (c.unicode()) {
            case u',':
            case u':':
            case u'>':
                put(QStringLiteral(" "));
                return;
            case u';':
            case u' ':
                put(QStringLiteral(" "));
                return;
            case u'!':
                return;
            case u'\\':
                fail(); // a line break: aligned or multi-line math
                return;
            default:
                name = QString(c);
            }
        } else {
            while (p_ < s_.size() && ascii_letter(s_[p_]))
                name += s_[p_++];
        }

        if (const auto found = symbols().constFind(name); found != symbols().cend()) {
            if (found->kind == Class::function)
                place(name, Class::function);
            else
                place(QString::fromUtf16(found->text), found->kind);
            if (name == u"bmod" || name == u"mod")
                last_ = Last::operation;
            return;
        }
        static const QStringList dropped = {
            QStringLiteral("left"),      QStringLiteral("right"),
            QStringLiteral("middle"),    QStringLiteral("big"),
            QStringLiteral("Big"),       QStringLiteral("bigg"),
            QStringLiteral("Bigg"),      QStringLiteral("bigl"),
            QStringLiteral("bigr"),      QStringLiteral("Bigl"),
            QStringLiteral("Bigr"),      QStringLiteral("biggl"),
            QStringLiteral("biggr"),     QStringLiteral("Biggl"),
            QStringLiteral("Biggr"),     QStringLiteral("limits"),
            QStringLiteral("nolimits"),  QStringLiteral("displaystyle"),
            QStringLiteral("textstyle"), QStringLiteral("scriptstyle"),
            QStringLiteral("nonumber"),  QStringLiteral("notag"),
        };
        if (dropped.contains(name)) {
            // \left. and \right. stand for no delimiter.
            skip_spaces();
            if ((name == u"left" || name == u"right" || name == u"middle") && p_ < s_.size() &&
                s_[p_] == u'.')
                ++p_;
            return;
        }
        if (name == u"quad") {
            put(QStringLiteral(" "));
            return;
        }
        if (name == u"qquad") {
            put(QStringLiteral("  "));
            return;
        }
        if (name == u"enspace" || name == u"thinspace" || name == u"medspace" ||
            name == u"thickspace") {
            put(QStringLiteral(" "));
            return;
        }
        if (name == u"frac" || name == u"dfrac" || name == u"tfrac" || name == u"cfrac") {
            fraction(false);
            return;
        }
        if (name == u"binom" || name == u"dbinom" || name == u"tbinom") {
            fraction(true);
            return;
        }
        if (name == u"sqrt") {
            root();
            return;
        }
        if (name == u"mathbb" || name == u"Bbb") {
            styled(false, true, true);
            return;
        }
        if (name == u"mathbf" || name == u"boldsymbol" || name == u"bm" || name == u"pmb") {
            styled(true, false, false);
            return;
        }
        if (name == u"mathcal" || name == u"mathscr" || name == u"mathfrak" || name == u"mathsf" ||
            name == u"mathtt") {
            styled(false, true, false);
            return;
        }
        if (name == u"mathit") {
            styled(false, false, false);
            return;
        }
        if (name == u"mathrm" || name == u"text" || name == u"textrm" || name == u"mbox" ||
            name == u"textnormal" || name == u"textsf" || name == u"texttt") {
            text_command(false, false, false);
            return;
        }
        if (name == u"textbf") {
            text_command(false, true, false);
            return;
        }
        if (name == u"textit" || name == u"emph") {
            text_command(false, false, true);
            return;
        }
        if (name == u"operatorname") {
            skip_spaces();
            if (p_ < s_.size() && s_[p_] == u'*')
                ++p_;
            text_command(true, false, false);
            return;
        }
        if (name == u"pmod") {
            const auto raw = rendered_argument();
            if (failed_ || raw.empty()) {
                fail();
                return;
            }
            put(QStringLiteral(" (mod "));
            append(raw, false);
            put(QStringLiteral(")"));
            return;
        }
        // A hat or tilde over a few letters (\tilde{KV}) marks each of them.
        if (name == u"hat" || name == u"widehat") {
            accent(0x0302, true);
            return;
        }
        if (name == u"tilde" || name == u"widetilde") {
            accent(0x0303, true);
            return;
        }
        if (name == u"bar" || name == u"overline") {
            accent(0x0305, true);
            return;
        }
        if (name == u"vec") {
            accent(0x20D7, false);
            return;
        }
        if (name == u"dot") {
            accent(0x0307, false);
            return;
        }
        if (name == u"ddot") {
            accent(0x0308, false);
            return;
        }
        if (name == u"not") {
            // \not= is ≠, \not\in ∉; another relation gets a slash.
            skip_spaces();
            const auto before = runs_.size();
            const auto text_before = before > 0 ? runs_.back().text.size() : 0;
            atom();
            if (failed_ || runs_.empty())
                return;
            auto& text = runs_.back().text;
            if (runs_.size() == before && text.size() <= text_before) {
                fail();
                return;
            }
            const auto trailing = text.endsWith(u' ');
            if (trailing)
                text.chop(1);
            if (text.endsWith(u'='))
                text.back() = QChar(u'≠');
            else if (text.endsWith(u'∈'))
                text.back() = QChar(u'∉');
            else
                text += QChar(0x0338);
            if (trailing)
                text += QLatin1Char(' ');
            return;
        }
        // \begin{..} environments and anything else with arguments show as
        // source; a lone unknown command shows as its name.
        skip_spaces();
        if (name == u"begin" || name == u"end" || name.size() == 1 ||
            (p_ < s_.size() && (s_[p_] == u'{' || s_[p_] == u'['))) {
            fail();
            return;
        }
        operand(name);
        pending_space_ = true; // read like a function name: "foo x"
    }

    const QString& s_;
    qsizetype p_{};
    std::vector<Run> runs_;
    Run::Shift shift_{Run::Shift::none};
    Last last_{Last::none};
    bool pending_space_{};
    bool bold_{};
    bool upright_{};
    bool failed_{};
    int depth_{};
    int fractions_{};
};

} // namespace

bool may_contain(const QString& markdown) {
    if (markdown.contains(u'$'))
        return true;
    // Single-character searches are vectorized; most prose has no backslash.
    for (auto at = markdown.indexOf(u'\\'); at >= 0 && at + 1 < markdown.size();
         at = markdown.indexOf(u'\\', at + 1))
        if (markdown[at + 1] == u'(' || markdown[at + 1] == u'[')
            return true;
    return false;
}

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::vector<Span> find(const QString& text) {
    std::vector<Span> out;
    if (!may_contain(text))
        return out;
    const qsizetype n = text.size();
    const auto add = [&](qsizetype start, qsizetype end, qsizetype open, qsizetype close,
                         bool display) {
        const auto source = text.mid(start + open, end - start - open - close);
        if (source.trimmed().isEmpty())
            return false;
        out.push_back({start, end - start, display, source, text.mid(start, end - start)});
        return true;
    };
    QString fence;
    bool line_start = true;
    qsizetype i = 0;
    while (i < n) {
        if (line_start) {
            line_start = false;
            // A fence the reply's pieces did not split out (inside a quote
            // or a list) still holds code.
            qsizetype j = i;
            while (j < n && (text[j] == u' ' || text[j] == u'\t' || text[j] == u'>'))
                ++j;
            const auto rest = QStringView(text).mid(j);
            if (fence.isEmpty() && (rest.startsWith(u"```") || rest.startsWith(u"~~~"))) {
                qsizetype length = 0;
                while (j + length < n && text[j + length] == text[j])
                    ++length;
                fence = text.mid(j, length);
                i = next_line(text, j);
                line_start = true;
                continue;
            }
            if (!fence.isEmpty()) {
                if (rest.startsWith(fence))
                    fence.clear();
                i = next_line(text, j);
                line_start = true;
                continue;
            }
        }
        const QChar c = text[i];
        if (c == u'\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (c == u'`') {
            qsizetype run = 1;
            while (i + run < n && text[i + run] == u'`')
                ++run;
            // A code span ends at the next run of exactly as many backticks.
            const QString ticks(run, u'`');
            qsizetype at = i + run;
            qsizetype end = -1;
            while ((at = text.indexOf(ticks, at)) >= 0) {
                qsizetype length = run;
                while (at + length < n && text[at + length] == u'`')
                    ++length;
                if (length == run) {
                    end = at + run;
                    break;
                }
                at += length;
            }
            i = end >= 0 ? end : i + run;
            continue;
        }
        if (c == u'\\' && i + 1 < n) {
            const QChar next = text[i + 1];
            if (next == u'(' || next == u'[') {
                const auto close = closing(text, i + 2, next == u'(' ? u"\\)" : u"\\]");
                if (close >= 0 && add(i, close + 2, 2, 2, next == u'[')) {
                    i = close + 2;
                    continue;
                }
            }
            i += 2; // an escaped character, \$ among them
            continue;
        }
        if (c == u'$') {
            if (i + 1 < n && text[i + 1] == u'$') {
                const auto close = closing(text, i + 2, u"$$");
                if (close >= 0 && add(i, close + 2, 2, 2, true)) {
                    i = close + 2;
                    continue;
                }
                i += 2;
                continue;
            }
            const auto close = single_dollar(text, i);
            if (close >= 0 && add(i, close + 1, 1, 1, false)) {
                i = close + 1;
                continue;
            }
            ++i;
            continue;
        }
        if (c == u']' && i + 1 < n && text[i + 1] == u'(') {
            // A link's destination, to its closing parenthesis on this line.
            int depth = 0;
            qsizetype j = i + 1;
            for (; j < n && text[j] != u'\n'; ++j) {
                if (text[j] == u'(')
                    ++depth;
                else if (text[j] == u')' && --depth == 0)
                    break;
            }
            i = j + 1;
            continue;
        }
        if (c == u'<' && i + 1 < n && ascii_letter(text[i + 1])) {
            // An autolink: <scheme:...>.
            qsizetype j = i + 1;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == u'+' || text[j] == u'.' ||
                             text[j] == u'-'))
                ++j;
            if (j < n && text[j] == u':') {
                const auto end = text.indexOf(u'>', j);
                const auto line_end = text.indexOf(u'\n', j);
                if (end >= 0 && (line_end < 0 || end < line_end)) {
                    i = end + 1;
                    continue;
                }
            }
        }
        ++i;
    }
    return out;
}

Rendered render(const QString& latex) { return Parser(latex).run(); }

QString plain(const Rendered& rendered) {
    QString out;
    auto shift = Run::Shift::none;
    for (const auto& run : rendered.runs) {
        if (run.shift != shift && shift != Run::Shift::none)
            out += QLatin1Char('}');
        if (run.shift != shift && run.shift != Run::Shift::none)
            out += run.shift == Run::Shift::super ? QStringLiteral("^{") : QStringLiteral("_{");
        shift = run.shift;
        out += run.text;
    }
    if (shift != Run::Shift::none)
        out += QLatin1Char('}');
    return out;
}

Prepared prepare(const QString& markdown) {
    Prepared out{markdown, {}};
    if (!may_contain(markdown))
        return out;
    // Text that already holds the placeholder characters is left alone.
    if (std::any_of(markdown.begin(), markdown.end(), [](QChar c) {
            return c.unicode() >= first_placeholder && c.unicode() <= last_placeholder;
        }))
        return out;
    auto spans = find(markdown);
    if (spans.empty())
        return out;
    if (spans.size() >
        static_cast<size_t>(last_placeholder) - static_cast<size_t>(first_placeholder) + 1)
        spans.resize(last_placeholder - first_placeholder + 1);
    QString text;
    text.reserve(markdown.size());
    qsizetype at = 0;
    for (size_t index = 0; index < spans.size(); ++index) {
        const auto& span = spans[index];
        text += QStringView(markdown).mid(at, span.start - at);
        text += QChar(static_cast<char16_t>(first_placeholder + index));
        at = span.start + span.length;
    }
    text += QStringView(markdown).mid(at);
    out.markdown = std::move(text);
    out.spans = std::move(spans);
    return out;
}

// Cohesive parser/test flow; branching stays local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void fill(QTextDocument& document, const std::vector<Span>& spans) {
    if (spans.empty())
        return;
    const auto last = static_cast<char16_t>(first_placeholder + spans.size() - 1);
    std::vector<std::pair<int, int>> found; // position, span
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto text = block.text();
        for (qsizetype i = 0; i < text.size(); ++i)
            if (text[i].unicode() >= first_placeholder && text[i].unicode() <= last)
                found.emplace_back(block.position() + static_cast<int>(i),
                                   text[i].unicode() - first_placeholder);
    }
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    const auto blank = [](QChar c) { return c == u' ' || c == QChar::LineSeparator; };
    // From the end, so each change leaves the positions before it alone.
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        int position = it->first;
        const auto& span = spans[static_cast<size_t>(it->second)];
        const auto rendered = render(span.source);
        cursor.setPosition(position);
        const bool own_paragraph = span.display && cursor.currentTable() == nullptr &&
                                   cursor.block().textList() == nullptr;
        if (own_paragraph) {
            // The equation becomes its own paragraph: break the block after
            // and before it, dropping the spaces its line breaks left.
            auto block = cursor.block();
            const int end = block.position() + block.length() - 1;
            int after = position + 1;
            while (after < end && blank(document.characterAt(after)))
                ++after;
            if (after > position + 1) {
                cursor.setPosition(position + 1);
                cursor.setPosition(after, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
            }
            if (position + 1 < block.position() + block.length() - 1) {
                cursor.setPosition(position + 1);
                cursor.insertBlock();
            }
            int before = position;
            while (before > block.position() && blank(document.characterAt(before - 1)))
                --before;
            if (before < position) {
                cursor.setPosition(before);
                cursor.setPosition(position, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
                position = before;
            }
            if (position > block.position()) {
                cursor.setPosition(position);
                cursor.insertBlock();
                ++position;
            }
            cursor.setPosition(position);
            auto format = cursor.blockFormat();
            format.setAlignment(rendered.ok ? Qt::AlignHCenter : Qt::AlignLeft);
            format.setProperty(display_property, true);
            cursor.setBlockFormat(format);
        }
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        auto base = cursor.charFormat();
        base.setProperty(span_property, it->second);
        base.setProperty(source_property, span.written);
        cursor.removeSelectedText();
        if (!rendered.ok) {
            // As written, in the code font the view styles as inline code.
            auto format = base;
            format.setFontFixedPitch(true);
            auto source = span.source.trimmed();
            source.replace(u'\n', span.display && own_paragraph ? QChar(QChar::LineSeparator)
                                                                : QChar(u' '));
            cursor.insertText(source, format);
            continue;
        }
        for (const auto& run : rendered.runs) {
            auto format = base;
            if (run.italic)
                format.setFontItalic(true);
            if (run.bold)
                format.setFontWeight(QFont::Bold);
            if (run.shift == Run::Shift::super)
                format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
            else if (run.shift == Run::Shift::sub)
                format.setVerticalAlignment(QTextCharFormat::AlignSubScript);
            cursor.insertText(run.text, format);
        }
    }
    cursor.endEditBlock();
}

} // namespace lapis::desktop::transcript::math
