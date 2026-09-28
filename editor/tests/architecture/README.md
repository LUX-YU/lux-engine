# Editor V4 migration checks

These are development checks, not product services or a general architecture framework.
`rules.json` contains only the V4 migration boundaries and deadlines. The original specification
and mutable ledger live locally; neither `.internal` nor `dev_log` is a build input.

`EditorArchitectureChecks.cmake` exports actual configured targets, resolves aliases and checks
conditional link edges conservatively (union of configurations). Explicit generator dependencies
are separate edges. `check_editor_boundaries.py` also checks source includes, resolved compile
include paths, retired paths and declarations. This is not C++ semantic/ownership/ABI proof.
Imported library internals and dynamic dependencies still require review. Do not turn a successful
scan into a claim that all such dependencies are absent.

`test_editor_boundaries.py` configures dependency-free CMake fixtures and checks the failure rule,
then repairs the same fixture and requires success. It tests transitive links through aliases and
`LINK_ONLY`, header leakage and expiry. Fixtures never require the engine's third-party SDKs.

`inventory_editor.py` consumes an existing compile database and clang-cl (VS developer environment).
It parses the seven P00 owners. Output distinguishes AST declarations/references from lexical
candidates; compiler diagnostics, command lines and AST hashes accompany the inventory. This tool
does not install dependencies or write generated production code.

Native-only configuration defaults to the native group; other groups must be explicitly enabled.
Use `-DCMAKE_PROJECT_INCLUDE=<absolute path>/deny_platform_test_tools.cmake` in a fresh build tree
to prove that no Editor PowerShell/lld-link lookup occurs. Keep the normal compiler, SDKs and
shader tools available: native-only tests do not turn the Editor product into a headless product.

The small Flow source test uses the existing graph/codec without UI, compiler or external linker.
The complete Flow compilation/link/publication protocol remains in the toolchain integration group.

`editor_baseline_failures C01|C03|C04 <plugin-root> <temporary-output>` is intentionally outside CTest.
Exit 0 means the intended behavior holds, 1 means its contract failed, and 2 means setup failed.
Never register these with WILL_FAIL or count a setup error as a reproduced defect. They retire with
the old framework at P12; the corresponding behavior assertions move to the new owners.
