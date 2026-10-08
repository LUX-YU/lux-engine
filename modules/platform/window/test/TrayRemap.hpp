#pragma once
#include "TrayBackend.hpp"
#define CreatePopupMenu tray_test::createMenu
#define DestroyMenu tray_test::destroyMenu
#define AppendMenuW tray_test::appendMenu
#define LoadIconW tray_test::loadIcon
#define Shell_NotifyIconW tray_test::notifyIcon
#define SetWindowSubclass tray_test::setSubclass
#define RemoveWindowSubclass tray_test::removeSubclass
#define SetForegroundWindow tray_test::foreground
#define TrackPopupMenu tray_test::trackMenu
