# Sekiro native Android development

This Android Studio project builds an ARM64 NativeActivity with a Vulkan surface. The debug APK can inspect an original PC texture supplied locally from Data5 and enumerate a supported BND4 binder supplied from Data1. Combat, character rendering, world loading and game progression are pending.

## Build on Windows

Open this directory in Android Studio, or run the wrapper from PowerShell. Use JDK 17 or 21 and the installed Android SDK. The project uses Android SDK 35, Build Tools 35.0.0, NDK 28.2.13676358 and CMake 3.22.1. The app targets Android 14 and requires Android 8.0 or newer, ARM64 and Vulkan 1.0.

```powershell
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
.\gradlew.bat :app:assembleDebug
```

The signed debug APK is at `app/build/outputs/apk/debug/app-debug.apk`. The [NDK setup instructions](https://developer.android.com/studio/projects/install-ndk) describe installing the SDK components.

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

For archive metadata alone, use `python tools/archive_probe.py $game`. The probe opens installation files for reading. The APK contains the native code and app metadata. The optional samples are supplied to an installed debug build during testing.

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

## Android smoke test

Use a connected ARM64 Android device. Skip the sample transfer to check the basic Vulkan clear frame. The optional `inspection.tpf` is read from the debug app's private files directory during startup.

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$package = 'dev.sekiro.nativeport'
$out = 'reverse-engineering/reports/generated'
& $adb install -r 'app/build/outputs/apk/debug/app-debug.apk'
foreach ($sample in 'inspection.bnd', 'inspection.tpf') {
    $remote = "/data/local/tmp/sekiro-$sample"
    & $adb push "$out/$sample" $remote
    if ($LASTEXITCODE -ne 0) { throw "Could not send $sample" }
    & $adb shell "cat $remote | run-as $package sh -c 'cat > files/$sample'"
    $copyStatus = $LASTEXITCODE
    & $adb shell rm $remote
    if ($copyStatus -ne 0) { throw "Could not install $sample in the debug app's files directory" }
}
& $adb shell am force-stop $package
& $adb shell am start -n "$package/android.app.NativeActivity"
& $adb logcat -d -s SekiroNative
```

Check for `Decoded 256x256 BC1 texture` and `Vulkan texture frame queued`, confirm the image on screen, and repeat after rotating or backgrounding the activity. The preview requires a Vulkan surface that accepts transfer-destination images in a compatible RGBA or BGRA format. When only the renderer is initialized, the app logs `Vulkan clear frame queued`.

See [port progress](PORT_PROGRESS.md) and [the engine and archive evidence](reverse-engineering/reports/engine-evidence.md).
