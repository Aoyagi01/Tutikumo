#include "ui/widgets.hpp"

#include "ui/i18n.hpp"
#include "ui/layer.hpp"

#include "gpu/vulkan_renderer.hpp"
#include "settings/settings.hpp"

#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tutikumo::ui {
namespace {

bool focus_next = false;

// Moves the gamepad and keyboard focus to the item just submitted when
// focus_next_row() asked for it. SetKeyboardFocusHere() would not do: it
// tabs to the next text field and starts editing it.
void take_focus() {
    if (!focus_next) return;
    focus_next = false;
    ImGui::FocusItem();
    ImGui::SetScrollHereY(0.3f);
}

float font() { return Layer::get().font_size(); }

// Japanese breaks between any two characters, but ImGui wraps only at blanks
// and punctuation, which leaves its lines ragged. In Japanese, lines are
// broken here instead: a run of Latin letters or digits stays whole, a blank
// may end a line, and closing punctuation never starts one.
bool closing(char32_t c) {
    switch (c) {
    case U'、': case U'。': case U'，': case U'．': case U'）': case U'」': case U'』': case U'】': case U'・':
    case U'ー': case U'？': case U'！': case U'：': case U'ぁ': case U'ぃ': case U'ぅ': case U'ぇ': case U'ぉ':
    case U'っ': case U'ゃ': case U'ゅ': case U'ょ': case U'ァ': case U'ィ': case U'ゥ': case U'ェ': case U'ォ':
    case U'ッ': case U'ャ': case U'ュ': case U'ョ': case U')': case U'.': case U',': case U':': case U';':
        return true;
    default: return false;
    }
}

std::string break_lines(const std::string &text, float size, float width) {
    if (language() != Language::Japanese || width <= 0.0f) return text;
    // A little short of the width, so ImGui's own wrapping never adds a line.
    width -= 2.0f;
    ImFont *f = ImGui::GetFont();
    std::string out;
    out.reserve(text.size() + 16u);
    float line = 0.0f;
    const char *p = text.c_str();
    const char *end = p + text.size();
    while (p < end) {
        if (*p == '\n') {
            out += '\n';
            line = 0.0f;
            ++p;
            continue;
        }
        // The next unit: a blank, a run of printable ASCII, or one character.
        const char *q = p;
        unsigned int first = 0;
        if (*p == ' ') {
            ++q;
        } else if (static_cast<unsigned char>(*p) < 0x80u) {
            while (q < end && *q != ' ' && *q != '\n' && static_cast<unsigned char>(*q) < 0x80u) ++q;
        } else {
            q += ImTextCharFromUtf8(&first, p, end);
            // An opening bracket never ends a line: it goes with what follows.
            if ((first == U'（' || first == U'「' || first == U'『' || first == U'【') && q < end && *q != '\n' &&
                *q != ' ') {
                if (static_cast<unsigned char>(*q) < 0x80u) {
                    while (q < end && *q != ' ' && *q != '\n' && static_cast<unsigned char>(*q) < 0x80u) ++q;
                } else {
                    unsigned int next = 0;
                    q += ImTextCharFromUtf8(&next, q, end);
                }
            }
        }
        const float unit = f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, q).x;
        if (*p == ' ') {
            // A blank at the start of a line is dropped.
            if (line > 0.0f) {
                out += ' ';
                line += unit;
            }
        } else if (line > 0.0f && line + unit > width && !closing(static_cast<char32_t>(first)) &&
                   !(q - p == 1 && closing(static_cast<char32_t>(*p)))) {
            // A blank that ended the line is left out.
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += '\n';
            out.append(p, q);
            line = unit;
        } else {
            out.append(p, q);
            line += unit;
        }
        p = q;
    }
    return out;
}

// The page beside the navigation rail is a child window inside the panel:
// end_panel() closes it first.
bool page_open = false;

float px(float value) { return std::round(value * Layer::get().scale()); }

// Left or right on the keyboard, the D-pad or the left stick, with repeat.
int horizontal_press() {
    int delta = 0;
    for (ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft})
        if (ImGui::IsKeyPressed(key, true)) delta = -1;
    for (ImGuiKey key : {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight})
        if (ImGui::IsKeyPressed(key, true)) delta = 1;
    return delta;
}

void draw_triangle(ImDrawList *draw, ImVec2 center, float size, bool right, ImU32 color) {
    const float h = size * 0.5f;
    if (right)
        draw->AddTriangleFilled({center.x - h * 0.8f, center.y - h}, {center.x - h * 0.8f, center.y + h},
                                {center.x + h * 0.8f, center.y}, color);
    else
        draw->AddTriangleFilled({center.x + h * 0.8f, center.y - h}, {center.x + h * 0.8f, center.y + h},
                                {center.x - h * 0.8f, center.y}, color);
}

struct Row {
    ImVec2 min;
    ImVec2 max;
    bool pressed{};
    bool focused{};
    bool hovered{};
};

