# Porting Status

This file is the final static parity check against the Python `main.py` control
flow. It separates completed migration from features that currently have a C++
implementation but still differ from the Python runtime behavior.

## Migrated

- CMake/MSVC project structure.
- `config.json` copied to `config/config.json`.
- `templates/` copied to `assets/templates/`.
- Resource path handling and JSON configuration loading.
- Real region and scale lookup.
- GDI screen capture.
- Win32 transparent topmost overlay foundation.
- Qt Widgets main control panel with the original 280x372 frameless translucent
  four-tab layout.
- Main UI tabs for map points, assistant toggles, region calibration, and hotkey
  settings.
- Region calibration overlay, forced-square regions, 1km scale calibration, and
  all-region debug overlay.
- Weapon, equipment, and gesture template detection using OpenCV C++.
- Minimap marker detection and distance conversion.
- Elevation marker detection.
- Recoil control loop with `SendInput`.
- SR breath-control scope-edge tracker using calibrated 4x/6x/8x top-edge
  regions and the main Python timing/threshold settings.
- Rocket, VSS, crossbow, and mortar HUD calculations.
- Map point assistant controls for map/category/size/color-blind mode.
- Map point assistant Y-axis mapping and legend.
- Large-map measurement after `F4` and the next left click, using marker
  alpha-template matching against the color mask.
- Throwable auto-cook/auto-release, pull-ring key press, distance guard, and
  jump-throw timing.
- C4 4s install timer, long-right-click cancel, explosion margin, countdown,
  start prompt, and jump prompt state machine.
- Persistent lower-left status HUD with marker indicator, core switch states,
  equipment status, weapon slot summaries, current weapon, and stance.
- `Home` show/hide, global left/right tab switching, `N` display toggle, and
  Alt-aware right-click map-point closing.
- Legacy default hotkey migration and configurable Ctrl/Shift/Alt hotkey
  triggering/recording.
- Window icon loading, native Win32 rounded region for the main panel, and
  explicit shutdown callback from the Qt close event.
- Recoil debugger window for weapon curves and attachment multiplier curves.
- Special weapon debugger window for rocket/VSS/crossbow/throwables curves and
  mortar/C4 parameters.

## Partially Migrated

- C4 overlay uses C++ GDI text/circle drawing instead of Python's exact Tkinter
  icon rendering, so the visual appearance is close but not pixel-perfect.
- SR breath control now reads the Python settings, but the C++ edge detector is
  still a simplified implementation and must be tuned on Windows.
- The screenshot scaling calibration, recoil debugger, and special weapon
  debugger windows exist, but they are functional Qt replacements rather than
  pixel-perfect/interaction-perfect copies of the Tkinter debugger windows.
- Hotkey recording supports keyboard Ctrl/Shift/Alt combinations and preserves
  Python's single-key restriction for throw/equipment/fire keys. Mouse-button
  capture is still represented as fixed labels rather than recorded interactively.

## Not Yet Migrated

- Pixel-perfect marker icon rendering inside the status/C4/throwable HUDs. The
  current C++ HUD uses text and colored circles rather than alpha-blended PNG
  icons.

## Windows Tuning Still Expected

- Overlay z-order/click-through behavior in the game.
- Screen capture color parity under the user's Windows display settings.
- Final mouse timing for auto-cook, jump throw, C4 prompts, recoil, and SR breath
  control.
- Pixel-perfect comparison of Qt widgets against the original Tkinter rendering.
- Full compile/runtime verification on Windows with Visual Studio, Qt, OpenCV,
  and vcpkg. This macOS environment does not have the needed Windows/Qt build
  stack installed.

## First Windows Test Checklist

1. Install dependencies with vcpkg:

   ```bat
   vcpkg install opencv4:x64-windows nlohmann-json:x64-windows qtbase:x64-windows
   ```

2. Configure and build:

   ```bat
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
     -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
   cmake --build build --config Release
   ```

3. Run `build\Release\PUBGAssistantCpp.exe` from a console.

4. Verify in this order:

   - Program starts and loads templates.
   - Main Qt panel opens, can drag, switch tabs, and close.
   - Press `F2`; minimap/elevation/special overlay appears and distances match Python.
   - Press `F1`; open inventory with `Tab`; equipment logs show both weapons.
   - Hold the active weapon; current weapon logs update.
   - Press `F3`; recoil movement is in the expected direction and magnitude.
   - Press `F4`, click own position on large map, and verify large-map one-shot distances.
   - Test Rocket/VSS/Crossbow/Mortar HUD after marker detection works.
   - Test throw auto-release with the configured throw key.
   - Test C4 install and prompt lifecycle.
   - Open all three debugger/calibrator windows from the panel.

## Notes

This C++ port is meant to be compiled and tuned on Windows. It avoids the old
Python tester modules and historical backup files. The highest-risk areas are
overlay behavior, screenshot color parity, and input timing, because those
depend heavily on the actual Windows/game environment.
