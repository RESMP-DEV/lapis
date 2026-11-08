#ifndef LAPIS_DESKTOP_CODE_HIGHLIGHT_HPP
#define LAPIS_DESKTOP_CODE_HIGHLIGHT_HPP
#include <QColor>
#include <QString>
#include <QSyntaxHighlighter>
#include <vector>

class QTextDocument;

namespace lapis::desktop::code {
// What a stretch of code is, which picks its color.
enum class Role {
    plain,
    comment,
    keyword,
    string,
    number,
    function,
    type,
    op,
    constant,
    property,
    parameter, // shell flags, Python decorators' targets
    builtin,   // self, this
    tag,       // markup tags
    meta,      // preprocessor lines, decorators, lifetimes
    added,
    removed,
    heading,
};
struct Span {
    int start{};
    int length{};
    Role role{Role::plain};
};

// A fence's language as lapis reads it: a family of rules and its words.
struct Language;
// The language for a fence name ("cpp", "py", "bash", ...); an unknown or
// empty name gets generic rules: strings, numbers, # and // comments.
[[nodiscard]] const Language& language(const QString& name);

// The colored stretches of one line. `state` carries what a line leaves
// open (a block comment, a triple-quoted string) to the next; start at 0.
[[nodiscard]] std::vector<Span> tokenize(const QString& line, const Language& language, int& state);

// Tokyo Night's colors as the Neovim theme uses them; `light` is its Day
// variant.
[[nodiscard]] QColor color(Role role, bool light);

// Colors a document's code with the rules above, line by line as Qt asks.
class Highlighter final : public QSyntaxHighlighter {
    Q_OBJECT
  public:
    Highlighter(QTextDocument* document, const QString& language, bool light);

  protected:
    void highlightBlock(const QString& text) override;

  private:
    const Language& language_;
    bool light_;
};
} // namespace lapis::desktop::code
#endif