// The part every row shares: a full-width focusable strip with its label.
Row row(const char *label, const RowOptions &options, ImU32 color = colors::kText, float height_lines = 1.0f) {
    const float height = std::round(font() * (0.9f + height_lines));
    Row result;
    result.min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    result.max = {result.min.x + width, result.min.y + height};
    result.pressed = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
    take_focus();
    result.focused = ImGui::IsItemFocused();
    result.hovered = ImGui::IsItemHovered();

    ImDrawList *draw = ImGui::GetWindowDrawList();
    // The focused row: a vermilion wash with a vermilion stroke on its left.
    if (result.focused) {
        draw->AddRectFilled(result.min, result.max, colors::kRowFocus);
        draw->AddRectFilled(result.min, {result.min.x + px(3.0f), result.max.y}, colors::kAccent);
    } else if (result.hovered) {
        draw->AddRectFilled(result.min, result.max, colors::kRowHover);
    }
    const float text_y = result.min.y + (height - font()) * 0.5f;
    const std::string shown = tr_label(label);
    draw->AddText({result.min.x + px(16.0f), text_y}, options.disabled ? colors::kTextDisabled : color,
                  shown.c_str());
    if (result.focused || (result.hovered && Layer::get().description().empty())) {
        std::string description = options.description;
        if (!options.note.empty()) description += (description.empty() ? "" : "\n") + options.note;
        Layer::get().set_description(description);
    }
    return result;
}

// Draws `value` right-aligned in the row, with the note to its left.
float draw_value(const Row &r, const std::string &english, const RowOptions &options, float right_inset,
                 ImU32 color) {
    const std::string value = tr(english);
    const std::string note = tr(options.note);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float text_y = r.min.y + (r.max.y - r.min.y - font()) * 0.5f;
    const float value_width = ImGui::CalcTextSize(value.c_str()).x;
    const float x = r.max.x - right_inset - value_width;
    draw->AddText({x, text_y}, options.disabled ? colors::kTextDisabled : color, value.c_str());
    if (!options.note.empty() && options.disabled) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        const float note_width = ImGui::CalcTextSize(note.c_str()).x;
        draw->AddText({x - px(18.0f) - note_width, text_y + font() * 0.15f}, colors::kTextDim, note.c_str());
        ImGui::PopFont();
    }
    return x;
}

// A rounded label such as a shoulder button or a key.
float draw_cap(ImDrawList *draw, ImVec2 at, const char *text, bool filled) {
    const float height = font() * 1.15f;
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float width = std::max(height, text_size.x + font() * 0.7f);
    const ImVec2 min{at.x, at.y + (font() - height) * 0.5f};
    const ImVec2 max{min.x + width, min.y + height};
    if (filled) draw->AddRectFilled(min, max, colors::kAccent, px(2.0f));
    else draw->AddRect(min, max, colors::kTextDim, px(2.0f), 0, px(1.0f));
    draw->AddText({min.x + (width - text_size.x) * 0.5f, min.y + (height - text_size.y) * 0.5f}, colors::kText, text);
    return width;
}

// A face button as the pad in use labels it: a PlayStation shape or a letter.
float draw_face(ImDrawList *draw, ImVec2 at, SDL_GamepadButton button) {
    const float radius = font() * 0.62f;
    const ImVec2 center{at.x + radius, at.y + font() * 0.5f};
    draw->AddCircleFilled(center, radius, colors::kAccent);
    SDL_Gamepad *pad = Layer::get().attached() ? Layer::get().renderer().gamepad() : nullptr;
    SDL_GamepadButtonLabel label = pad != nullptr ? SDL_GetGamepadButtonLabel(pad, button)
                                                  : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    if (label == SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) {
        // Positional names of the common Xbox-style layout, as on a Steam Deck.
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: label = SDL_GAMEPAD_BUTTON_LABEL_A; break;
        case SDL_GAMEPAD_BUTTON_EAST: label = SDL_GAMEPAD_BUTTON_LABEL_B; break;
        case SDL_GAMEPAD_BUTTON_WEST: label = SDL_GAMEPAD_BUTTON_LABEL_X; break;
        default: label = SDL_GAMEPAD_BUTTON_LABEL_Y; break;
        }
    }
    const ImU32 ink = colors::kPanel;
    const float s = radius * 0.45f;
    const float thickness = std::max(1.5f, radius * 0.16f);
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        draw->AddLine({center.x - s, center.y - s}, {center.x + s, center.y + s}, ink, thickness);
        draw->AddLine({center.x - s, center.y + s}, {center.x + s, center.y - s}, ink, thickness);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: draw->AddCircle(center, s * 1.1f, ink, 0, thickness); break;
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        draw->AddRect({center.x - s, center.y - s}, {center.x + s, center.y + s}, ink, 0.0f, 0, thickness);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        draw->AddTriangle({center.x, center.y - s * 1.1f}, {center.x + s * 1.1f, center.y + s * 0.8f},
                          {center.x - s * 1.1f, center.y + s * 0.8f}, ink, thickness);
        break;
    default: {
        const char *letter = label == SDL_GAMEPAD_BUTTON_LABEL_A   ? "A"
                             : label == SDL_GAMEPAD_BUTTON_LABEL_B ? "B"
                             : label == SDL_GAMEPAD_BUTTON_LABEL_X ? "X"
                                                                   : "Y";
        const ImVec2 size = ImGui::CalcTextSize(letter);
        draw->AddText({center.x - size.x * 0.5f, center.y - size.y * 0.5f}, ink, letter);
        break;
    }
    }
    return radius * 2.0f;
}

const char *shoulder_name(bool right) {
    SDL_Gamepad *pad = Layer::get().attached() ? Layer::get().renderer().gamepad() : nullptr;
    const SDL_GamepadType type = pad != nullptr ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const char *name = pad != nullptr ? SDL_GetGamepadName(pad) : nullptr;
    const bool playstation = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
                             type == SDL_GAMEPAD_TYPE_PS5;
    // The Steam Deck prints L1 and R1 on its bumpers.
    const bool deck = name != nullptr && std::strstr(name, "Steam Deck") != nullptr;
    if (playstation || deck) return right ? "R1" : "L1";
    if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)
        return right ? "R" : "L";
    return right ? "RB" : "LB";
}

