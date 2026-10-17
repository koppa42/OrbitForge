#pragma once
#include <QColor>
#include <QMargins>
#include <QString>
#include <string_view>

// Workbench colour tokens: indigo blue-grey surfaces, a periwinkle "star blue" interaction
// accent taken from the startup artwork, and a restrained sakura highlight. Stylesheets
// reference tokens as `@name` or `@name/NN` (NN = opacity in percent); painted widgets use
// the same constants so chrome, charts and overlays stay consistent.
//
// Legibility: text, textSoft and textMuted keep at least 4.5:1 contrast on every surface;
// textFaint is reserved for disabled content. White text on accentDeep is about 4.9:1.
namespace orbitforge::desktop::theme {
struct Token {
    std::string_view name;
    std::string_view value;
};

// Surfaces, darkest to lightest: the canvas shows between floating panels.
inline constexpr Token canvas{"canvas", "#0b0f1a"};
inline constexpr Token chrome{"chrome", "#121829"};
inline constexpr Token panel{"panel", "#141b2d"};
inline constexpr Token surface{"surface", "#171f33"};
inline constexpr Token raised{"raised", "#1e2740"};
inline constexpr Token overlay{"overlay", "#26304c"};
inline constexpr Token field{"field", "#0f1424"};
inline constexpr Token field_muted{"fieldMuted", "#171e31"};
// Lines and borders.
inline constexpr Token line{"line", "#232c45"};
inline constexpr Token border{"border", "#2f3a58"};
inline constexpr Token border_strong{"borderStrong", "#46537a"};
// Text.
inline constexpr Token text{"text", "#e9edf9"};
inline constexpr Token text_soft{"textSoft", "#c4cbe2"};
inline constexpr Token text_muted{"textMuted", "#909bbd"};
inline constexpr Token text_faint{"textFaint", "#636e90"};
// Star-blue interaction accent.
inline constexpr Token accent{"accent", "#93acff"};
inline constexpr Token accent_bright{"accentBright", "#c6d3ff"};
inline constexpr Token accent_deep{"accentDeep", "#5269cc"};
inline constexpr Token accent_wash{"accentWash", "#28325c"};
inline constexpr Token accent_hover{"accentHover", "#1f2742"};
inline constexpr Token on_accent{"onAccent", "#ffffff"};
// Sakura highlight for brand moments, the object's own interval and impulses.
inline constexpr Token sakura{"sakura", "#f4b8d4"};
inline constexpr Token sakura_deep{"sakuraDeep", "#de8cb5"};
inline constexpr Token sakura_wash{"sakuraWash", "#3a2844"};
// Status.
inline constexpr Token success{"success", "#8edcc6"};
inline constexpr Token success_wash{"successWash", "#173434"};
inline constexpr Token warning{"warning", "#ecc98f"};
inline constexpr Token warning_wash{"warningWash", "#372f24"};
inline constexpr Token danger{"danger", "#f2a5a3"};
inline constexpr Token danger_wash{"dangerWash", "#3b2430"};

inline constexpr Token all[] = {
    canvas,      chrome,       panel,      surface,      raised,        overlay,
    field,       field_muted,  line,       border,       border_strong, text,
    text_soft,   text_muted,   text_faint, accent,       accent_bright, accent_deep,
    accent_wash, accent_hover, on_accent,  sakura,       sakura_deep,   sakura_wash,
    success,     success_wash, warning,    warning_wash, danger,        danger_wash};

inline QColor color(const Token &token, int alpha = 255) {
    QColor result(QLatin1StringView(token.value.data(), qsizetype(token.value.size())));
    result.setAlpha(alpha);
    return result;
}

// Spacing scale on a 4 px grid (2, 6 and 10 for fine adjustment), shared by every window so
// pages read with one rhythm. Logical pixels.
namespace space {
inline constexpr int xs = 4;                  // icon to text, tight clusters
inline constexpr int sm = 8;                  // controls within one row
inline constexpr int row = 10;                // between form rows
inline constexpr int md = 12;                 // label to field; card padding
inline constexpr int lg = 16;                 // between groups and major blocks
inline constexpr int xl = 24;                 // page side margins
inline const QMargins page{24, 20, 24, 20};   // dialogs and document pages
inline const QMargins footer{24, 12, 24, 20}; // action row closing a page
inline const QMargins panel{16, 16, 16, 16};  // side panels and settings columns
inline const QMargins card{12, 10, 12, 10};   // cards, callouts and popups
} // namespace space

// Replaces `@token` and `@token/NN` references; throws std::invalid_argument for unknown
// names or opacities above 100 so a typo fails loudly instead of silently dropping a rule.
QString resolve(QString stylesheet);
} // namespace orbitforge::desktop::theme
