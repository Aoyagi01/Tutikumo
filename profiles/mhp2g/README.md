# MHP2G profile

This profile builds Tutikumo for **Monster Hunter Portable 2nd G** (`ULJM-05500`, disc version 1.01). See the top-level [README](../../README.md) for what it does and [docs/BUILDING.md](../../docs/BUILDING.md) for how to build it.

## The game

| Item | Value |
| --- | --- |
| Module | `MonsterHunterPortable2ndG` |
| Executable | `PSP_GAME/SYSDIR/BOOT.BIN`, unencrypted (SHA-256 `3c221249…c846`) |
| Load image | `0x08804000`–`0x09D60400` (fits 32 MiB; the host gives it 64) |
| Entry / `gp` | `0x089094D4` / `0x089CEB50` |
| Imports | 290; the HTTP/SSL/infrastructure ones are logging stubs |
| Saves | `ULJM05500` (game data), `ULJM05500DAT` (install data) |

`DATA.BIN` uses the same archive and cipher as MHP3rd's: `tools/databin.py` lists and extracts it. It holds 298 code overlays (`MWo3` images) in four slots: the `*_task` modes at `0x09A5A580`, `demo_sub`/`game_sub` at `0x09C14280`, the `em*` monsters at `0x09D15100` and the `stage*` areas at `0x09D5DF80`.

## Game-specific drivers

- **Analog camera** (`host/camera/game_camera.cpp`). The camera update `0x08886B54` is wrapped: before it runs, the stick turns the target and shown yaw (+0x80/+0x82 of the camera, 0xA0 into the update's argument). For pitch, the hunter state check `0x08865CB0` called right before the eye's offset is read from the preset (+0x70) is wrapped too, and hands the camera a copy of its preset with the offset turned about the look-at point. Twenty-one instructions are checked first. The village and the house skip the update (MHP2G has no camera turn there).
- **Movies.** MHP2G uses the older `sceMpegAvcDecode` interface, which writes RGBA itself.
- **Texture packs.** `textures/ULJM05500` in the data directory; PNG or KTX2 images, xxh64, xxh32 or quick hashes.

Not ported from Yakumo's MHP3rd profile: hiding the HUD, lock-on, layered armor and equipment mods, the free camera and the debug tools, which read MHP3rd's own structures. The view shape is the PSP's (480:272 is close to 16:9).

## Settings and diagnostics

Settings are kept in `%APPDATA%\Tutikumo\MHP2G\settings.ini` and changed in the in-game menu (Esc, or L3+R3). Useful environment variables:

| Variable | Effect |
| --- | --- |
| `TUTIKUMO_DATA_DIR` | Use another data folder |
| `TUTIKUMO_INTERNAL_SCALE` | Render scale (`1`–`6`, `auto`) |
| `TUTIKUMO_FRAME_RATE` | `30`, `45`, `60`, `90`, `120`, `display` |
| `TUTIKUMO_PERF=log` | One line of frame statistics per second |
| `TUTIKUMO_NO_RENDER`, `TUTIKUMO_NO_AUDIO` | Run without a window or sound |
| `TUTIKUMO_INPUT_SCRIPT` | Scripted input and window captures (`host/ui/input_script.hpp`) |
| `TUTIKUMO_SCREENSHOT_DIR` | Write the game's frames as BMP (the folder must exist) |
| `TUTIKUMO_TRACE_CAMERA`, `_IO`, `_KERNEL`, `_MPEG`, `_TEXTURE_PACK`, … | Trace one subsystem |
| `TUTIKUMO_DUMP_OVERLAYS` | Dump an overlay that has no recompiled library |
| `TUTIKUMO_NO_COMPACT_INDICES` | Decode sparse indexed prims in full, as before |
