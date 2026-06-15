# PUBGAssistant C++ Port

This is a parallel C++ port of the Python PUBGAssistance project.

The project is intentionally created next to the original Python project so the
Python version can stay untouched while the C++ version is tested and tuned on
Windows.

## Toolchain

- Visual Studio 2022
- MSVC x64
- CMake 3.24+
- vcpkg
- Qt 6 Widgets

## Visual Studio Setup

Use a short ASCII path with no spaces for this project, for example:

```text
C:\Projects\PUBGAssistant-cpp
```

Avoid building from OneDrive, Chinese paths, or paths containing spaces. Some
third-party vcpkg build scripts used by Qt/OpenCV are sensitive to those paths.
The included presets keep build output, vcpkg downloads, and temporary files
inside this project under `.build/`, `.vcpkg-downloads/`, and `.tmp/`.

This project is intended to be opened as a CMake folder in Visual Studio:

1. Open Visual Studio.
2. Choose `File > Open > Folder...`.
3. Select this project folder.
4. Select the `Windows x64 Debug` CMake preset.
5. Build and run the `PUBGAssistantCpp` target.

Do not use the initial `.slnx/.vcxproj` as the primary entry point yet. It was
created before the source tree and does not describe the Qt/CMake build. Opening
the folder lets Visual Studio read `CMakePresets.json`, run vcpkg manifest mode,
generate Qt moc files, and copy `assets/`, `config/`, and `icon.ico` beside the
executable after build.

The repository includes `vcpkg.json` and `CMakePresets.json`, so Visual Studio
can use manifest-mode vcpkg to restore these dependencies automatically:

- `opencv4` with only the image-processing/image-loading features this project
  currently needs
- `nlohmann-json`
- `qtbase` with GUI/Widgets

The preset also sets `HTTP_PROXY` and `HTTPS_PROXY` to `http://127.0.0.1:7890`
because the current Windows proxy detected by vcpkg is a local HTTP proxy.
Change or remove those values in `CMakePresets.json` if your proxy uses another
port or if you do not use a proxy.

If vcpkg fails while downloading MSYS2 packages with `curl operation failed with
error code 35 (SSL connect error)`, the project files are configured correctly,
but the local proxy/mirror connection needs to be fixed before dependency
restore can finish. Re-run CMake configuration after the proxy is working.

## Manual Dependencies

```bat
vcpkg install --triplet x64-windows --x-manifest-root=.
```

## Manual Build

```bat
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
```

You can also open this folder directly in Visual Studio 2022.

## Runtime Files

The application expects these files next to the executable or in the project
root during debugging:

- `config/config.json`
- `assets/templates`

The initial port keeps the Python data layout so template names and
configuration fields stay compatible.

## Current Scope

The C++ project now includes the Qt main panel, region calibration, map point
assistant, large-map `F4` measurement flow, throwable auto-release, C4 prompts,
SR breath tracking, and the main debugger/calibrator windows. Final timing and
visual parity should be checked on Windows in the actual game environment.