// The button left of the pad's centre, as the pad in use names it.
const char *select_name() {
    SDL_Gamepad *pad = Layer::get().attached() ? Layer::get().renderer().gamepad() : nullptr;
    switch (pad != nullptr ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN) {
    case SDL_GAMEPAD_TYPE_PS3: return "Select";
    case SDL_GAMEPAD_TYPE_PS4: return "Share";
    case SDL_GAMEPAD_TYPE_PS5: return "Create";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "−";
    default: return "View";
    }
}

} // namespace

void draw_diamond(ImDrawList *draw, ImVec2 center, float size, ImU32 color, bool filled, float thickness) {
    const float h = size * 0.5f;
    const ImVec2 top{center.x, center.y - h}, right{center.x + h, center.y}, bottom{center.x, center.y + h},
        left{center.x - h, center.y};
    if (filled) draw->AddQuadFilled(top, right, bottom, left, color);
    else draw->AddQuad(top, right, bottom, left, color, thickness);
}

ImGuiStyle make_style(float scale, float font_size) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = {28.0f, 22.0f};
    style.FramePadding = {12.0f, 7.0f};
    style.ItemSpacing = {10.0f, 3.0f};
    style.ScrollbarSize = 6.0f;
    style.ScrollbarRounding = 0.0f;
    style.ScaleAllSizes(scale);
    style.FontSizeBase = font_size;

    ImVec4 *c = style.Colors;
    const auto rgba = [](ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); };
    c[ImGuiCol_Text] = rgba(colors::kText);
    c[ImGuiCol_TextDisabled] = rgba(colors::kTextDisabled);
    c[ImGuiCol_WindowBg] = rgba(colors::kPanel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgba(IM_COL32(28, 25, 19, 252));
    c[ImGuiCol_ModalWindowDimBg] = rgba(IM_COL32(6, 5, 4, 160));
    c[ImGuiCol_Border] = rgba(colors::kPanelEdge);
    c[ImGuiCol_FrameBg] = rgba(IM_COL32(236, 228, 212, 16));
    c[ImGuiCol_FrameBgHovered] = rgba(colors::kRowHover);
    c[ImGuiCol_FrameBgActive] = rgba(colors::kRowFocus);
    c[ImGuiCol_TextSelectedBg] = rgba(IM_COL32(184, 64, 44, 120));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgba(IM_COL32(88, 78, 60, 200));
    c[ImGuiCol_ScrollbarGrabHovered] = rgba(IM_COL32(120, 106, 82, 220));
    c[ImGuiCol_ScrollbarGrabActive] = rgba(colors::kAccent);
    c[ImGuiCol_Separator] = rgba(colors::kPanelEdge);
    // Rows draw their own focus; ImGui's rectangle would double it.
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    return style;
}

namespace {

// What the rail lists below the name: the menu's pages, which switch, or the
// setup's steps, which only show where the player is.
enum class RailItems { Pages, Steps };

// The panel every screen sits in, centred in the window: the rail down its
// left (the emblem and the name, `status`, then `items`), and a child window
// beside it for the page, headed by `page_title`. end_panel() closes both.
void railed_panel(const char *id, const std::string &status, const std::string &page_title,
                  const char *const *items, int count, int *selected, RailItems kind, bool dim_game) {
    const ImGuiIO &io = ImGui::GetIO();
    if (dim_game) ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, io.DisplaySize, colors::kBackdrop);
    const float margin = std::round(std::min(io.DisplaySize.x, io.DisplaySize.y) * 0.03f);
    const ImVec2 size{std::min(io.DisplaySize.x - 2.0f * margin, font() * 56.0f),
                      std::min(io.DisplaySize.y - 2.0f * margin, font() * 33.0f)};
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const ImVec2 padding = ImGui::GetStyle().WindowPadding;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float rail = std::round(std::min(font() * 11.0f, size.x * 0.3f));
    draw->AddRectFilled(origin, {origin.x + rail, origin.y + size.y}, colors::kRail);
    draw->AddLine({origin.x + rail, origin.y}, {origin.x + rail, origin.y + size.y}, colors::kPanelEdge, px(1.0f));

