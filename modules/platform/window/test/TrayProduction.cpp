// Compile the actual production owner with test-only native failure injection.
// clang-format off: native remapping must precede compilation of the owner body.
#include "TrayRemap.hpp"
#include "../src/TrayIconWin32.cpp"
// clang-format on
