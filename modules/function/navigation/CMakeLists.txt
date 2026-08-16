# Navigation is split into a backend-independent algorithm contract and leaf
# implementations.  Neither layer owns entities, streaming policy or a World.
add_subdirectory(core)
add_subdirectory(detour3d)