    const float inset = px(18.0f);
    float y = origin.y + padding.y;
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        const float title_size = ImGui::GetFontSize();
        const float emblem = title_size * 0.62f;
        const ImVec2 center{origin.x + inset + emblem * 0.5f, y + title_size * 0.55f};
        draw_diamond(draw, center, emblem, colors::kAccent, false, px(2.0f));
        draw_diamond(draw, center, emblem * 0.36f, colors::kAccent, true);
        draw->AddText({origin.x + inset + emblem + px(10.0f), y}, colors::kText, "Tutikumo");
        ImGui::PopFont();
        y += title_size + px(6.0f);
    }
    if (!status.empty()) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.82f);
        const std::string shown = tr(status);
        const float small = ImGui::GetFontSize();
        draw->AddCircleFilled({origin.x + inset + small * 0.35f, y + small * 0.55f}, small * 0.22f,
                              colors::kAccentBright);
        draw->AddText({origin.x + inset + small * 0.9f, y}, colors::kTextDim, shown.c_str());
        ImGui::PopFont();
        y += font() * 1.1f;
    }
    y += px(14.0f);
    draw->AddLine({origin.x + inset, y}, {origin.x + rail - inset, y}, colors::kPanelEdge, px(1.0f));
    y += px(10.0f);

    const bool pages = kind == RailItems::Pages;
    const int current = selected != nullptr ? *selected : -1;
    const float item_height = std::round(font() * 2.1f);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < count; ++i) {
        const ImVec2 min{origin.x, y};
        const ImVec2 max{origin.x + rail, y + item_height};
        bool hovered = false;
        if (pages) {
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("page", {rail, item_height}) && selected != nullptr) *selected = i;
            hovered = ImGui::IsItemHovered();
            ImGui::PopID();
        }
        const bool active = i == current;
        const bool done = !pages && i < current;
        if (active) {
            draw->AddRectFilled(min, max, colors::kRowFocus);
            draw->AddRectFilled(min, {min.x + px(4.0f), max.y}, colors::kAccent);
        } else if (hovered) {
            draw->AddRectFilled(min, max, colors::kRowHover);
        }
        const float mid = (min.y + max.y) * 0.5f;
        // A page's marker; a step's is filled once it is behind the player.
        draw_diamond(draw, {min.x + inset + font() * 0.3f, mid}, font() * 0.5f,
                     active ? colors::kAccentBright : done ? colors::kAccent : colors::kTextDisabled,
                     active || done);
        const char *label = tr(items[i]);
        draw->AddText({min.x + inset + font() * 1.05f, mid - font() * 0.5f},
                      active ? colors::kText : done ? colors::kTextDim : pages ? colors::kTextDim : colors::kTextDisabled,
                      label);
        y += item_height + px(2.0f);
    }
    ImGui::PopItemFlag();

    // The buttons that step through the pages.
    if (pages) {
        const bool pad = Layer::get().input_device() == InputDevice::Gamepad;
        const float foot = origin.y + size.y - padding.y - font();
        float x = origin.x + inset;
        x += draw_cap(draw, {x, foot}, pad ? shoulder_name(false) : "Q", false) + px(6.0f);
        x += draw_cap(draw, {x, foot}, pad ? shoulder_name(true) : "W", false) + px(8.0f);
        draw->AddText({x, foot}, colors::kTextDim, tr("Switch page"));
    }

    // The page: its name over a hairline, then what begin_content() holds.
    ImGui::SetCursorScreenPos({origin.x + rail + px(1.0f), origin.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::BeginChild("##page", {size.x - rail - px(1.0f), size.y},
                      ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    page_open = true;
    {
        ImDrawList *page = ImGui::GetWindowDrawList();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
        ImGui::TextUnformatted(tr(page_title).c_str());
        ImGui::PopFont();
        ImGui::Dummy({0.0f, px(2.0f)});
        const ImVec2 line = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        page->AddLine({line.x, line.y}, {line.x + width, line.y}, colors::kPanelEdge, px(1.0f));
        page->AddRectFilled({line.x, line.y - px(1.0f)}, {line.x + font() * 2.4f, line.y + px(1.5f)}, colors::kAccent);
        ImGui::Dummy({0.0f, px(8.0f)});
    }
}

} // namespace

void begin_panel(const char *id, const std::string &title, const std::string &status, bool dim_game) {
    railed_panel(id, status, title, nullptr, 0, nullptr, RailItems::Steps, dim_game);
}

void begin_panel(const char *id, const std::string &title, const std::string &status, const char *const *steps,
                 int count, int step, bool dim_game) {
    railed_panel(id, status, title, steps, count, &step, RailItems::Steps, dim_game);
}

bool begin_panel_nav(const char *id, const std::string &status, const char *const *labels, int count, int &selected,
                     bool dim_game) {
    const int before = selected;
    const bool typing = ImGui::GetIO().WantTextInput;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_Q, false)))
        selected = (selected + count - 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_W, false)))
        selected = (selected + 1) % count;
    // A click on the rail changes `selected` while it is drawn; the page
    // drawn below is then already the new one.
    const int shown = selected;
    railed_panel(id, status, labels[shown], labels, count, &selected, RailItems::Pages, dim_game);
    return selected != before;
}

void begin_content() {
    const float footer = std::round(font() * 4.3f);
    const float content = std::max(font() * 3.0f, ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("content", {0.0f, content}, ImGuiChildFlags_NavFlattened);
}

void touch_scroll() {
    ImGuiIO &io = ImGui::GetIO();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen) return;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        return;
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, font() * 0.4f)) return;
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    // A drag is a scroll, not a press: the row the finger started on must not
    // act when it lifts.
    ImGui::ClearActiveID();
}

