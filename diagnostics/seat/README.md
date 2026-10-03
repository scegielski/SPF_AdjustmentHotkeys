# Native seat diagnostics

Read-only observer of the ATS 1.61.3.1s seat adjustment menu apply routine.
It forwards the original context once, observes menu values and the vehicle
adjustment settings before/after, and changes no controls or game data.
Exact executable hash and a unique native function signature gate the hook.
The routine writes vehicle adjustment deltas and calls native refresh;
static inspection does not establish save persistence or live axis mapping.

Build separately from the main plugin:

    cmake -S diagnostics/seat -B build-seat -G "Visual Studio 17 2022" -A x64
    cmake --build build-seat --config Release
    ctest --test-dir build-seat -C Release --output-on-failure

With ATS closed, install SPF_SeatDiagnostics/SPF_SeatDiagnostics.dll under
bin/win_x64/plugins/spfPlugins/. Enable SPF_SeatDiagnostics in Plugin Manager.
The existing Mirror Controls plugin can stay enabled; their hooks differ.
Load the Mustang. Use F4 seat adjustment, move one axis at a time with a short
pause between: forward/backward, up/down, left/right. Then test tilt, rotation,
and FOV separately. Do not use the old camera-only seat hotkeys in this test.
Close the menu, then check the SeatDiagnostics SPF log for native observer
installed/enabled and records with before_valid=1 and after_valid=1.
The test sequence identifies category/axis mapping before hotkey integration.
Disable this diagnostic after capturing the test.
