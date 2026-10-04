#pragma once

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <iterator>

namespace tutikumo {

inline constexpr std::uint32_t kLoadBase = psprecomp::kDefaultPspUserLoadBase;
inline constexpr std::uint32_t kGuestRamBytes = 64u * 1024u * 1024u;

struct ProfilePaths {
    std::filesystem::path disc_image;    // UMD ISO; empty disables disc0:
    std::filesystem::path memory_stick;  // host directory backing ms0:
};

// Installs the kernel and HLE modules, binds logging stubs for the remaining
// imports (unless TUTIKUMO_STRICT_HLE is set) and prepares the loader thread
// that runs module_start.
void install_profile(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &elf, const ProfilePaths &paths);

// The host directory that backs ms0:, where the game's saves live under
// PSP/SAVEDATA. This is the only place that decides it; the rest of the host
// receives the result, so moving saves to a per-user location changes only
// this function.
[[nodiscard]] std::filesystem::path memory_stick_directory(const std::filesystem::path &game_dir);

// Overlay slots, from the executable's section table. They sit inside the load
// image's reserved BSS, and the game copies code into them at run time, so a
// jump into one stops the runtime until that overlay has its own corpus.
// The end of each slot is the start of the next one.
//
// MHP2G (ULJM05500), from the overlays in DATA.BIN (tools/databin.py list
// --overlays) and BOOT.BIN's zero-sized sections.
inline constexpr std::uint32_t kOverlaySlots[] = {
    0x09A5A580u,  // *_task mode overlays (12): demo, edit, game, lobby, ...
    0x09C14280u,  // demo_sub, game_sub
    0x09D15100u,  // em* monsters (17)
    0x09D5DF80u,  // stage* (267)
    0x09D60380u,  // one zero-sized section with no image in DATA.BIN
    0x09D60400u,  // end of the load image
};

} // namespace tutikumo
