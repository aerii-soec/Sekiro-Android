# Port progress

## Current implementation

- Built an Android Studio project for ARM64 using C++20 and the Android NativeActivity lifecycle.
- Implemented Vulkan surface, device and swapchain setup, on-demand redraw, image synchronization and resize handling. The optional texture preview transfers original BC1 texels decoded to RGBA into the swapchain with nearest-neighbor scaling for inspection.
- Ran the existing Ghidra project in read-only mode. Exported a function database, import references, selected call edges, incoming references, mnemonics and decompilations. Identified startup, Direct3D and archive path leads. [Evidence report](reverse-engineering/reports/engine-evidence.md)
- Built a read-only archive and middleware probe. Decoded real DCX/KRAK samples from Data1 and Data5 using the host ooz CLI. Implemented restricted native BND4 and PC TPF readers for the observed layouts.

## Build and verification

- Gradle `:app:assembleDebug` succeeded with Android SDK 35 and NDK r28c. The APK uses Android's debug signature, declares Vulkan 1.0 and has an AArch64 `libsekiro_native.so` with the NativeActivity entry point.
- `apksigner verify` and `zipalign -c -P 16 -v 4` passed. ELF LOAD segments use 16 KiB alignment. The build contains no game or middleware payloads.
- All four Python archive parser tests passed. The C++ host tests passed under AddressSanitizer and UndefinedBehaviorSanitizer and accepted the decoded original BND4 and 256×256 TPF.
- ADB found no Android device and the local emulator had no configured AVD. Device installation, Vulkan presentation and activity resume/resize behavior remain unverified.
- The APK implements a diagnostic screen. Gameplay, audio, movies, controls, saves, world and character rendering, and complete campaign progression remain unimplemented.

## Next engineering tasks

1. On Vulkan-capable ARM64 hardware, verify installation, the clear frame, the Data5 texture and surface recreation after rotation or backgrounding. Use the existing [smoke test instructions](README.md#android-smoke-test).
2. Compare an authorized clean Steam 1.06 executable with the recorded Ghidra input before treating engine behavior as stock. Trace the archive object initialization at `0x141de3d10` and its registration at `0x141a075d0`.
3. Obtain an authorized FLVER2 mesh and its associated textures. Decode static positions, multi-attribute vertex layout and face sets on the host, then upload vertex and index buffers with a shader-based Vulkan material path.

The debug APK is at `app/build/outputs/apk/debug/app-debug.apk`. Generated local evidence and staged samples are under `reverse-engineering/reports/generated`.
