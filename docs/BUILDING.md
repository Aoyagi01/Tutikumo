# Building Tutikumo (Windows)

## Requirements

| Tool | Notes |
| --- | --- |
| Visual Studio 2022 or newer, or Build Tools | The *Desktop development with C++* workload (MSVC and the Windows SDK) |
| CMake 3.20+ and Ninja | |
| Python 3 | On `PATH` as `python3` (copy `python.exe` to `python3.exe` in its folder; the Microsoft Store alias does not work) |
| Git for Windows | Git Bash runs `profiles/mhp2g/scripts/*.sh` |
| Vulkan SDK (LunarG) | The loader, headers and `glslangValidator` |
| SDL3 | The official `SDL3-devel-*-VC.zip`; pass its folder in `CMAKE_PREFIX_PATH` |

FFmpeg (music and the opening movie) is downloaded as a pinned, checksum-verified prebuilt LGPL build the first time the build directory is configured.

Run everything below from a developer prompt (`Enter-VsDevShell` / *x64 Native Tools Command Prompt*) so MSVC is on `PATH`.

## Steps

```powershell
# 1. The game: the plain executable is PSP_GAME/SYSDIR/BOOT.BIN on the disc.
#    Put it and the image in profiles/mhp2g/game (ignored by Git).
python profiles/mhp2g/tools/extract_iso.py <your.iso> work /PSP_GAME/SYSDIR/BOOT.BIN
copy work\PSP_GAME\SYSDIR\BOOT.BIN profiles\mhp2g\game\EBOOT.ELF
mklink /H profiles\mhp2g\game\disc.iso <your.iso>

# 2. Configure, recompile the executable, build.
cmake -S . -B out/mhp2g -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mhp2g -DCMAKE_PREFIX_PATH=<SDL3 folder>
cmake --build out/mhp2g --target psp_recomp
out\mhp2g\psp_recomp.exe profiles\mhp2g\game\EBOOT.ELF --auto profiles\mhp2g\generated
cmake -S . -B out/mhp2g
cmake --build out/mhp2g --target Tutikumo -j 6

# 3. The code overlays (about an hour; resumable).
bash profiles/mhp2g/scripts/build_overlays.sh out/mhp2g 4

# 4. Run. Copy SDL3.dll next to Tutikumo.exe first.
out\mhp2g\bin\Tutikumo.exe
```

Instead of the `profiles/mhp2g/game` folder, a player can let Tutikumo set itself up: `Tutikumo.exe --install <your.iso>` checks the image (`ULJM-05500`, unmodified), takes `BOOT.BIN` from it and keeps everything in `%APPDATA%\Tutikumo\MHP2G\`.

The expected executable: SHA-256 `3c221249fcc1c3455ea8aff372993cb78a88ad90bde03a07c3da6974c2bcc846` (`BOOT.BIN`); the encrypted `EBOOT.BIN` is `a62ecebb1bd42a15c556e1f053d3535cc14b0904214c16182cf9762a085f595a`.

## Tests

```powershell
cmake --build out/mhp2g
ctest --test-dir out/mhp2g
```