void begin_footer() {
    touch_scroll();
    ImGui::EndChild();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImGui::Dummy({0.0f, px(6.0f)});
    // The description in a box of two lines, whatever it holds, so the hints
    // stay put.
    const float box_padding = px(8.0f);
    const ImVec2 box = ImGui::GetCursorScreenPos();
    const float outer_width = ImGui::GetContentRegionAvail().x;
    const float description_height = font() * 2.4f;
    const ImVec2 box_max{box.x + outer_width, box.y + description_height + 2.0f * box_padding};
    draw->AddRectFilled(box, box_max, colors::kRow);
    draw->AddRectFilled(box, {box.x + px(2.0f), box_max.y}, colors::kAccentBright);
    const float width = outer_width - 2.0f * box_padding - px(4.0f);
    const ImVec2 at{box.x + box_padding + px(4.0f), box.y + box_padding};
    ImGui::SetCursorScreenPos(at);
    // A description that would wrap past its two lines (a narrow or a very
    // wide window) is set smaller rather than run into the hints below.
    float size = ImGui::GetStyle().FontSizeBase * 0.88f;
    const float smallest = ImGui::GetStyle().FontSizeBase * 0.66f;
    std::string description = break_lines(Layer::get().description(), size, width);
    while (size > smallest &&
           ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, width, description.c_str()).y > description_height) {
        size -= 1.0f;
        description = break_lines(Layer::get().description(), size, width);
    }
    ImGui::PushFont(nullptr, size);
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kTextDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos({box.x, box_max.y + px(10.0f)});
}

void end_panel() {
    if (page_open) {
        page_open = false;
        ImGui::EndChild();
    }
    ImGui::End();
}

bool tab_bar(const char *const *labels, int count, int &selected) {
    const int before = selected;
    const bool typing = ImGui::GetIO().WantTextInput;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_Q, false)))
        selected = (selected + count - 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_W, false)))
        selected = (selected + 1) % count;

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const bool pad = Layer::get().input_device() == InputDevice::Gamepad;
    const float height = font() * 1.9f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    // The switch buttons at both ends.
    const float cap_left = draw_cap(draw, {start.x, start.y + (height - font()) * 0.5f},
                                    pad ? shoulder_name(false) : "Q", false);
    const float cap_right_width = ImGui::CalcTextSize(pad ? shoulder_name(true) : "W").x + font() * 0.7f;
    draw_cap(draw, {start.x + width - std::max(font() * 1.15f, cap_right_width), start.y + (height - font()) * 0.5f},
             pad ? shoulder_name(true) : "W", false);

    const float inner_left = start.x + cap_left + px(14.0f);
    const float inner_width = width - cap_left - std::max(font() * 1.15f, cap_right_width) - px(28.0f);
    const float tab_width = inner_width / static_cast<float>(count);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < count; ++i) {
        const ImVec2 min{inner_left + tab_width * static_cast<float>(i), start.y};
        const ImVec2 max{min.x + tab_width, start.y + height};
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", {tab_width, height})) selected = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool active = i == selected;
        if (hovered && !active) draw->AddRectFilled(min, max, colors::kRowHover);
        const char *label = tr(labels[i]);
        const ImVec2 size = ImGui::CalcTextSize(label);
        draw->AddText({min.x + (tab_width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f},
                      active ? colors::kText : colors::kTextDim, label);
        // The open tab is underlined in vermilion across the tab's width.
        if (active)
            draw->AddRectFilled({min.x + px(6.0f), max.y - px(2.0f)}, {max.x - px(6.0f), max.y}, colors::kAccent);
    }
    // A hairline under the whole row of tabs.
    draw->AddLine({inner_left, start.y + height}, {inner_left + inner_width, start.y + height}, colors::kPanelEdge,
                  px(1.0f));
    ImGui::PopItemFlag();
    ImGui::SetCursorScreenPos({start.x, start.y + height + px(8.0f)});
    ImGui::Dummy({0.0f, 0.0f});
    return selected != before;
}

void focus_next_row() { focus_next = true; }

void language_row() {
    settings::Settings &s = settings::current();
    // The choices in their own languages, and the label in both, so the row
    // is found whichever language is on.
    static const char *const kSettings[] = {"", "ja", "en"};
    int current = 0;
    for (int i = 0; i < 3; ++i)
        if (s.language == kSettings[i]) current = i;
    const auto name = [](Language language) { return language == Language::Japanese ? "日本語" : "English"; };
    const std::string value = current == 0 ? std::string(tr("System")) + " (" + name(language_for("")) + ")"
                                            : std::string(name(language_for(kSettings[current])));
    RowOptions o;
    o.description = "The language of Tutikumo's menus and screens. The game's own text stays as it is.";
    if (const char *variable = settings::overridden_by("ui.language")) {
        o.disabled = true;
        o.note = std::string("Set by ") + variable;
    }
    if (const int delta = choice_row("Language / 言語##language", value, o)) {
        s.language = kSettings[((current + delta) % 3 + 3) % 3];
        set_language(language_for(s.language));
        settings::save();
    }
}

int choice_row(const char *label, const std::string &value, const RowOptions &options) {
    const Row r = row(label, options);
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const float right_arrow_x = r.max.x - px(16.0f) - font() * 0.45f;
    const float value_x = r.max.x - px(16.0f) - font() * 1.1f - ImGui::CalcTextSize(tr(value).c_str()).x;
    const float left_arrow_x = value_x - font() * 0.65f;
    int delta = r.focused ? horizontal_press() : 0;
    if (r.pressed) {
        // A click on the left arrow steps back; anything else steps forward.
        const float mouse_x = ImGui::GetIO().MousePos.x;
        const bool on_left_arrow = ImGui::GetIO().MouseReleased[0] && mouse_x > left_arrow_x - font() &&
                                   mouse_x < left_arrow_x + font() * 0.6f;
        delta = on_left_arrow ? -1 : 1;
    }
    const bool live = r.focused || r.hovered;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw_value(r, value, options, px(16.0f) + font() * 1.1f, live ? colors::kAccentBright : colors::kText);
    // A locked row shows its value without the arrows that would change it.
    if (options.disabled) return 0;
    const ImU32 arrow = live ? colors::kAccent : colors::kTextDim;
    draw_triangle(draw, {right_arrow_x, mid_y}, font() * 0.55f, true, arrow);
    draw_triangle(draw, {left_arrow_x, mid_y}, font() * 0.55f, false, arrow);
    return delta;
}

