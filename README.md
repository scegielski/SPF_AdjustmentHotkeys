# SPF Adjustment Hotkeys

Live, non-modal adjustments for American Truck Simulator through SPF hotkeysâ€”
adjust mirrors without opening menus, with cycling and audio feedback.

The current Windows x64 plugin provides tap-to-toggle or held adjustment of left, center and
right mirrors in American Truck Simulator. It also supports one-key cycling,
spoken selection cues, and a quiet servo loop routed left/center/right in stereo.

The current version is **0.2.1**, pinned to the inspected ATS **1.61.3.1s**
executable and **SPF 1.2.5**. The Road Trip Ford Mustang was tested in-game.
Other vehicles and executable builds have not been verified. Slot 4 can represent
a different mirror in other vehicles. Unknown executables register no hooks.

## Controls and settings

On a fresh installation only **WASD** is assigned. Choose a selector or cycle
binding in SPF Keybind Settings; left, center, right, cycle and reset start
unassigned. Updates preserve saved bindings, including deliberately cleared keys.

In SPF Settings, select this plugin and expand **General Settings**:

- **Hold adjustment selector**: off uses tap-to-toggle; on adjusts while held.
- **Adjustment voice cues** (off by default): announces the mirror when entering or switching.
- **Adjustment servo sound** (on by default): motor loop during directional adjustment.
- **Adjustment selection click** (on by default): one short click on selection or switching, including cycling.
  It follows left/center/right stereo routing and plays independently of speech. Exiting is silent.

Toggle cycle goes left â†’ center â†’ right â†’ off. Held cycle advances each press and
releases to exit. Pause, focus loss and world unload exit adjustment.
Servo audio is left-only for the left mirror, both channels for center, right-only
for right. It is stereo routing rather than world-positioned VR audio. Windows
default audio output is used separately from the game's FMOD mixer.

## Native VR seat prototype

Seat mode adds up/down and forward/back using the existing movement assignments:
W/S up/down and A/D forward/back by default. Select seat directly (unassigned
by default) or cycle after the right mirror. Hold/toggle behavior is shared.
Seat servo and click use both channels; the optional spoken cue says "Seat adjust".
Voice remains off by default, servo and click remain on.

Seat defaults and limits are read automatically from the game's native vehicle
and interior-camera data. No F4 adjustment or calibration is required. If those
data are unavailable while a vehicle loads, seat mode is temporarily skipped.
Pause/focus loss exits adjustment. World unload clears cached defaults, and
selection reads the current vehicle's defaults again.
The existing reset binding restores vertical/depth defaults in seat mode.
Lateral position, tilt, rotation and FOV are outside this prototype's controls.

This calls the game's native seat apply routine using private menu values;
native camera/vehicle writes and refresh execute normally. A scoped assembly
hook bypasses only the UI tail for that private object. Live F4 diagnostics
confirmed changes in vehicle settings; native hotkey movement was tested in VR with the earlier captured-defaults build.
Automatic initialization and persistence across save/reload still need live validation. Disable Seat Diagnostics first,
because its hook overlaps the integrated seat observer.

## Build and test

Requires Visual Studio 2022 C++ build tools (including MASM), Windows SDK and CMake.
All required SPF headers and nlohmann/json are included; no dependency download is
required during build.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The installable DLL and audio files are staged under
`build/package/SPF_MirrorControls`. Build steps do not modify the game installation.

## Install

Close ATS. Copy the staged `SPF_MirrorControls` folder into
`bin/win_x64/plugins/spfPlugins/`. Preserve the installed `config` folder when
updating, then enable the plugin in SPF's plugin manager. Keep the earlier
`SPF_MirrorCenter` and `SPF_MirrorDiagnostics` plugins disabled: hook targets overlap.

## Implementation and verification

Native selector and movement branches are intercepted only within a scoped call
for the captured context. Native angle calculation, rotation setter and mirror
camera refresh remain intact. Calls preserve context/delta arguments and forward
once. Exact executable hash and unique instruction signatures gate registration.

Regression tests exercise the real assembly stubs, slot/input/flags guards,
hold/cycle transitions, persisted assigned and empty binding lists, speech cue
selection/mute, servo movement cancellation and stereo channel routing.
The user confirmed mirror movement, center selection, persistence, stereo click and servo
playback in-game. Broader vehicle/game-version compatibility remains untested.

## Third-party material

- SPF API 1.2.5 headers from TrackAndTruckDevs/SPF_ConsoleCommandHotkeys's official
  release: Apache-2.0; see `licenses/SPF-API-Apache-2.0.txt`.
- nlohmann/json 3.11.3: MIT; see `licenses/nlohmann-json-MIT.txt` and its header.
- Spoken WAV cues were generated locally with Microsoft David Desktop.
- Servo and selection-click WAVs are original synthesized audio; no game audio or executable is included.

The repository includes source and small audio assets. Compiled binaries belong
in release packages rather than source control. Source redistribution licensing
should follow the destination repository's chosen license.

