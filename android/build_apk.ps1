# Build the Android APK (NativeActivity, no Gradle): cmake (NDK) -> libhades.so -> aapt2 -> zipalign -> apksigner.
# Usage, in a dev shell (. .\dev.ps1):  android\build_apk.ps1 [-Install]
param([switch]$Install)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$sdk = "C:\Android\sdk"
$ndk = "$sdk\ndk\28.2.13676358"
$bt = "$sdk\build-tools\35.0.0"
$jar = "$sdk\platforms\android-35\android.jar"
$out = "$root\private\apk"
$build = "$root\private\ba"

if (-not (Test-Path "$build\build.ninja")) {
    cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static
}
cmake --build $build --target hades
if ($LASTEXITCODE) { throw "native build failed" }

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\lib\arm64-v8a", "$out\res" | Out-Null
& "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-unneeded -o "$out\lib\arm64-v8a\libhades.so" "$build\libhades.so"

& "$bt\aapt2.exe" compile --dir "$PSScriptRoot\res" -o "$out\res.zip"
& "$bt\aapt2.exe" link -o "$out\base.apk" -I $jar --manifest "$PSScriptRoot\AndroidManifest.xml" "$out\res.zip"
if ($LASTEXITCODE) { throw "aapt2 link failed" }

# Add the native library (stored uncompressed would need page alignment; compressed + extractNativeLibs is fine).
Push-Location $out
Compress-Archive -Path "lib" -DestinationPath "$out\lib.zip" -Force
Pop-Location
Add-Type -AssemblyName System.IO.Compression.FileSystem
$apk = [System.IO.Compression.ZipFile]::Open("$out\base.apk", "Update")
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk, "$out\lib\arm64-v8a\libhades.so", "lib/arm64-v8a/libhades.so", "Optimal") | Out-Null
$apk.Dispose()

& "$bt\zipalign.exe" -f -p 4 "$out\base.apk" "$out\aligned.apk"
& "$bt\apksigner.bat" sign --ks "$env:USERPROFILE\.android\debug.keystore" --ks-pass pass:android --key-pass pass:android `
    --out "$out\hades.apk" "$out\aligned.apk"
if ($LASTEXITCODE) { throw "apksigner failed" }
$size = [math]::Round((Get-Item "$out\hades.apk").Length / 1MB, 1)
Write-Host "APK: $out\hades.apk ($size MB)"

if ($Install) { & "$sdk\platform-tools\adb.exe" install -r "$out\hades.apk" }
