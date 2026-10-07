#!/bin/sh
# Builds Athanor.app on macOS. Requires: brew install cmake ninja qt libheif libavif mupdf webp ffmpeg
set -e
cd "$(dirname "$0")"
PREFIX="$(brew --prefix)"
cmake -S . -B .build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$PREFIX"
cmake --build .build
APP=.build/native/Athanor.app
mkdir -p "$APP/Contents/Resources"
cp -R assets "$APP/Contents/Resources/"
"$(brew --prefix qt)/bin/macdeployqt" "$APP" -qmldir=native -libpath="$PREFIX/lib"
codesign --force --deep --sign - "$APP"
echo "Built $APP (ffmpeg/ffprobe are taken from Homebrew unless placed in Contents/Resources/tools)"
