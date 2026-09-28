#!/usr/bin/env bash
# Build the Android APK on macOS/Linux (the same steps as build_apk.ps1): cmake (NDK) -> libhades.so -> aapt2 ->
# zipalign -> apksigner. Needs the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with NDK 28.2 and
# build-tools 35, Vulkan headers (VULKAN_SDK with Include/, e.g. $(brew --prefix vulkan-headers)), cmake and ninja.
# Usage: android/build_apk.sh [--install]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ndk="$sdk/ndk/28.2.13676358"
bt="$sdk/build-tools/35.0.0"
jar="$sdk/platforms/android-35/android.jar"
out="$root/private/apk"
build="$root/private/ba"
host="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"
export VULKAN_SDK="${VULKAN_SDK:-$(brew --prefix vulkan-headers 2>/dev/null || true)}"

if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
fi
cmake --build "$build" --target hades

rm -rf "$out"
mkdir -p "$out/lib/arm64-v8a"
"$ndk/toolchains/llvm/prebuilt/$host/bin/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/libhades.so" "$build/libhades.so"

"$bt/aapt2" compile --dir "$root/android/res" -o "$out/res.zip"
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$root/android/AndroidManifest.xml" "$out/res.zip"
(cd "$out" && zip -q -9 base.apk lib/arm64-v8a/libhades.so)
"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android \
    --out "$out/hades.apk" "$out/aligned.apk"
echo "APK: $out/hades.apk ($(du -h "$out/hades.apk" | cut -f1))"

if [ "${1:-}" = "--install" ]; then "$sdk/platform-tools/adb" install -r "$out/hades.apk"; fi
