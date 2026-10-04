#pragma once

#include "imgui.h"

#include <initializer_list>
#include <string>

// The look of the interface and the controls its screens are built from.
// Everything is sized from the current text size, so it scales with the
// window, and every control works with a gamepad, a keyboard and a mouse.
namespace tutikumo::ui {

// Tutikumo's look, "ink and vermilion": sumi black panels, washi-white text,
// a vermilion accent and gold leaf for what is lit. Edges are straight; the
// diamond of the emblem marks titles, sections and slider knobs.
namespace colors {
inline constexpr ImU32 kBackdrop = IM_COL32(8, 7, 5, 175);
inline constexpr ImU32 kPanel = IM_COL32(22, 20, 15, 248);
inline constexpr ImU32 kRail = IM_COL32(15, 13, 10, 255);
inline constexpr ImU32 kPanelEdge = IM_COL32(58, 51, 38, 255);
inline constexpr ImU32 kRow = IM_COL32(236, 228, 212, 8);
inline constexpr ImU32 kRowHover = IM_COL32(236, 228, 212, 18);
inline constexpr ImU32 kRowFocus = IM_COL32(184, 64, 44, 52);
inline constexpr ImU32 kAccent = IM_COL32(184, 64, 44, 255);
inline constexpr ImU32 kAccentBright = IM_COL32(226, 178, 104, 255);
inline constexpr ImU32 kText = IM_COL32(236, 228, 212, 255);
inline constexpr ImU32 kTextDim = IM_COL32(140, 130, 112, 255);
inline constexpr ImU32 kTextDisabled = IM_COL32(88, 82, 70, 255);
inline constexpr ImU32 kDanger = IM_COL32(236, 96, 80, 255);
inline constexpr ImU32 kGood = IM_COL32(150, 184, 112, 255);
inline constexpr ImU32 kTrack = IM_COL32(236, 228, 212, 32);
} // namespace colors

// The emblem's diamond, `size` across, centred on `center`: filled or drawn
// as an outline.
void draw_diamond(ImDrawList *draw, ImVec2 center, float size, ImU32 color, bool filled, float thickness = 1.5f);

ImGuiStyle make_style(float scale, float font_size);

// The panel every screen sits in, centred in the window: a rail down its
// left with the emblem, the name and `status`, and beside it the page: its
// title, then fixed parts, then the scrolling content from begin_content(),
// then a footer with the focused row's description and the button hints:
//
//   begin_panel(...); begin_content(); rows...;
//   begin_footer(); hints(...); end_panel();
void begin_panel(const char *id, const std::string &title, const std::string &status, bool dim_game);
// The same with a list of steps in the rail, the player at `step` (the
// steps before it are drawn as done).
void begin_panel(const char *id, const std::string &title, const std::string &status, const char *const *steps,
                 int count, int step, bool dim_game);
// The same with the menu's pages in the rail instead, switched by a click
// or with L1/R1 (Q/W on the keyboard), the open page's name as the title.
// Returns true when `selected` changed.
bool begin_panel_nav(const char *id, const std::string &status, const char *const *labels, int count, int &selected,
                     bool dim_game);
void begin_content();
// A drag with a finger scrolls the current window, as on any touch screen;
// ImGui itself scrolls only with a wheel. Call before ending the window.
void touch_scroll();
void begin_footer();
void end_panel();

// Tabs switched with L1/R1 (Q/W on the keyboard) or the mouse. Returns true
// when `selected` changed.
bool tab_bar(const char *const *labels, int count, int &selected);

struct RowOptions {
    bool disabled{};
    std::string note;         // shown dimmed next to the value, e.g. who decides it
    std::string description;  // shown in the footer while the row is focused
    bool warning{};           // the value clashes with something: drawn in the danger colour
};

// A setting with a few values, changed with left/right or by activating it.
// Returns -1, 0 or +1.
int choice_row(const char *label, const std::string &value, const RowOptions &options = {});
// An on/off setting. Returns true when toggled.
bool toggle_row(const char *label, bool value, const RowOptions &options = {});
// A number between minimum and maximum, changed in steps with left/right or
// dragged with the mouse. Returns true when `value` changed.
bool slider_row(const char *label, int &value, int minimum, int maximum, int step, const char *format,
                const RowOptions &options = {});
// An action. Returns true when activated.
bool button_row(const char *label, const RowOptions &options = {}, ImU32 color = colors::kText);
// A setting chosen on a screen of its own: its value, and a chevron that
// opens that screen. Returns true when activated.
bool value_row(const char *label, const std::string &value, const RowOptions &options = {});
// An entry of a list such as the file browser's: an icon, a name and a
// detail on the right. `id` keeps rows with equal names apart.
enum class ListIcon { None, Folder, ParentFolder, File, Disc, Drive };
bool list_row(const char *id, const std::string &name, const std::string &detail, ListIcon icon,
              bool highlight = false);
// A line of information, focusable so a gamepad can scroll to it.
void info_row(const char *label, const std::string &value);
void section(const char *title);

// The interface's language: the system's, Japanese or English. Applies at
// once and is saved.
void language_row();

// Focuses the next row, e.g. the first row after switching tabs.
void focus_next_row();

// A wide button for the setup screens. Returns true when activated.
bool big_button(const char *label, float width, bool primary = false, bool disabled = false);
void progress_bar(float fraction, const std::string &overlay);

// Text wrapped to the available width.
void paragraph(const std::string &text, ImU32 color = colors::kText);
void heading(const std::string &text);

// Button hints for the footer, drawn with the glyphs of the pad in use or the
// keys of the keyboard. Toggle, Shift, Space and Symbols exist on the pad only
// and are skipped for the keyboard.
enum class Control {
    Confirm,
    Back,
    Tabs,
    Change,
    Menu,
    Start,
    Toggle,   // the top face button
    Delete,   // the back face button; Backspace
    Shift,    // the left face button
    Space,    // the top face button
    Symbols,  // Select, Share, Create or View, as the pad names it
    Cursor,   // the shoulder buttons; the arrow keys
    Clear,    // the top face button; Delete
    Reset,    // Select, Share, Create or View, as Symbols
};
struct Hint {
    Control control;
    const char *text;
};
void hints(std::initializer_list<Hint> list);

} // namespace tutikumo::ui
