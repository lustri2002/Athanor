# Athanor

A desktop converter and compressor for images, audio, video and PDFs, built from
one C++20 / Qt Quick source tree on Windows, macOS and Linux. The full window,
compact window, CLI, queue, quality/target-size/Convert modes, previews, format
guide, cancellation, validation and opt-in deletion use the same implementation
on all three platforms.

## Build

Requirements: CMake 3.24+, a C++20 compiler, Qt 6.8+ (Core, Gui, Widgets, Quick,
QuickControls2, Concurrent, Svg, ShaderTools, Network), MuPDF 1.28+, libavif
1.4.1+, libheif 1.23.4+, libwebp, FFmpeg, ffprobe and avifenc. HEIF requires the
x265 encoder and libde265 decoder. FFmpeg must include libaom-av1, libsvtav1,
libvpx-vp9, libx264/libx264rgb, FFV1, libopus and libmp3lame. Python 3 is only
used by tests and packaging; it is not part of the app's runtime.

### macOS (Apple Silicon or Intel)

```sh
brew install qtbase qtdeclarative qtshadertools qtsvg qtimageformats libavif libheif mupdf ffmpeg-full ninja
export PATH="$(brew --prefix ffmpeg-full)/bin:$PATH"
cmake -S . -B .build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build .build/native -j2
ctest --test-dir .build/native --output-on-failure
python3 tests/ui.py .build/native/native/Athanor.app/Contents/MacOS/Athanor .build/ui
python3 scripts/package.py .build/native .build/packages
```

The package script deploys Qt/QML and codec dylibs with `macdeployqt`, verifies
the bundled conversion tools, signs the local build ad hoc and creates a ZIP.
The maintainer can replace that test signature with Developer ID signing and
notarization before distributing a public release. Build each architecture
with matching Qt and codec libraries; combining arm64 and x64 dependencies is
not supported. CI builds and tests both architectures independently.

### Linux

Use Qt 6.8+ and the development packages for libaom, dav1d, libde265, x265,
libwebp, libpng and libjpeg. Install FFmpeg with the encoders listed above.
`scripts/build-linux-codecs.sh` builds the required MuPDF, libavif and libheif
versions into `.build/sdk` without modifying the system.

```sh
bash scripts/build-linux-codecs.sh
export PATH="$PWD/.build/sdk/bin:$PATH"
cmake -S . -B .build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.8.3/gcc_64;$PWD/.build/sdk"
cmake --build .build/native -j2
ctest --test-dir .build/native --output-on-failure
xvfb-run -a sh -c 'openbox >/tmp/athanor-openbox.log 2>&1 & exec python3 tests/ui.py .build/native/native/Athanor .build/ui'
xvfb-run -a sh -c 'openbox >/tmp/athanor-openbox.log 2>&1 & exec python3 scripts/package.py .build/native .build/packages'
```

The portable TAR uses linuxdeploy and its Qt plugin to bundle runtime
dependencies. Extract `Athanor` and run `Athanor/usr/bin/Athanor`. It contains a
`.athanor-portable` marker for safe in-app updates. Regular CMake installations
also work, use system conversion tools and leave upgrades to the package manager.
The CI baseline is Ubuntu 24.04 x64; Linux distributions with an older glibc
must build from source on their own baseline.
For headless UI tests, install Xvfb and Openbox so window-scoped keyboard
shortcuts are exercised in an active desktop window. On a normal desktop,
run `tests/ui.py` directly.

### Windows

The existing MSVC codec SDK and portable launcher remain supported:

```powershell
./build.ps1 -QtRoot C:/Qt/6.8.3/msvc2022_64 -CodecRoot C:/codecs -RuntimeRoot C:/runtime
./pack-portable.ps1 -Runtime ../outputs/Athanor
```

An alternative reproducible build uses MSYS2 UCRT64 packages (the CI workflow
lists the complete package set):

```sh
cmake -S . -B .build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/native -j2
ATHANOR_TOOLS_DIR="$(cygpath -m "$MINGW_PREFIX/bin")" ctest --test-dir .build/native --output-on-failure
ATHANOR_TOOLS_DIR="$(cygpath -m "$MINGW_PREFIX/bin")" python tests/ui.py .build/native/native/Athanor.exe .build/ui
python scripts/package.py .build/native .build/packages
```

`pack-portable.ps1` can wrap that deployed runtime in the original single-file
EXE; run it in an MSVC Developer PowerShell with 7-Zip installed.

## Platform integration and storage

| Function | Windows | macOS | Linux |
|---|---|---|---|
| File manager actions | Explorer submenu (HKCU) | Finder Quick Actions / Services | KDE service menus; Nautilus and Nemo Scripts |
| Open media from the file manager | CLI/Explorer | Document-open events / Finder | Desktop entry / CLI |
| Settings | Original portable/unpacked locations | Qt application config location | XDG application config location |
| Temporary decoding and previews | System temporary directory | System temporary directory | System temporary directory |
| Output publication | MoveFileEx without replacement | renamex_np with RENAME_EXCL | renameat2 with RENAME_NOREPLACE |
| Automatic updates | Existing verified single-file EXE | Verified app ZIP, backup and restart | Verified portable TAR, backup and restart |
| System appearance / motion | Windows preferences | Qt appearance / AppKit Reduce Motion | Qt appearance / GNOME or KDE motion |

File manager actions are installed for the current user when enabled in
Settings. Disabling the option removes only Athanor-generated files; existing
files with different ownership markers are preserved and reported. Finder may
require enabling a newly installed action in System Settings > Extensions.
Linux exposes actions through the host file manager's native Scripts or service
menu instead of a Windows registry submenu.

`ATHANOR_SETTINGS_DIR` overrides the settings location (and retains the existing
portable launcher contract). `ATHANOR_TOOLS_DIR` selects a complete tool folder
for development or tests. Assets are embedded, so moving an installed app does
not break icons. Worker scratch directories remain beside the destination to
keep publication atomic and cancellation cleanup unchanged.

Updates accept only matching OS/architecture assets from the upstream GitHub
release, with exact size and SHA-256 verification. macOS/Linux helpers recheck
the archive, validate its layout, launch the replacement runtime before touching
the installation, wait for the parent process, keep a rollback backup and
restart. App/package folders must be writable. Errors are shown on the next
launch. Settings and originals live outside the replaced runtime. No root/admin
elevation is attempted.

## Validation

CI compiles the same target on Windows x64, Linux x64, macOS arm64 and macOS x64.
Generated fixtures cover every output format, animation and alpha preservation,
lossless modes, PDF structure and page export, target-size modes, Unicode paths,
file collisions, deletion after validation, failure retention, worker
cancellation, previews and queue reuse. The original UI harness checks the full
window and all compact categories and produces reviewable screenshots.
Packaging repeats the conversion and native UI checks with developer library
paths removed, and tests macOS/Linux updates against complete relocated app
trees, including bundled Qt symlinks and codec libraries.

`ATHANOR_TEST=1` prevents tests from installing real user integrations or making
automatic GitHub requests. Platform tests put file manager actions in temporary
folders. No private images or external fixture folders are required.

## Dependencies and licenses

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Keep the corresponding
dependency sources, build configuration and license notices with public release
artifacts. The original managed Windows SDK can continue to use its documented
codec versions. Native package managers may supply newer compatible versions;
the macOS package includes the Homebrew dependency environment and formula
provenance, and packages retain dependency license notices.
