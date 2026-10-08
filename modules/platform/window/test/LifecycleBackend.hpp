#pragma once

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off: mirror the production native-header prerequisites.
#include <windows.h>
#include <commctrl.h>
#endif
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
// clang-format on

// Test-only native-call instrumentation. The tested owner code is the actual production source.
namespace lifecycle_test
{
    int initialize();
    void terminate();
    int vulkanSupported();
    GLFWwindow* createWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*);
    void destroyWindow(GLFWwindow*);
#if defined(_WIN32)
    BOOL setSubclass(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
    BOOL removeSubclass(HWND, SUBCLASSPROC, UINT_PTR);
#endif
} // namespace lifecycle_test
