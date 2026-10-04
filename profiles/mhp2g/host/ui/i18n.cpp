#include "ui/i18n.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tutikumo::ui {
namespace {

struct Entry {
    const char *english;
    const char *japanese;
};

constexpr Entry kJapanese[] = {
#include "ui/i18n_ja.inc"
#include "ui/i18n_ja_menu2.inc"
#include "ui/i18n_ja_screens.inc"
#include "ui/i18n_ja_tools.inc"
#include "ui/i18n_ja_mods.inc"
#include "ui/i18n_ja_input.inc"
#include "ui/i18n_ja_install.inc"
};

Language current_language = Language::English;

// An entry whose English has "{}" for parts that vary: the fixed pieces
// between them, in order.
struct Pattern {
    std::vector<std::string_view> pieces;
    const char *japanese;
    std::size_t fixed{};  // characters in the pieces: the longest match wins
};

struct Table {
    std::unordered_map<std::string_view, const char *> whole;
    std::vector<Pattern> patterns;
};

const Table &table() {
    static const Table t = [] {
        Table result;
        for (const Entry &entry : kJapanese) {
            const std::string_view english(entry.english);
            if (english.find("{}") == std::string_view::npos) {
                result.whole.emplace(english, entry.japanese);
                continue;
            }
            Pattern pattern;
            pattern.japanese = entry.japanese;
            std::size_t start = 0;
            for (;;) {
                const std::size_t hole = english.find("{}", start);
                pattern.pieces.push_back(english.substr(start, hole == std::string_view::npos ? hole : hole - start));
                if (hole == std::string_view::npos) break;
                start = hole + 2u;
            }
            for (std::string_view piece : pattern.pieces) pattern.fixed += piece.size();
            result.patterns.push_back(std::move(pattern));
        }
        return result;
    }();
    return t;
}

const char *whole(std::string_view english) {
    const Table &t = table();
    const auto found = t.whole.find(english);
    return found != t.whole.end() ? found->second : nullptr;
}

// The parts of `text` where `pattern` has "{}", or false when it does not
// match. The first piece must start the text and the last end it.
bool match(const Pattern &pattern, std::string_view text, std::vector<std::string_view> &parts) {
    parts.clear();
    const std::vector<std::string_view> &pieces = pattern.pieces;
    if (text.size() < pattern.fixed) return false;
    if (text.substr(0, pieces.front().size()) != pieces.front()) return false;
    const std::string_view &last = pieces.back();
    if (text.substr(text.size() - last.size()) != last) return false;
    std::size_t at = pieces.front().size();
    const std::size_t end = text.size() - last.size();
    if (at > end) return false;
    for (std::size_t i = 1; i + 1 < pieces.size(); ++i) {
        const std::size_t found = pieces[i].empty() ? at : text.find(pieces[i], at);
        if (found == std::string_view::npos || found + pieces[i].size() > end) return false;
        parts.push_back(text.substr(at, found - at));
        at = found + pieces[i].size();
    }
    parts.push_back(text.substr(at, end - at));
    return true;
}

// The Japanese of a pattern with its parts put in: "{}" takes the next part,
// "{0}", "{1}"... a given one.
std::string fill(const char *japanese, const std::vector<std::string_view> &parts) {
    std::string result;
    std::size_t next = 0;
    for (const char *p = japanese; *p != '\0';) {
        if (p[0] == '{' && p[1] == '}') {
            if (next < parts.size()) result += tr(std::string(parts[next]));
            ++next;
            p += 2;
        } else if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') {
            const std::size_t index = static_cast<std::size_t>(p[1] - '0');
            if (index < parts.size()) result += tr(std::string(parts[index]));
            p += 3;
        } else {
            result += *p++;
        }
    }
    return result;
}

bool trace() {
    static const bool on = [] {
        const char *value = std::getenv("TUTIKUMO_TRACE_I18N");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    return on;
}

// Texts translated through a pattern, and those with no translation, so a
// text drawn every frame is matched once.
std::unordered_map<std::string, std::string> &matched() {
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}
bool trim_due = false;

const std::string *translated(const std::string &english) {
    std::unordered_map<std::string, std::string> &cache = matched();
    const auto cached = cache.find(english);
    if (cached != cache.end()) return &cached->second;
    const Pattern *best = nullptr;
    std::vector<std::string_view> parts, best_parts;
    for (const Pattern &pattern : table().patterns)
        if ((best == nullptr || pattern.fixed > best->fixed) && match(pattern, english, parts)) {
            best = &pattern;
            best_parts = parts;
        }
    std::string result;
    if (best != nullptr) {
        result = fill(best->japanese, best_parts);
    } else {
        result = english;
        static std::unordered_set<std::string> reported;
        if (trace() && reported.insert(english).second) std::cout << "[i18n] untranslated: " << english << '\n';
    }
    if (cache.size() > 8192u) trim_due = true;
    return &cache.emplace(english, std::move(result)).first->second;
}

bool has_letters(std::string_view text) {
    for (char c : text)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    return false;
}

} // namespace

Language language() { return current_language; }

void set_language(Language language) { current_language = language; }

Language language_for(const std::string &setting) {
    if (setting == "ja") return Language::Japanese;
    if (setting == "en") return Language::English;
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    Language result = Language::English;
    if (locales != nullptr) {
        if (count > 0 && locales[0] != nullptr && locales[0]->language != nullptr &&
            std::strcmp(locales[0]->language, "ja") == 0)
            result = Language::Japanese;
        SDL_free(locales);
    }
    return result;
}

const char *tr(const char *english) {
    if (current_language == Language::English || english == nullptr || *english == '\0') return english;
    if (const char *found = whole(english)) return found;
    if (!has_letters(english)) return english;
    return translated(english)->c_str();
}

std::string tr(const std::string &english) {
    if (current_language == Language::English || english.empty()) return english;
    if (const char *found = whole(english)) return found;
    if (!has_letters(english)) return english;
    return *translated(english);
}

std::string tr_lines(const std::string &english) {
    if (current_language == Language::English || english.find('\n') == std::string::npos) return tr(english);
    std::string result;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = english.find('\n', start);
        result += tr(english.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        result += '\n';
        start = end + 1u;
    }
    return result;
}

std::string tr_label(const char *label) {
    const char *hidden = std::strstr(label, "##");
    const std::string shown = hidden != nullptr ? std::string(label, hidden) : std::string(label);
    return tr(shown);
}

void i18n_new_frame() {
    if (!trim_due) return;
    trim_due = false;
    matched().clear();
}

} // namespace tutikumo::ui
