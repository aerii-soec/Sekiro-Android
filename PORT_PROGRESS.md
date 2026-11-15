# Port progress

## Fullscreen follow-up

- v0.3.1 requests fullscreen after the activity window attaches, regains focus, or receives control of the system bars. It checks the reported visibility once and retries with compatibility flags if needed. Android Back opens a versioned asset menu with **Retry fullscreen**.
- The Android 15 x86-64 emulator rendered the original texture and reported both bars hidden after launch, manual retry, and background/resume. Both gesture and three-button navigation passed. v0.3.0 also hid both bars on this emulator, so the reported phone behavior was not reproduced.
- The signed ARM64 v0.3.1 APK passed signature and 16 KiB alignment checks. Running ARM64 through the emulator's x86 translation aborted in the Vulkan debug callback bridge. The fullscreen runtime checks used the same source built for x86-64. A physical ARM64 phone retest remains open.

## Fullscreen and static model preview

- The user verified v0.2.0 installation, landscape, and visible original Data5 texture on an Android phone. Status and navigation bars remained visible in that build.
- Version 0.3.0 requests immersive control again on resume, focus, configuration change and when Android grants control of the system-bar insets. The swapchain uses the Android surface extent. Native viewports account for cutout safe insets, and both previews preserve aspect ratio.
- Added a bounded little-endian Sekiro FLVER2 reader for supported base-LOD geometry and linked BND4 members, including multiple vertex streams, positions, normals, UV0, indexed faces, material references and bone parents. The Vulkan preview uses device-local vertex/index buffers, a perspective camera and depth. It can bind one supported imported diffuse texture by matching its original basename.
- Host Vulkan readback tested the original Data5 texture in UNORM and sRGB. A synthetic FLVER2 fixture exercised multi-stream parsing, strip restart and winding, bone hierarchy validation, one material match, depth occlusion and a cutout-safe viewport. No complete original FLVER was recovered from the local read-only archive scan. Original 3D geometry, textures and on-device behavior of v0.3.0 remain unverified.
- The signed ARM64 v0.3.0 APK passed signature and 16 KiB alignment checks. The four Python tests and sanitized BND4 and TPF tests passed. The host Vulkan run used Khronos validation with synchronization checks and logged no validation warnings or errors. ADB found no connected device or configured local AVD for this build.
- Inspected the original Data1 Havok 2016 cutscene tagfile and recovered skeleton-parent and animation-binding metadata. It is a single-bone camera and marker scene. A matching character mesh, rig and idle clip for GPU skinning validation are still needed. [Asset and engine evidence](reverse-engineering/reports/engine-evidence.md#model-and-animation-follow-up)

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

1. On Vulkan-capable ARM64 hardware, verify that v0.3.0 hides both bars and that the Data5 texture stays aspect-correct after rotation, system gestures and backgrounding. Use the [smoke test instructions](README.md#android-smoke-test).
2. Compare an authorized clean Steam 1.06 executable with the recorded Ghidra input before treating engine behavior as stock. Trace the archive object initialization at `0x141de3d10` and its registration at `0x141a075d0`.
3. Import an authorized original FLVER2 export and its matching texture. Verify geometry, UV mapping, orientation and scale against a reference viewer, then validate the matching character skeleton and idle clip.

The debug APK is at `app/build/outputs/apk/debug/app-debug.apk`. Generated local evidence and staged samples are under `reverse-engineering/reports/generated`.
