#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off: instrument the production Win32 boundary, not a replacement owner.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
// clang-format on

namespace tray_test
{
    HMENU createMenu();
    BOOL destroyMenu(HMENU);
    BOOL appendMenu(HMENU, UINT, UINT_PTR, LPCWSTR);
    HICON loadIcon(HINSTANCE, LPCWSTR);
    BOOL notifyIcon(DWORD, PNOTIFYICONDATAW);
    BOOL setSubclass(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
    BOOL removeSubclass(HWND, SUBCLASSPROC, UINT_PTR);
    BOOL foreground(HWND);
    BOOL trackMenu(HMENU, UINT, int, int, int, HWND, const RECT*);
} // namespace tray_test
