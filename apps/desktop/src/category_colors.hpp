#ifndef LAPIS_DESKTOP_CATEGORY_COLORS_HPP
#define LAPIS_DESKTOP_CATEGORY_COLORS_HPP

#include <array>
#include <cstddef>
#include <string_view>

namespace lapis::desktop {
// The fixed palette categories are colour-coded with in the sidebar, as
// Chrome's tab groups are. The registry stores a hue by name; the window
// resolves it for the theme, so a name means the same hue on every theme.
struct CategoryColor {
    std::string_view name;
    // On a dark surface (every built-in theme but Daylight) and on a light
    // one. Each keeps at least 4:1 contrast with its themes' surfaces.
    std::string_view dark;
    std::string_view light;
};

// Swatch order, as the category menu lists them.
inline constexpr std::array<CategoryColor, 8> kCategoryColors{{
    {"grey", "#9ba5b2", "#6a7280"},
    {"blue", "#6fa6ff", "#2563c9"},
    {"red", "#f2777e", "#c4323c"},
    {"yellow", "#e6c36a", "#946200"},
    {"green", "#71cf8a", "#237a44"},
    {"pink", "#f29ac6", "#bd3a7c"},
    {"purple", "#b69cff", "#7347c9"},
    {"cyan", "#56d0e0", "#06768a"},
}};

// A category without a chosen colour takes one by its position: neighbours
// differ in hue, red (close to the fault colour) comes late and grey last.
inline constexpr std::array<std::size_t, 8> kCategoryColorCycle{1, 4, 6, 3, 7, 5, 2, 0};

[[nodiscard]] constexpr std::string_view category_color_for_position(std::size_t position) {
    return kCategoryColors.at(kCategoryColorCycle.at(position % kCategoryColorCycle.size())).name;
}

[[nodiscard]] constexpr bool category_color_known(std::string_view name) {
    for (const auto& color : kCategoryColors)
        if (color.name == name)
            return true;
    return false;
}
} // namespace lapis::desktop

#endif
