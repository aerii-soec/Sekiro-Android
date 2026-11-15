# Sekiro native Android development

This Android Studio project builds an ARM64 NativeActivity with native Vulkan texture and static-mesh previews. The original Data5 texture has been displayed on an Android phone. The FLVER2 path has been tested with a host fixture. An original model, character animation and gameplay remain pending.

## Build on Windows

Open this directory in Android Studio, or run the wrapper from PowerShell. Use JDK 17 or 21 and the installed Android SDK. The project uses Android SDK 35, Build Tools 35.0.0, NDK 28.2.13676358 and CMake 3.22.1. The app targets Android 14 and requires Android 8.0 or newer, ARM64 and Vulkan 1.0.

```powershell
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
.\gradlew.bat :app:assembleDebug
```

The signed debug APK is at `app/build/outputs/apk/debug/app-debug.apk`. Gradle compiles and packages the texture and model shaders in `app/src/main/shaders`. [NDK setup](https://developer.android.com/studio/projects/install-ndk) · [Android shader compilers](https://developer.android.com/ndk/guides/graphics/shader-compilers)

### Vulkan validation

The delivered debug APK includes Khronos Validation Layers 1.4.341.0. To reproduce it, use the prepared local directory or place the official ARM64 library under `jniLibs/arm64-v8a/` and its license under `notices/`. The Android release ZIP SHA-256 is `b47aa5c4a65d9787ef051276587f50cfd70dcd822a8c1b7d4d4a92e28e993459`. [Khronos Android binaries](https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/tag/vulkan-sdk-1.4.341.0)

```powershell
$validation = Resolve-Path .toolchain/vulkan-validation-1.4.341.0
.\gradlew.bat :app:assembleDebug "-PvulkanValidationDir=$validation"
```

Debug builds log whether the Vulkan loader exposes the Khronos validation layer. When the loader provides `VK_EXT_debug_utils`, warning and error callbacks use the `SekiroVulkan` Logcat tag. [Android Vulkan validation](https://developer.android.com/ndk/guides/graphics/validation-layer)

## Reproduce the analysis

Use JDK 25 for Ghidra 12.2. The script opens the existing project in read-only mode and defaults to the project in the parent directory.

```powershell
$env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot'
.\analyze.ps1 -Ghidra 'D:\gems\ghidra_12.2_PUBLIC'
```

Use Python 3.10 or newer to inventory the installation. Add the path to the independent [ooz 7.0 Windows CLI](https://github.com/powzix/ooz/releases/tag/v7.0) to decode and stage the two known plaintext samples. The output folder is local and ignored by Git.

```powershell
$game = 'D:\gems\Sekiro.Shadows.Die.Twice.v1.06\Sekiro.Shadows.Die.Twice.v1.06'
$ooz = '.toolchain\ooz-7.0\64bit\ooz.exe'
$out = 'reverse-engineering/reports/generated'
New-Item -ItemType Directory -Force $out | Out-Null
python tools/archive_probe.py $game --ooz $ooz --stage-samples > "$out/installation.json"
```

For archive metadata alone, use `python tools/archive_probe.py $game`. The probe opens installation files for reading. The APK packages the preview shaders and code. The game-derived `inspection.tpf` is a separate local file. It comes from Data5 BDT offset `0x3ef830` and has SHA-256 `342a3a19043f673cf4800eb002f5113ff916fd46e0444243861878791926d3bc`.

## Preview a local model

Press Android Back in the preview to open the asset menu. Choose **Open FLVER** and select a local little-endian Unicode Sekiro FLVER2 export. The importer also accepts the existing supported BND4 layout when it contains an uncompressed FLVER member. It previews indexed base-LOD geometry, positions, normals and UV0. The log reports the decoded model and the number of surfaces with a matching texture.

Import a single-texture PC TPF containing a BC1 sRGB DDS through **Open TPF**. A mesh surface uses that texture when its diffuse reference and the TPF entry have the same basename. Other surfaces use neutral inspection lighting. **Show texture** and **Show model** switch the preview when both files are present. Support is limited to the implemented geometry layouts and first supported diffuse slot. The preview does not play animations.

## Host verification

```powershell
python -m unittest discover -s tests -v
$out = 'reverse-engineering/reports/generated'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($format in 'bnd4', 'tpf') {
    wsl.exe --exec g++ -std=c++20 -Wall -Wextra -Werror '-fsanitize=address,undefined' `
        -I app/src/main/cpp "app/src/main/cpp/assets/$format.cpp" "tests/test_$format.cpp" `
        -o "$out/test_$format"
    if ($LASTEXITCODE -ne 0) { throw "Could not compile $format host test" }
    wsl.exe --exec "./$out/test_$format"
    if ($LASTEXITCODE -ne 0) { throw "$format host test failed" }
}
wsl.exe --exec "./$out/test_bnd4" "$out/inspection.bnd"
wsl.exe --exec "./$out/test_tpf" "$out/inspection.tpf"
```

The offscreen Vulkan test reads back the original Data5 texture and uses a labeled FLVER fixture to check indexed geometry, mesh depth, UV/material binding and the cutout-safe viewport. It uses the production C++ draw code and shaders extracted from the APK.

```powershell
$out = 'reverse-engineering/reports/generated'
$hostOut = "$out/host-preview"
python -m zipfile -e 'app/build/outputs/apk/debug/app-debug.apk' $hostOut
$headers = (wsl.exe --exec wslpath -a "$env:ANDROID_HOME/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include").Trim()
wsl.exe --exec g++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers `
    -I app/src/main/cpp -idirafter $headers app/src/main/cpp/assets/tpf.cpp app/src/main/cpp/assets/flver.cpp `
    app/src/main/cpp/preview_texture.cpp app/src/main/cpp/mesh_preview.cpp `
    tests/test_vulkan_preview.cpp '-l:libvulkan.so.1' `
    -o "$hostOut/test_vulkan_preview"
if ($LASTEXITCODE -ne 0) { throw 'Could not compile Vulkan host test' }
wsl.exe --exec "$hostOut/test_vulkan_preview" "$out/inspection.tpf" `
    "$hostOut/assets/shaders/preview.vert.spv" "$hostOut/assets/shaders/preview.frag.spv" `
    "$hostOut/assets/shaders/model.vert.spv" "$hostOut/assets/shaders/model.frag.spv"
```

## Android smoke test

1. Update the APK and copy the staged `inspection.tpf` to the phone's Downloads folder.
2. Launch the app, tap **Open TPF**, and select the sample. If the previous build already imported it, the update can reuse it. Expect a square texture centered within the landscape safe area. A square texture leaves side margins on a wider screen.
3. Background and reopen the app. Rotate to the other landscape orientation and check that the texture stays visible. An edge swipe should reveal the system bars temporarily.

In v0.3.1, Android Back shows **Sekiro asset preview · 0.3.1**. Choose **Retry fullscreen** if a bar remains visible after the preview opens.

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb install -r 'app/build/outputs/apk/debug/app-debug.apk'
& $adb push 'reverse-engineering/reports/generated/inspection.tpf' '/sdcard/Download/inspection.tpf'
& $adb shell am start -n 'dev.sekiro.nativeport/.PreviewActivity'
& $adb logcat -d -s SekiroNative SekiroVulkan
```

Look for `Window insets: status visible=false, navigation visible=false`, `Loaded private inspection.tpf` and `Vulkan queued sampled texture frame`. For a local model, look for `FLVER geometry uploaded` and `Vulkan queued indexed FLVER frame`. A queue message records Vulkan accepting the presentation request. Check the phone screen for the visible result. [Android document picker](https://developer.android.com/training/data-storage/shared/documents-files) · [Immersive mode](https://developer.android.com/develop/ui/views/layout/immersive)

See [port progress](PORT_PROGRESS.md) and [the engine and archive evidence](reverse-engineering/reports/engine-evidence.md).
