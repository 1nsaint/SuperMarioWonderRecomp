#!/usr/bin/env bash
# Build a game's Android APK on macOS/Linux (the same steps as build_apk.ps1): cmake (NDK) -> lib<game>.so -> aapt2 ->
# zipalign -> apksigner. Needs the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with NDK 28.2 and
# build-tools 35, Vulkan headers (VULKAN_SDK with Include/, e.g. $(brew --prefix vulkan-headers)), cmake and ninja.
# Usage: android/build_apk.sh [--game <game>] [--install]     (game: a folder under games/, default hades)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
game=hades
install=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --install) install=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
# Hades keeps the layout from before there were other games (private/apk, private/ba).
if [ "$game" = hades ]; then priv="$root/private"; else priv="$root/private/$game"; fi
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ndk="$sdk/ndk/28.2.13676358"
bt="$sdk/build-tools/35.0.0"
jar="$sdk/platforms/android-35/android.jar"
out="$priv/apk"
build="$priv/ba"
host="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"
export VULKAN_SDK="${VULKAN_SDK:-$(brew --prefix vulkan-headers 2>/dev/null || true)}"

if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DNX_GAME="$game"
fi
cmake --build "$build" --target "$game"

rm -rf "$out"
mkdir -p "$out/lib/arm64-v8a"
"$ndk/toolchains/llvm/prebuilt/$host/bin/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/lib$game.so" "$build/lib$game.so"

"$bt/aapt2" compile --dir "$root/android/res" -o "$out/res.zip"
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$root/games/$game/AndroidManifest.xml" "$out/res.zip"
(cd "$out" && zip -q -9 base.apk "lib/arm64-v8a/lib$game.so")
"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android \
    --out "$out/$game.apk" "$out/aligned.apk"
echo "APK: $out/$game.apk ($(du -h "$out/$game.apk" | cut -f1))"

if [ "$install" = 1 ]; then "$sdk/platform-tools/adb" install -r "$out/$game.apk"; fi
