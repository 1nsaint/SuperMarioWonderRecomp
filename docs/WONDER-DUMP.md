# The dump this port is built and tested on

The recompiled code, the translated shaders and every offset in this repository are for **one build of the game**. A dump of
another version will not work. This page names the one used, so you can check yours. No game file, key or firmware is in the
repository; you need your own dump (from your own cartridge) and your own `prod.keys`.

| | |
|---|---|
| Game | Super Mario Bros. Wonder (Nintendo Switch) |
| Title ID | `010015100B514000` |
| Version | **1.0.0** (base game, as dumped from the cartridge) |
| Update | **none**: the dump has no update and none is applied |
| Container | XCI (gamecard image), partitions `update`, `normal`, `secure`; the game is in `secure` (4 files) |
| File name used | `MarioWonder.xci` |
| Size | 3,769,099,776 bytes |
| SHA-256 | `21395a2c0707df3d1fe4f62718b3be3c3a4abafbb53006ab964571b2c90fd426` |
| Program NCA | `60a0e19616078a390aba0ead98ebc113.nca` (section 0 ExeFS 32.7 MiB, section 1 RomFS 3560.8 MiB, section 2 logo 0.1 MiB) |
| Other NCAs in `secure` | `4ac11815eb7d57f31b8e0dae93ef65e1.cnmt.nca` (meta), `92d54e98d47fa3847e99409c92a545e1.nca`, `c319e1a8267c77421f6ecfe92956094c.nca` |

What the importer extracts from it (`android/wonder/importer`, same numbers on a Mac, a Galaxy S23 and a POCO F6):

| Output | Size |
|---|---|
| `analysis/main.bin` | 56,216,616 bytes |
| `analysis/sdk.bin` | 13,949,136 bytes |
| `analysis/main.npdm` | 1,604 bytes |
| `extract/romfs/` | 19,012 files in 166 folders, 3.73 GB |

## Checking your copy

1. The size must be exactly 3,769,099,776 bytes.
2. The SHA-256 must match the one above (`shasum -a 256 MarioWonder.xci` on macOS, `sha256sum` on Linux/Android shell). A different
   hash does not always mean a bad dump (a trimmed or differently dumped copy of the same version can differ), but a copy damaged in
   a transfer will, and the importer will stop with `ExeFS block N fails its check` or a similar hash message.
3. If your dump has a different title version (an update inside the XCI, or a separate update NSP/XCI in the same folder), the
   importer applies the update, `main` changes and the recompiled code no longer matches. Put only the 1.0.0 dump in the folder.

## Not covered

Updates (1.0.1 and later), the DLC, other regions or builds with a different `main`, split XCI only as far as the importer's
synthetic test goes (no real split image was run), NCZ/XCZ (rejected with a message).