bool toggle_row(const char *label, bool value, const RowOptions &options) {
    const Row r = row(label, options);
    const int delta = r.focused ? horizontal_press() : 0;
    const bool toggled = !options.disabled && (r.pressed || (delta > 0 && !value) || (delta < 0 && value));

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float height = font() * 0.95f;
    const float width = height * 1.9f;
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const ImVec2 max{r.max.x - px(16.0f), mid_y + height * 0.5f};
    const ImVec2 min{max.x - width, mid_y - height * 0.5f};
    const bool on = toggled ? !value : value;
    const ImU32 track = options.disabled ? colors::kTrack : on ? colors::kAccent : colors::kTrack;
    draw->AddRectFilled(min, max, track, px(1.0f));
    // A square knob sliding in a straight track.
    const float knob = height * 0.5f - px(3.0f);
    const ImVec2 knob_center{on ? max.x - height * 0.5f : min.x + height * 0.5f, mid_y};
    draw->AddRectFilled({knob_center.x - knob, knob_center.y - knob}, {knob_center.x + knob, knob_center.y + knob},
                        options.disabled ? colors::kTextDisabled : colors::kText);
    draw_value(r, on ? "On" : "Off", options, px(16.0f) + width + px(12.0f),
               on ? colors::kAccentBright : colors::kTextDim);
    return toggled;
}

bool slider_row(const char *label, int &value, int minimum, int maximum, int step, const char *format,
                const RowOptions &options) {
    const Row r = row(label, options);
    const int before = value;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const float bar_width = std::min(font() * 10.0f, (r.max.x - r.min.x) * 0.38f);
    const ImVec2 bar_max{r.max.x - px(16.0f), mid_y + px(3.0f)};
    const ImVec2 bar_min{bar_max.x - bar_width, mid_y - px(3.0f)};

    if (!options.disabled) {
        if (r.focused) value += horizontal_press() * step;
        // Dragging along the bar sets the value directly.
        static ImGuiID dragging = 0;
        const ImGuiID id = ImGui::GetItemID();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemActivated() && mouse.x >= bar_min.x - font() && mouse.x <= bar_max.x + font()) dragging = id;
        if (dragging == id) {
            if (ImGui::IsMouseDown(0)) {
                const float t = std::clamp((mouse.x - bar_min.x) / bar_width, 0.0f, 1.0f);
                const float raw = static_cast<float>(minimum) + t * static_cast<float>(maximum - minimum);
                value = minimum + static_cast<int>(std::round((raw - static_cast<float>(minimum)) / step)) * step;
            } else {
                dragging = 0;
            }
        }
        value = std::clamp(value, minimum, maximum);
    }

    const float t = static_cast<float>(value - minimum) / static_cast<float>(std::max(1, maximum - minimum));
    const bool live = (r.focused || r.hovered) && !options.disabled;
    draw->AddRectFilled({bar_min.x, mid_y - px(1.0f)}, {bar_max.x, mid_y + px(1.0f)}, colors::kTrack);
    draw->AddRectFilled({bar_min.x, mid_y - px(1.5f)}, {bar_min.x + bar_width * t, mid_y + px(1.5f)},
                        options.disabled ? colors::kTextDisabled : colors::kAccent);
    // The knob is the emblem's diamond.
    draw_diamond(draw, {bar_min.x + bar_width * t, mid_y}, font() * (live ? 0.9f : 0.72f),
                 options.disabled ? colors::kTextDisabled : live ? colors::kAccentBright : colors::kText, true);
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    draw_value(r, text, options, px(16.0f) + bar_width + font() * 1.0f,
               live ? colors::kAccentBright : colors::kText);
    return value != before;
}

bool button_row(const char *label, const RowOptions &options, ImU32 color) {
    const Row r = row(label, options, color);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const ImU32 chevron = options.disabled ? colors::kTextDisabled : (r.focused || r.hovered) ? colors::kAccent
                                                                                            : colors::kTextDim;
    const float x = r.max.x - px(16.0f) - font() * 0.3f;
    const float s = font() * 0.28f;
    draw->AddLine({x - s, mid_y - s * 1.6f}, {x + s * 0.6f, mid_y}, chevron, px(2.0f));
    draw->AddLine({x + s * 0.6f, mid_y}, {x - s, mid_y + s * 1.6f}, chevron, px(2.0f));
    if (options.disabled && !options.note.empty()) draw_value(r, "", options, px(16.0f) + font() * 1.2f, 0);
    return r.pressed && !options.disabled;
}

bool value_row(const char *label, const std::string &value, const RowOptions &options) {
    const Row r = row(label, options);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const bool live = (r.focused || r.hovered) && !options.disabled;
    const ImU32 chevron = options.disabled ? colors::kTextDisabled : live ? colors::kAccent : colors::kTextDim;
    const float x = r.max.x - px(16.0f) - font() * 0.3f;
    const float s = font() * 0.28f;
    draw->AddLine({x - s, mid_y - s * 1.6f}, {x + s * 0.6f, mid_y}, chevron, px(2.0f));
    draw->AddLine({x + s * 0.6f, mid_y}, {x - s, mid_y + s * 1.6f}, chevron, px(2.0f));
    draw_value(r, value, options, px(16.0f) + font() * 1.1f,
               options.warning ? colors::kDanger : live ? colors::kAccentBright : colors::kText);
    return r.pressed && !options.disabled;
}

