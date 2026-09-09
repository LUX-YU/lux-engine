# Lua 5.5.1 dependency wrapper

This builds the unmodified official C VM as one shared library. It is a separate dependency build,
not an engine architecture target. Only a single RelWithDebInfo configuration is accepted.

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
The Windows DLL is `lux_lua55.dll`, with `/MD` and public `LUA_BUILD_AS_DLL`. No extra-space, numeric-width,
bytecode, collector, compiler-language or optimization-policy changes are made. The Linux recipe needs a real Linux
qualification run before it can be reported as verified; other platforms have no recipe here.
