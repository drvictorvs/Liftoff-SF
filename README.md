# Liftoff: Starfield load progress and diagnostics

An SFSE plugin that shows a compact overlay with a live progress bar while Starfield starts up
and while a save is loading, inspired by SkyLine (Startup Progress for Skyrim). It uses CommonLibSF
and the Address Library to detect when the player has entered the loaded world; players need the
SFSE and Address Library versions that match their game. Address Library is installed as a game
mod and is not included in this source repository.
The bar completes after SFSE reports that game data loading and its callbacks have finished,
which is the latest version-independent startup milestone exposed by the SFSE messaging API.
This can happen just before the main menu is rendered.
By default the overlay also shows for the first save load of a session (the one from the main
menu) and never again after it; `ShowOnSaveLoad` (or "Show on save loads" in OSF Settings) can
limit it to startup only or show it for every save load. During a shown save load it displays the
current save or tracked plugin/archive file. If SFSE does not send that event, Liftoff detects a read of a `.sfs` save.
The overlay closes after the player's cell and 3D are present for 45 consecutive game frames.
If the player was already in a loaded cell when the save read was detected, Liftoff waits at
least five seconds before accepting that state. The overlay may briefly appear while the save
browser scans save files.
Save-load progress is indeterminate because SFSE does not expose a percentage for that operation.
The overlay appears at the desktop's bottom-left immediately and follows the game window there
when it appears or moves. Its taskbar button is created at startup, before Starfield's main
window. As a separate Windows window, the overlay may not appear over exclusive-fullscreen rendering.
The overlay stays visible through each load and fades out five seconds after the player is live.
It stays hidden between startup and save loads.

By default the overlay copies Starfield's main menu: white uppercase text on a flat grey (#5D5D5D)
panel with a white progress bar. The panel colour and opacity, text colour and size, capitals and
bar colour are all adjustable. The text uses the game's own menu font, read at startup from its
interface files (`Data\Interface\fonts_<language>.swf` and `fontconfig_<language>.txt`, loose or in
an archive), so nothing is bundled and font replacers apply too. If those can't be read, it uses
NB Architekt if installed in Windows, otherwise Bahnschrift. Text size is given for a 1080p screen
and scales with the monitor. Clicking it does not dismiss it or take focus from
the game.

## Build (Windows, VS Code)

1. Install **Build Tools for Visual Studio** with the *Desktop development with C++* workload.
2. In VS Code, install the recommended extensions (C/C++ and CMake Tools).
3. Ensure internet access for CMake to fetch pinned CommonLibSF, commonlib-shared, spdlog, and
   MinHook sources. These are pinned in `CMakeLists.txt`; no recursive submodule checkout is needed.
4. Copy `.vscode/settings.example.json` to `.vscode/settings.json` and set `STARFIELD_DIR` to your
   Starfield folder. The local settings file is ignored by Git.
5. `Ctrl+Shift+P` → **CMake: Select a Kit** → the *Visual Studio … amd64* kit.
6. `Ctrl+Shift+P` → **CMake: Build** (or F7). The DLL is copied to `Data\SFSE\Plugins` automatically.
7. Copy `dist\Data\SFSE\Plugins\Liftoff.ini` there once.
8. For the OSF interface, install the Starfield Creation Kit and extract
   `Tools\ContentResources.zip` so `Starfield_Papyrus_Flags.flg` is under
   `Tools\ContentResources\Scripts\Source`. Find `OSFSettings.psc` in the installed OSF Settings
   mod's `Scripts\Source` folder, then run:
   `compile_papyrus.bat "<Starfield folder>" "<OSF Settings Scripts\Source folder>"`.
   Copy the generated `dist\Data\Scripts\Liftoff_OSF.pex` to the game's `Data\Scripts` folder.
9. Start the game with `sfse_loader.exe`.

The package also includes an OSF Settings schema at
`Data\SFSE\Plugins\OSF\Settings\schemas\liftoff.json`, which exposes the `Config.h` options
when OSF Settings is installed. The DLL calls the global Papyrus bridge without a quest or plugin
file, polls for OSF changes, and writes them to `Liftoff.ini`. Changes take effect next launch.
After changing OSF values, leave the game running for up to two seconds so the bridge can save them.
The look settings include a transparent panel option; turning off the Starfield-style font uses
Segoe UI.
Without the compiled `.pex` or OSF Settings, Liftoff continues to use `Liftoff.ini` normally.
No Liftoff `.esm` or quest attachment is needed: the DLL invokes the script's global function
through the Papyrus VM. The `.psc` source and compiler helper are included in the source package.

Logs: `Documents\My Games\Starfield\SFSE\Logs\Liftoff.log` (and SFSE's own `sfse.txt`).
Liftoff also logs file-open durations, periodic CPU/disk/memory samples, and thread stacks during
save loads or when the game window stops responding. Press **Ctrl+Shift+F9** to add a log marker
and collect follow-up samples for one minute.

## Release

`Ctrl+Shift+P` → **CMake: Select Variant** → Release, then **CMake: Build Target** → `package_mod`.
The upload-ready zip appears in `build\`.
Also build `package_source` and distribute the resulting `Liftoff-1.0.0-source.zip` alongside
the plugin package; it contains Liftoff and the corresponding CommonLibSF, commonlib-shared,
spdlog, and MinHook sources and license files.

## Files

| File | What it does |
|---|---|
| `src/main.cpp` | SFSE entry points: version data, `SFSEPlugin_Load`, message handler |
| `src/sfse_api.h` | The few SFSE structures we need (from SFSE's MIT-licensed PluginAPI.h) |
| `src/FileWatch.cpp` | Hooks `CreateFileW/A/2` with MinHook to see which `.esm`/`.ba2` files load |
| `src/FileTrace.cpp` | Logs native file-open durations and reports tracked files during save loads |
| `src/Watchdog.cpp` | Samples process resources and captures thread stacks for save loads and stalls |
| `src/Stacks.cpp` | Captures and describes thread call stacks |
| `src/Progress.cpp` | Turns file milestones into a percentage using the previous launch's timings |
| `src/GameReady.cpp` | Checks player cell and 3D readiness on the game thread |
| `src/Splash.cpp` | The window: own thread, GDI+ drawing, fade-out |
| `src/Config.cpp` | Reads `Liftoff.ini` |
| `src/OsfSettings.cpp` | Polls the Papyrus bridge and saves OSF values to `Liftoff.ini` |
| `scripts/Source/Liftoff_OSF.psc` | Reads Liftoff's values from OSF Settings; compiled to `Data/Scripts/Liftoff_OSF.pex` |
| `dist/Data/SFSE/Plugins/OSF/Settings/schemas/liftoff.json` | Defines Liftoff's OSF Settings page |
| `src/Log.cpp` | Log file |

## Credits

SFSE by ianpatt and team (plugin API header, MIT). CommonLibSF and commonlib-shared by libxse
(GPL-3.0-or-later with modding/linking exceptions). spdlog by Gabi Melman (MIT). MinHook by
Tsuda Kageyu (BSD-2-Clause). Idea from SkyLine by mrDrowner1.
