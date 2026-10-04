#pragma once

#include <string>

// The language of the port's own interface (the game keeps its own). Texts
// are written in English in the code and translated where they are drawn:
// tr() looks a text up in the table of translations (i18n_ja.inc), first
// whole, then against the entries with "{}" where a part of the text varies
// ("Set by {}"), whose parts are looked up in turn. A text without an entry
// stays in English.
namespace tutikumo::ui {

enum class Language { English, Japanese };

[[nodiscard]] Language language();
void set_language(Language language);
// The language a setting names: "en", "ja", or empty or "auto" for the
// system's (Japanese when the system prefers it, English otherwise).
[[nodiscard]] Language language_for(const std::string &setting);

// The text in the interface's language. The pointer stays valid until the
// next frame (i18n_new_frame()).
[[nodiscard]] const char *tr(const char *english);
[[nodiscard]] std::string tr(const std::string &english);
// Each line of a text translated on its own: a description with its note.
[[nodiscard]] std::string tr_lines(const std::string &english);
// Only the visible part of an ImGui label ("Name##id" gives "Name"),
// translated.
[[nodiscard]] std::string tr_label(const char *label);

// Once a frame: lets the cache of matched texts be trimmed.
void i18n_new_frame();

} // namespace tutikumo::ui