bool list_row(const char *id, const std::string &name, const std::string &detail, ListIcon icon, bool highlight) {
    const Row r = row(id, {});
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float mid_y = (r.min.y + r.max.y) * 0.5f;
    const float icon_x = r.min.x + px(18.0f);
    const float unit = font() * 0.5f;
    const ImU32 ink = highlight ? colors::kAccent : colors::kTextDim;
    switch (icon) {
    case ListIcon::Folder:
    case ListIcon::ParentFolder: {
        const ImVec2 min{icon_x, mid_y - unit * 0.7f};
        const ImVec2 max{icon_x + unit * 2.0f, mid_y + unit * 0.8f};
        draw->AddRectFilled({min.x, min.y - unit * 0.3f}, {min.x + unit * 0.9f, min.y + unit * 0.2f}, ink, px(2.0f));
        draw->AddRectFilled(min, max, ink, px(2.0f));
        if (icon == ListIcon::ParentFolder)
            draw->AddTriangleFilled({min.x + unit, min.y + unit * 0.25f}, {min.x + unit * 0.55f, min.y + unit * 0.9f},
                                    {min.x + unit * 1.45f, min.y + unit * 0.9f}, colors::kPanel);
        break;
    }
    case ListIcon::Disc:
        draw->AddCircle({icon_x + unit, mid_y}, unit * 0.85f, ink, 0, px(2.0f));
        draw->AddCircleFilled({icon_x + unit, mid_y}, unit * 0.25f, ink);
        break;
    case ListIcon::Drive:
        draw->AddRect({icon_x, mid_y - unit * 0.55f}, {icon_x + unit * 2.0f, mid_y + unit * 0.55f}, ink, px(2.0f), 0,
                      px(2.0f));
        draw->AddCircleFilled({icon_x + unit * 1.55f, mid_y}, unit * 0.15f, ink);
        break;
    case ListIcon::File:
        draw->AddRect({icon_x + unit * 0.3f, mid_y - unit * 0.8f}, {icon_x + unit * 1.7f, mid_y + unit * 0.8f}, ink,
                      px(2.0f), 0, px(1.5f));
        break;
    case ListIcon::None: break;
    }
    const float text_x = icon == ListIcon::None ? r.min.x + px(16.0f) : icon_x + unit * 2.0f + px(14.0f);
    const float text_y = mid_y - font() * 0.5f;
    const std::string shown_detail = tr(detail);
    const float detail_width = ImGui::CalcTextSize(shown_detail.c_str()).x;
    const float detail_x = r.max.x - px(16.0f) - detail_width;
    ImGui::PushClipRect({text_x, r.min.y}, {detail_x - px(12.0f), r.max.y}, true);
    draw->AddText({text_x, text_y}, highlight ? colors::kAccentBright : colors::kText, name.c_str());
    ImGui::PopClipRect();
    draw->AddText({detail_x, text_y}, colors::kTextDim, shown_detail.c_str());
    return r.pressed;
}

void info_row(const char *label, const std::string &english) {
    // Label above, value below and wrapped: paths can be long.
    const float width = ImGui::GetContentRegionAvail().x - px(32.0f);
    const std::string value = break_lines(tr_lines(english), ImGui::GetFontSize(), width);
    const float value_height = ImGui::CalcTextSize(value.c_str(), nullptr, false, width).y;
    const float height = std::round(font() * 1.5f + value_height);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, {ImGui::GetContentRegionAvail().x, height}, ImGuiButtonFlags_EnableNav);
    const bool focused = ImGui::IsItemFocused();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 max{min.x + width + px(32.0f), min.y + height};
    if (focused) {
        draw->AddRectFilled(min, max, colors::kRowFocus);
        draw->AddRectFilled(min, {min.x + px(3.0f), max.y}, colors::kAccent);
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
    draw->AddText({min.x + px(16.0f), min.y + px(5.0f)}, colors::kTextDim, tr_label(label).c_str());
    ImGui::PopFont();
    draw->AddText(nullptr, 0.0f, {min.x + px(16.0f), min.y + font() * 0.95f}, colors::kText, value.c_str(), nullptr,
                  width);
}

void section(const char *title) {
    ImGui::Dummy({0.0f, px(10.0f)});
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
    // A small vermilion diamond, the section's name, and a hairline after it.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float size = ImGui::GetFontSize();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw_diamond(draw, {at.x + px(16.0f) + size * 0.25f, at.y + size * 0.55f}, size * 0.5f, colors::kAccent, true);
    ImGui::SetCursorScreenPos({at.x + px(16.0f) + size * 0.8f, at.y});
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kTextDim);
    ImGui::TextUnformatted(tr(title));
    ImGui::PopStyleColor();
    const ImVec2 end = ImGui::GetItemRectMax();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    draw->AddLine({end.x + px(10.0f), at.y + size * 0.55f}, {right - px(16.0f), at.y + size * 0.55f},
                  colors::kPanelEdge, px(1.0f));
    ImGui::PopFont();
    ImGui::Dummy({0.0f, px(2.0f)});
}

