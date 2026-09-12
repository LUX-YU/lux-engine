# lux-engine

C++20 engine libraries, offline asset tools, and a Vulkan editor.

- `modules/`: reusable platform, core, resource, rendering, and scripting libraries.
- `engine/domain/`: World data, Simulation, ECS, and built-in systems.
- `engine/process/`: asynchronous execution and loading.
- `engine/scene/`: World/Simulation composition and presentation.
- `engine/toolchain/`: asset compilation, script generation, and packaging.
- `engine/editor/`: editor application and UI.
- `cmake/`: build, dependency, and installation support.

## Build and install

Use CMake 3.22 or newer, Ninja, and a C++20 compiler. Install the sibling
`lux-cmake-toolset` and `lux-cxx` packages first, together with the dependencies
required by the selected components. On Windows, run from an x64 Visual Studio
developer shell and supply the vcpkg toolchain if using vcpkg.

Lua scripting requires Lux's patched Lua 5.5.1 dependency. Build it using
[`cmake/dependencies/lua55`](cmake/dependencies/lua55/CMakeLists.txt) and provide
its installation through `LuxLua55_DIR` or `CMAKE_PREFIX_PATH`. The preparation
and patch scripts are in the same directory. Lua 5.4 and LuaJIT are not supported.

```sh
cmake -S . -B ../build/lux-engine -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="<toolset-prefix>;<cxx-prefix>;<dependency-prefix>" \
  -DLuxLua55_DIR="<lua55-prefix>/lib/cmake/LuxLua55" \
  -DCMAKE_INSTALL_PREFIX="<sdk-prefix>" \
  -DLUX_BUILD_PROFILE=DEVELOPER
cmake --build ../build/lux-engine --target all -j 4 -- -k 0
cmake --install ../build/lux-engine --config RelWithDebInfo
```

`DEVELOPER` and `PLAYER` build the runtime libraries. `TOOLCHAIN` also builds
offline tools; `EDITOR` builds the editor and its required tools. `PLAYER` is a
library configuration, not a generated game executable.

Optional features include `LUX_BUILD_PHYSICS2D`, `LUX_SCRIPT_HAS_LUA`, and
`LUX_BUILD_PACKED_RENDER_CONTENT`. Cross-compilation uses a target toolchain and
separately installed host tools, configured through `LUX_HOST_TOOLS_PREFIX`.

Installed CMake packages export component targets under `lux::engine` namespaces,
with public headers, required generated headers, libraries, and code-generation
helpers. Private `pinclude/` and project-only `sinclude/` headers are not SDK APIs.
