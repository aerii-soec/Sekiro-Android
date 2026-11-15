# Engine and archive evidence

## Executable provenance

The input is the separate installation at `D:\gems\Sekiro.Shadows.Die.Twice.v1.06\Sekiro.Shadows.Die.Twice.v1.06`. The version resource reports 1.6.0.0. Ghidra's saved import and the installation's `sekiro.exe` have the same SHA-256, `23e174f006a79b56bc993f177109f75695be73832694465c04fefe94ececdf90`. The executable is x86-64 PE, with image base `0x140000000` and entry address `0x14235a28c`.

The directory includes `sekiro_coop.dll` (Sekiro Together 1.0.9-beta), `dinput8.dll`, Steam emulation configuration files, and multiple Steam API binaries. The 1,002,496-byte active `steam_api64.dll` differs from both 119,720-byte alternate copies, which are identical to each other. The alternate `sekiro.rne` has a different size, SHA-256 and PE entry RVA. The supplied executable's clean Steam provenance remains unverified. The report applies to the specific Ghidra import, and a clean authorized 1.06 executable is needed for comparison before attributing startup or gameplay behavior to stock Sekiro.

The toolchain uses Ghidra 12.2 PUBLIC and JDK 25 for exports. Android work uses the installed SDK, Gradle 8.11.1, NDK 28.2.13676358 and CMake 3.22.1 and Ninja 1.10.2. Discovery found about 17.6 GB free on D: and 15.4 GiB total visible RAM.

## Targeted Ghidra findings

| Area | Evidence | Confidence |
| --- | --- | --- |
| Process entry | `0x14235a28c` invokes `0x14235a82c` and `0x14235a120`. The first routine initializes a process security cookie. The second calls `0x1411e42c0`, which parses the wide command line and calls `0x1411e40d0` after application setup. | High for the call chain and cookie. |
| Lifecycle | `0x1411e40d0` processes work with repeated calls to `0x1411b3ee0` and performs a bounded shutdown progression. Its relationship to simulation ticks and presentation requires tracing. | Medium for a lifecycle loop. |
| Graphics | `0x141b5e9a0` calls `0x141b69610`, which calls `D3D11CreateDevice` at `0x141b6974f`. It also calls `0x141b6ff70`, which invokes `CreateDXGIFactory` at `0x141b6ffcf`. D3DX11 and D3DCompiler imports give further rendering leads. | High for Win32 device and factory setup. |
| Archive path | `0x1406e1be0` references the UTF-16 `.bdt` suffix at `0x142994360`, appends it after string construction, and forwards both paths to `0x141de3b90`. That routine creates a 200-byte object through `0x141de3840`, prepares the second path, invokes `0x141de3d10` and forwards the object to `0x141a075d0` on success. The byte layout and registry type remain hypotheses. The image contains UTF-16 filenames for all five BHD files. | High for the path reference. Medium for the archive-open handoff. |
| Compression | `0x141de7760`, `0x141de7c70` and `0x141de85b0` call `OodleLZ_Decompress` via `OO2CORE_6_WIN64.DLL`. The relationship between these call sites and specific file entries is still open. | High for the import and call sites. |
| Input and media | The executable imports DirectInput 8 and XInput, Bink playback, and FMOD Ex/Event APIs. The installation includes 77 FSB files, 8 FEV files and 2 BK2 movies. `fmod_event_net64.dll` is present on disk. The direct import map has no reference to that DLL. | High for inventory and observed imports. |

### FUN_14001054d

The function processes 16-byte blocks, XORs block data into a running checksum and uses `BSF` to index 16-byte state values at selected block numbers. It calls `0x1400108e0` for batches of six, `0x140010a80` for smaller groups, and `0x140010ba0` on single-block paths. The helpers execute `AESDEC` and `AESDECLAST`.

The combination is a high-confidence match to the structure of AES-NI OCB bulk decryption. OpenSSL's 1.1.1w assembly contains the same combination of trailing-zero indexes, a six-block decrypt helper and a running checksum, consistent with RFC 7253. This is a structural comparison. Ghidra recorded one incoming DATA reference at `0x143f49630` and found no direct caller, so the linked library version and the game's use of the routine remain unverified. [OpenSSL AES-NI implementation](https://raw.githubusercontent.com/openssl/openssl/OpenSSL_1_1_1w/crypto/aes/asm/aesni-x86_64.pl) · [RFC 7253](https://www.rfc-editor.org/rfc/rfc7253.html)

### Local exports

The Ghidra run generated 185,439 function rows, 8,795 import/reference rows across 534 unique library-symbol pairs, 149 direct call edges for selected roots and 30 incoming root references. These are Ghidra's current analysis results. Import and reference rows include thunks and data references. The generated directory contains mnemonics and labeled Ghidra pseudocode for the selected functions, plus the TSVs and log. [Ghidra Headless Analyzer](https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/RuntimeScripts/support/analyzeHeadlessREADME.md)

## Data1–Data5

The BHD fingerprint and paired BDT inventory used read-only file handles. None of the five BHD prefixes starts with plaintext `BHD5`. The BDT counts describe validated DCX/KRAK header candidates in the first 8 MiB of each file. A zero count leaves the remainder and other payload formats unclassified. [BHD5 format implementation](https://github.com/JKAnderson/SoulsFormats/blob/master/SoulsFormats/Formats/BHD5.cs) · [DCX format implementation](https://github.com/JKAnderson/SoulsFormats/blob/master/SoulsFormats/Formats/DCX.cs)

| Pair | BHD bytes | BDT bytes | KRAK candidates in prefix |
| --- | ---: | ---: | ---: |
| Data1 | 137,728 | 2,755,237,627 | 2 |
| Data2 | 32,256 | 465,298,672 | 0 |
| Data3 | 86,272 | 1,643,835,952 | 0 |
| Data4 | 270,848 | 541,402,628 | 0 |
| Data5 | 1,035,520 | 6,830,280,950 | 28 |

- The payload at Data1 BDT offset zero decoded with ooz from 5,927 compressed bytes to a 19,260-byte BND4 containing `a0000.hkx` (11,960 bytes) and `s89_00_0099.mqb` (6,812 bytes). The C++ binder parser checked the two byte ranges on the decoded file.
- The payload at Data5 BDT offset `0x3ef830` decoded with ooz from 30,136 compressed bytes to a 43,964-byte PC TPF. Its single DDS uses BC1 sRGB (DXGI format 72). The native CPU decoder produced a 256×256 image from the first mip. [TPF template](https://github.com/JKAnderson/SoulsTemplates/blob/master/TPF.bt) · [Microsoft BC1 specification](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression)
- Some other Data5 candidate payloads failed to decode with ooz 7.0 during sampling. Header validity independently establishes the envelope. Archive coverage, member encryption, filename mapping and other codecs remain open.
- Use of the standalone [ooz CLI](https://github.com/powzix/ooz) is local to the host probe. The tested release ZIP SHA-256 was `b39fdb41848970ec9e471677817f5d49fe0222472b103dcf92c4babb3a0c50c6`. The APK contains no game files or Windows middleware.

## Reuse candidates

The MIT/Apache-2.0 [sekiro-rs project](https://github.com/AKJama/sekiro-rs) publishes a Bevy reimplementation and documents its asset and combat work. Its build and gameplay claims were not verified here. Its documented full setup requests about 50 GB for generated data, exceeding the available workspace space at discovery. Evaluate individual readers and test vectors against the native project's small asset samples before bringing in code. [sekiro-rs setup and architecture](https://github.com/AKJama/sekiro-rs/blob/main/docs/GETTING-STARTED.md)
