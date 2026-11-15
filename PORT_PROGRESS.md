# Port progress

## Android rendering follow-up

The phone test of `cbdb888` confirmed installation, startup and a solid blue-gray surface. The APK contained no game assets. Its clear-only render pass used RGBA `(0.025, 0.07, 0.085, 1)` when private `inspection.tpf` was absent. Even with an asset present, that version copied pixels directly to the swapchain and had no shader sampling pipeline.

- Version 0.2.0 prompts for a local TPF when the texture is absent, and displays a diagnostic when loading or rendering fails. The app validates the file before replacing the saved copy in private storage.
- The preview stages decoded RGBA texels, uploads them into a device-local sampled image, binds a combined image sampler, and draws an aspect-fit quad with packaged SPIR-V. The Android compositor handles display orientation when the swapchain supports identity pre-transform.
- The activity requests sensor landscape and immersive system bars. Logcat covers asset loading, Vulkan initialization, swapchain properties, shader loading, upload, pipeline creation, presentation and validation errors.
- Debug builds succeeded with and without the optional Khronos validation layer. The delivered APK includes the layer and passed signature and 16 KiB alignment checks. The TPF is imported separately.
- Offscreen Vulkan readback using the production shaders extracted from the APK matched the original Data5 texture in both UNORM and sRGB. Each run found 62,676 nonblack texels and verified the side margins. Khronos validation reported no errors or warnings.
- ADB found no connected device and the emulator had no configured local AVD. Import, presentation, immersive bars and rotation still need verification on the phone. Original 3D model implementation is pending that check.

## Initial implementation

- Built an Android Studio project for ARM64 using C++20 and the Android NativeActivity lifecycle.
- Implemented Vulkan surface, device and swapchain setup, on-demand redraw, image synchronization and resize handling.
- Ran the existing Ghidra project in read-only mode. Exported a function database, import references, selected call edges, incoming references, mnemonics and decompilations. Identified startup, Direct3D and archive path leads. [Evidence report](reverse-engineering/reports/engine-evidence.md)
- Built a read-only archive and middleware probe. Decoded real DCX/KRAK samples from Data1 and Data5 using the host ooz CLI. Implemented restricted native BND4 and PC TPF readers for the observed layouts.

## Initial build and verification

- Gradle `:app:assembleDebug` succeeded with Android SDK 35 and NDK r28c. The APK uses Android's debug signature, declares Vulkan 1.0 and has an AArch64 `libsekiro_native.so` with the NativeActivity entry point.
- `apksigner verify` and `zipalign -c -P 16 -v 4` passed. ELF LOAD segments use 16 KiB alignment. The build contains no game or middleware payloads.
- All four Python archive parser tests passed. The C++ host tests passed under AddressSanitizer and UndefinedBehaviorSanitizer and accepted the decoded original BND4 and 256×256 TPF.
- The user verified installation and launch of the initial APK on an Android phone.
- The APK implements a diagnostic screen. Gameplay, audio, movies, controls, saves, world and character rendering, and complete campaign progression remain unimplemented.

## Next engineering tasks

1. On Vulkan-capable ARM64 hardware, verify installation, the clear frame, the Data5 texture and surface recreation after rotation or backgrounding. Use the existing [smoke test instructions](README.md#android-smoke-test).
2. Compare an authorized clean Steam 1.06 executable with the recorded Ghidra input before treating engine behavior as stock. Trace the archive object initialization at `0x141de3d10` and its registration at `0x141a075d0`.
3. Obtain an authorized FLVER2 mesh and its associated textures. Decode static positions, multi-attribute vertex layout and face sets on the host, then upload vertex and index buffers with a shader-based Vulkan material path.

The debug APK is at `app/build/outputs/apk/debug/app-debug.apk`. Generated local evidence and staged samples are under `reverse-engineering/reports/generated`.
