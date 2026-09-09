# Lua 5.5.1 dependency wrapper

This builds Lua 5.5.1 as one shared C library. It is a separate dependency build, not an engine
architecture target. Only a single RelWithDebInfo configuration is accepted. The default applies
the independently identified `lux-leaf-r1` patch to a verified copy; official input files stay unchanged.

Download [Lua 5.5.1](https://www.lua.org/ftp/lua-5.5.1.tar.gz) before configuring. Verify the archive SHA-256:
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce` (398643 bytes).
Use the verifier below to unpack to a new absolute path. CMake performs no downloads;
its header release check does not replace archive verification.

```powershell
python cmake/dependencies/lua55/prepare.py --archive <downloaded-tar.gz> --output <absolute-source>
cmake -S cmake/dependencies/lua55 -B <dependency-build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLUX_LUA55_SOURCE_DIR=<absolute-source> -DCMAKE_INSTALL_PREFIX=<dependency-prefix>
cmake --build <dependency-build> --target all -j 4 -- -k 0
cmake --build <dependency-build> --target all -j 4 -- -k 0
ctest --test-dir <dependency-build> --output-on-failure
cmake --install <dependency-build> --config RelWithDebInfo
```

Consumers use `find_package(LuxLua55 5.5.1 EXACT CONFIG REQUIRED)` and `LuxLua55::Runtime`.
Headers reside in `include/lua55`; the imported target provides that include directory and the real DLL/import library.
The Windows DLL is `lux_lua55.dll`, with `/MD` and public `LUA_BUILD_AS_DLL`. Numeric widths, bytecode,
standard coroutine APIs and error protection stay unchanged. `share/LuxLua55/identity.json` records
compiler, flags, patch SHA and revision. Never infer the installed identity from another repository's HEAD.

`LUX_LUA55_LEAF_YIELD=OFF` builds the same official Lua55 without the experimental return optimization;
this is not an old-VM selector. Eligible engine leaf waits propagate a yielded status through actual
CallInfo/OP_CALL/resume/unroll frames. Protected calls, C reentry, hooks, tail/iterator/metamethod calls
and to-be-closed values use the standard Lua55 yield path. User errors always use standard protection.
`leaf_contract.c` verifies direct and fallback counts, deep/repeated waits, cancellation, thread identity,
recovery and OOM. The engine's typed C++ worker must finish before the C boundary calls error or yield.

`LUX_LUA55_JUMPTABLE=ON` requires an actual labels-as-values compile/run probe. One clang-cl 19.1.5
build passed the probe and correctness tests, but its bounded workload comparison did not justify
replacing the MSVC production dependency. The option remains an explicit dependency experiment.
The engine owns its allocator from `lua_newstate` through `lua_close`, defaults to incremental GC
after an Event/Physics comparison, and exposes the six Lua55 GC parameters through `LuaVmConfiguration`.
It reuses only memory released by the VM, never observable thread objects.

An optional verified official suite directory can be supplied as `LUX_LUA55_TEST_SUITE`; it is not
downloaded by CMake. The upstream 5.5.1 basic suite contains Linux device paths even with `_U=true`.
The Windows qualification keeps the original suite and separately records the NUL-device/portable-mode
test adaptation. It does not claim the upstream internal C test library or all platforms were tested.
The Linux recipe needs a real Linux qualification run; other platforms have no recipe here.
