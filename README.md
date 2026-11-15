# Sekiro native Android development

This Android Studio project builds an ARM64 NativeActivity with a Vulkan preview of a supported texture supplied locally from Data5. It also reads the observed Data1 BND4 binder layout. 3D models, gameplay and game progression are pending.

## Build on Windows

Open this directory in Android Studio, or run the wrapper from PowerShell. Use JDK 17 or 21 and the installed Android SDK. The project uses Android SDK 35, Build Tools 35.0.0, NDK 28.2.13676358 and CMake 3.22.1. The app targets Android 14 and requires Android 8.0 or newer, ARM64 and Vulkan 1.0.

```powershell
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
.\gradlew.bat :app:assembleDebug
```

The signed debug APK is at `app/build/outputs/apk/debug/app-debug.apk`. Gradle compiles the GLSL in `app/src/main/shaders` to SPIR-V and packages both shaders. [NDK setup](https://developer.android.com/studio/projects/install-ndk) · [Android shader compilers](https://developer.android.com/ndk/guides/graphics/shader-compilers)

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

The offscreen Vulkan test loads the shaders extracted from the APK, executes the production texture upload and draw code, and reads back every pixel. It compares both UNORM and sRGB outputs against the original decoded Data5 texels and the expected side margins.

```powershell
$out = 'reverse-engineering/reports/generated'
$hostOut = "$out/host-preview"
python -m zipfile -e 'app/build/outputs/apk/debug/app-debug.apk' $hostOut
$headers = (wsl.exe --exec wslpath -a "$env:ANDROID_HOME/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include").Trim()
wsl.exe --exec g++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers `
    -I app/src/main/cpp -idirafter $headers app/src/main/cpp/assets/tpf.cpp `
    app/src/main/cpp/preview_texture.cpp tests/test_vulkan_preview.cpp '-l:libvulkan.so.1' `
    -o "$hostOut/test_vulkan_preview"
if ($LASTEXITCODE -ne 0) { throw 'Could not compile Vulkan host test' }
wsl.exe --exec "$hostOut/test_vulkan_preview" "$out/inspection.tpf" `
    "$hostOut/assets/shaders/preview.vert.spv" "$hostOut/assets/shaders/preview.frag.spv"
```

## Android smoke test

1. Update the APK and copy the staged `inspection.tpf` to the phone's Downloads folder.
2. Launch the app, tap **Open TPF file**, and select the sample. The app validates the file and reloads the preview. Expect a square texture centered in landscape.
3. Background and reopen the app. Rotate to the other landscape orientation and check that the texture stays visible. An edge swipe should reveal the system bars temporarily.

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb install -r 'app/build/outputs/apk/debug/app-debug.apk'
& $adb push 'reverse-engineering/reports/generated/inspection.tpf' '/sdcard/Download/inspection.tpf'
& $adb shell am start -n 'dev.sekiro.nativeport/.PreviewActivity'
& $adb logcat -d -s SekiroNative SekiroVulkan
```

Look for `Loaded private inspection.tpf`, the two SPIR-V shader paths, `Uploaded 256x256 RGBA texture`, and `Vulkan queued sampled texture frame`. The last message records Vulkan accepting the presentation request. Check the screen for the visible result. If it fails, capture both log tags and any `AndroidRuntime` exception. [Android document picker](https://developer.android.com/training/data-storage/shared/documents-files)

See [port progress](PORT_PROGRESS.md) and [the engine and archive evidence](reverse-engineering/reports/engine-evidence.md).
