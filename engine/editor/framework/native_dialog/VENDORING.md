# Vendored: nativefiledialog-extended

- **Upstream:** https://github.com/btzy/nativefiledialog-extended
- **Version:** v1.3.0 (commit `86f742bab39f1b253ad111e8ce776b46dd1ccdbe`)
- **License:** zlib — see [LICENSE](LICENSE), retained verbatim (clause 3).

Vendored **unmodified** into lux-engine on 2026-06-15. Only the library proper is
kept; upstream `test/`, `screens/`, `.github/`, `.clang-format`, and `.gitignore`
were dropped. Everything under `src/` is byte-for-byte upstream.

Consumed in-tree by the editor (`engine/editor`) for native open/save/folder
dialogs (the `lux::editor::*Dialog` facade in `engine/editor/src/FileDialog.cpp`).
Built via the upstream CMake (target `nfd`); `engine/CMakeLists.txt` forces
`NFD_BUILD_TESTS=OFF` and `NFD_INSTALL=OFF`, and the editor consumes it headers-only
in its static lib, linking the real `nfd` archive on the `lux_editor` executable.

**To upgrade:** re-copy `LICENSE`, `README.md`, `CMakeLists.txt`, and `src/` from the
new tag, then bump the version above. Per zlib license clause 2, if any vendored
file is ever patched locally it must be plainly marked "Altered from upstream".
