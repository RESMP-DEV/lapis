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
    // one. Category names are written in these, so each keeps text contrast
    // (4.5:1) with its themes' surfaces, as does its quieter mix with the
    // theme's muted text (kCategoryQuietMix).
    std::string_view dark;
    std::string_view light;
};

// Swatch order, as the category menu lists them.
inline constexpr std::array<CategoryColor, 8> kCategoryColors{{
    {"grey", "#9ba5b2", "#5f6774"},
    {"blue", "#6fa6ff", "#2360c4"},
    {"red", "#f2777e", "#bb2f39"},
    {"yellow", "#e6c36a", "#875a00"},
    {"green", "#71cf8a", "#1f7340"},
    {"pink", "#f29ac6", "#b03374"},
    {"purple", "#b69cff", "#7347c9"},
    {"cyan", "#56d0e0", "#056f82"},
}};

// A category other than the shown one writes its name in its colour mixed
// half and half with the theme's muted text: quieter, still its hue.
inline constexpr double kCategoryQuietMix = 0.5;

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