bool big_button(const char *label, float width, bool primary, bool disabled) {
    const float height = std::round(font() * 2.2f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max{min.x + width, min.y + height};
    const bool pressed = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
    take_focus();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = 0.0f;
    ImU32 text = colors::kText;
    if (disabled) {
        draw->AddRect(min, max, colors::kTextDisabled, rounding, 0, px(1.0f));
        text = colors::kTextDisabled;
    } else if (primary) {
        // Vermilion, washi-white text; lit, a lighter vermilion.
        draw->AddRectFilled(min, max, focused || hovered ? IM_COL32(208, 84, 60, 255) : colors::kAccent, rounding);
    } else {
        draw->AddRectFilled(min, max, focused ? colors::kRowFocus : hovered ? colors::kRowHover : colors::kRow,
                            rounding);
        draw->AddRect(min, max, focused ? colors::kAccent : colors::kPanelEdge, rounding, 0, px(1.0f));
    }
    if (focused && !disabled)
        draw->AddRect({min.x - px(3.0f), min.y - px(3.0f)}, {max.x + px(3.0f), max.y + px(3.0f)},
                      colors::kAccentBright, rounding, 0, px(1.5f));
    const std::string shown = tr_label(label);
    const ImVec2 size = ImGui::CalcTextSize(shown.c_str());
    draw->AddText({min.x + (width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f}, text, shown.c_str());
    return pressed && !disabled;
}

void progress_bar(float fraction, const std::string &english) {
    const std::string overlay = tr(english);
    const float height = std::round(font() * 1.6f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy({width, height});
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 max{min.x + width, min.y + height};
    draw->AddRectFilled(min, max, colors::kTrack);
    const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
    if (filled > 1.0f) draw->AddRectFilled(min, {min.x + filled, max.y}, colors::kAccent);
    const ImVec2 size = ImGui::CalcTextSize(overlay.c_str());
    draw->AddText({min.x + (width - size.x) * 0.5f, min.y + (height - size.y) * 0.5f}, colors::kText,
                  overlay.c_str());
}

void paragraph(const std::string &text, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(break_lines(tr_lines(text), ImGui::GetFontSize(), width).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void heading(const std::string &text) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kAccentBright);
    ImGui::TextUnformatted(tr(text).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy({0.0f, px(4.0f)});
}

void hints(std::initializer_list<Hint> list) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const bool pad = Layer::get().input_device() == InputDevice::Gamepad;
    const bool south = Layer::get().confirm_south();
    const SDL_GamepadButton confirm = south ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
    const SDL_GamepadButton back = south ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImVec2 at = start;
    const float gap = px(6.0f);
    for (const Hint &hint : list) {
        // The browser's file filter switch has a button on the pad only.
        const bool pad_only = hint.control == Control::Toggle || hint.control == Control::Shift ||
                              hint.control == Control::Space || hint.control == Control::Symbols ||
                              hint.control == Control::Reset;
        if (pad_only && !pad) continue;
        float x = at.x;
        const auto cap = [&](const char *text) { x += draw_cap(draw, {x, at.y}, text, false) + gap; };
        const auto arrows = [&] {
            const float size = font() * 1.15f;
            for (int i = 0; i < 2; ++i) {
                const ImVec2 min{x, at.y + (font() - size) * 0.5f};
                draw->AddRect(min, {min.x + size, min.y + size}, colors::kTextDim, size * 0.3f, 0, px(1.5f));
                draw_triangle(draw, {min.x + size * 0.5f, min.y + size * 0.5f}, size * 0.4f, i == 1, colors::kText);
                x += size + gap;
            }
        };
        switch (hint.control) {
        case Control::Confirm:
            if (pad) x += draw_face(draw, {x, at.y}, confirm) + gap;
            else cap("Enter");
            break;
        case Control::Back:
            if (pad) x += draw_face(draw, {x, at.y}, back) + gap;
            else cap("Esc");
            break;
        case Control::Tabs:
            cap(pad ? shoulder_name(false) : "Q");
            cap(pad ? shoulder_name(true) : "W");
            break;
        case Control::Change: arrows(); break;
        case Control::Menu:
            if (pad) {
                cap("L3");
                cap("R3");
            } else {
                cap("Esc");
            }
            break;
        case Control::Start: cap(pad ? "Start" : "Enter"); break;
        case Control::Toggle:
        case Control::Space: x += draw_face(draw, {x, at.y}, SDL_GAMEPAD_BUTTON_NORTH) + gap; break;
        case Control::Delete:
            if (pad) x += draw_face(draw, {x, at.y}, back) + gap;
            else cap("Backspace");
            break;
        case Control::Shift: x += draw_face(draw, {x, at.y}, SDL_GAMEPAD_BUTTON_WEST) + gap; break;
        case Control::Reset: cap(select_name()); break;
        case Control::Clear:
            if (pad) x += draw_face(draw, {x, at.y}, SDL_GAMEPAD_BUTTON_NORTH) + gap;
            else cap("Del");
            break;
        case Control::Symbols: cap(select_name()); break;
        case Control::Cursor:
            if (pad) {
                cap(shoulder_name(false));
                cap(shoulder_name(true));
            } else {
                arrows();
            }
            break;
        }
        const char *text = tr(hint.text);
        draw->AddText({x + px(2.0f), at.y}, colors::kTextDim, text);
        at.x = x + ImGui::CalcTextSize(text).x + px(26.0f);
    }
    // Room for what was drawn, so a window around the hints can fit them.
    ImGui::Dummy({std::max(0.0f, at.x - start.x - px(24.0f)), font()});
}

} // namespace tutikumo::ui
