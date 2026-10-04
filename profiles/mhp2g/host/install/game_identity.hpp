#pragma once

// Identity of the one release this profile supports. The hashes match
// config/mhp2g_uljm05500.toml and the profile README.
namespace tutikumo::install {

// DISC_ID in PSP_GAME/PARAM.SFO.
inline constexpr const char *kDiscId = "ULJM05500";
inline constexpr const char *kDiscIdDisplay = "ULJM-05500";
inline constexpr const char *kGameTitle = "Monster Hunter Portable 2nd G";

inline constexpr const char *kExecutablePathOnDisc = "PSP_GAME/SYSDIR/EBOOT.BIN";
// The disc also carries the executable unencrypted, which is what the profile
// prepares the game from.
inline constexpr const char *kPlainExecutablePathOnDisc = "PSP_GAME/SYSDIR/BOOT.BIN";
inline constexpr const char *kParamSfoPathOnDisc = "PSP_GAME/PARAM.SFO";

// SHA-256 of PSP_GAME/SYSDIR/EBOOT.BIN as it is on the disc.
inline constexpr const char *kEncryptedExecutableSha256 =
    "a62ecebb1bd42a15c556e1f053d3535cc14b0904214c16182cf9762a085f595a";
// SHA-256 of the executable the recompiled code was generated from
// (PSP_GAME/SYSDIR/BOOT.BIN).
inline constexpr const char *kExecutableSha256 =
    "3c221249fcc1c3455ea8aff372993cb78a88ad90bde03a07c3da6974c2bcc846";

// SHA-256 of the MHP3rd HD executable the title-specific hooks (analog
// camera, view shape, HUD, layered armor) were written against. They check
// the code they patch themselves; this keeps them off on any other game.
inline constexpr const char *kMhp3rdExecutableSha256 =
    "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";

} // namespace tutikumo::install
