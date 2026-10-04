# AGENTS.md

Working notes for anyone changing this repository: people and coding agents alike. Read this first.

Tutikumo is a native port of *Monster Hunter Portable 2nd G* (`ULJM-05500`). It recompiles the game's PSP (MIPS) code to C++ ahead of time and supplies the PSP system around it: the kernel, HLE modules, a Vulkan GE renderer, audio, input, save data, ad hoc networking and an ImGui interface. The C++ lives in `profiles/mhp2g/host/` (the port) and `include/psprecomp/` plus `src/` (the reusable runtime and recompiler). It is built on Yakumo, TeamGDB's port of MHP3rd HD; keep the notices in `LICENSE`.

Identifiers use `tutikumo` (the `tutikumo::` namespace, `TUTIKUMO_*` environment variables and macros, `tutikumo_*` test targets). `MHP3rd` in comments means the game Yakumo was written for, where a fact or a constant comes from it.

## Rules

- **No game data, ever.** Disc images, `EBOOT`/`DATA.BIN` contents, the generated code (`profiles/mhp2g/generated/`), overlay corpora (`profiles/mhp2g/overlays/`), and saves stay local; they are ignored by Git.
- **Write it yourself.** Never copy or line-by-line translate code from a project whose licence is incompatible with the MIT licence. Constants, offsets and format facts are fine. Record where intentionally included third-party code comes from; see [SOURCE_PROVENANCE.md](docs/SOURCE_PROVENANCE.md).
- **Say what you did not verify.**
- **Game-specific hooks check the code they patch.** Every driver that depends on MHP2G's code (the analog camera in `host/camera/game_camera.cpp`) lists the instructions it relies on and stays off if any differs.

## Building

[docs/BUILDING.md](docs/BUILDING.md) is the full guide (Windows, MSVC).

- **Build commands.** Build with `cmake --build out/mhp2g`. Keep parallelism moderate: generated units need gigabytes of memory each.
- **Overlays.** `profiles/mhp2g/scripts/build_overlays.sh out/mhp2g <jobs>` (Git Bash) extracts all 298 overlays from `DATA.BIN` and builds a library for each, about an hour the first time.
- **Host-only changes rebuild in minutes.** Changes under `include/psprecomp/` rebuild everything, overlays included.

## Running and testing

- **Bound every run**, and never drive the game with an open-ended input loop.
- **Quick boot checks.** `TUTIKUMO_NO_RENDER=1 TUTIKUMO_NO_AUDIO=1`, then look for the function count and `[overlay] installed` in the output.
- **Scripted input and captures.** `TUTIKUMO_INPUT_SCRIPT` (syntax in `profiles/mhp2g/host/ui/input_script.hpp`) and `TUTIKUMO_SCREENSHOT_DIR` (the directory must exist). Look at the captures; don't assume.
- **Numbers.** `TUTIKUMO_PERF=log` prints fps, the game's own frame rate, speed and the frame's vertex space once per second.
- **Unit tests.** `cmake --build out/mhp2g && ctest --test-dir out/mhp2g`.

## Interface text

The interface is written in English in the code and translated where it is drawn (`host/ui/i18n.hpp`): the widgets pass their labels, values and descriptions through `tr()`, which looks them up in the Japanese tables `host/ui/i18n_ja*.inc`, whole or against entries with `{}` for the parts that vary. A new or changed English text needs its entry there; `TUTIKUMO_TRACE_I18N=1` with `TUTIKUMO_LANGUAGE=ja` prints every text drawn without one.

## Lessons that cost real time

- **Trace; don't recall.** Add or use a `TUTIKUMO_TRACE_*` switch and read what the game actually does.
- **Generated code may split a function.** A long guest function can return to the dispatcher part way through and carry on from there, so a host wrapper around it does not necessarily see the whole call (the camera's pitch had to be applied at the point of use for this reason).
- **MHP2G draws sparse indexed prims.** Its terrain indexes a few hundred vertices across a large shared buffer; decoding the whole span filled the frame's vertex space and dropped the hunter and the HUD in quests.
- **Import stubs inside `.sceStub.text`.** MHP2G keeps its stubs in an executable section; the recompiler must treat them as dispatcher boundaries.
