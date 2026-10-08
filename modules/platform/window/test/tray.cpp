#undef CreatePopupMenu
#undef DestroyMenu
#undef AppendMenuW
#undef LoadIconW
#undef Shell_NotifyIconW
#undef SetWindowSubclass
#undef RemoveWindowSubclass
#undef SetForegroundWindow
#undef TrackPopupMenu
#include "TrayBackend.hpp"
#include <cassert>
#include <iostream>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/TrayIcon.hpp>
#include <type_traits>

namespace tray_test
{
    bool fail_menu{}, fail_icon{}, fail_registration{}, fail_subclass{};
    unsigned fail_item{}, item_calls{}, menus{}, menus_deleted{}, icons{}, icons_deleted{}, attached{}, detached{};
    UINT menu_command{};
    HMENU last_menu{};
    std::unique_ptr<lux::window::TrayIcon>* reset_on_foreground{};
    std::unique_ptr<lux::window::TrayIcon>* reset_in_menu{};
    std::unique_ptr<lux::window::LuxWindow>* reset_window_in_menu{};

    HMENU createMenu()
    {
        auto menu = fail_menu ? nullptr : CreatePopupMenu();
        menus += menu != nullptr;
        last_menu = menu;
        return menu;
    }

    BOOL destroyMenu(HMENU menu)
    {
        assert(IsMenu(menu));
        ++menus_deleted;
        return DestroyMenu(menu);
    }

    BOOL appendMenu(HMENU menu, UINT flags, UINT_PTR id, LPCWSTR label)
    {
        return ++item_calls == fail_item ? FALSE : AppendMenuW(menu, flags, id, label);
    }

    HICON loadIcon(HINSTANCE instance, LPCWSTR name)
    {
        return fail_icon ? nullptr : LoadIconW(instance, name);
    }

    BOOL notifyIcon(DWORD operation, PNOTIFYICONDATAW data)
    {
        if (operation == NIM_ADD && fail_registration)
        {
            return FALSE;
        }
        const auto result = Shell_NotifyIconW(operation, data);
        if (operation == NIM_ADD)
        {
            icons += result != FALSE;
        }
        else if (operation == NIM_DELETE)
        {
            ++icons_deleted;
        }
        return result;
    }

    BOOL setSubclass(HWND native, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data)
    {
        const auto result = fail_subclass ? FALSE : SetWindowSubclass(native, proc, id, data);
        attached += result != FALSE;
        return result;
    }

    BOOL removeSubclass(HWND native, SUBCLASSPROC proc, UINT_PTR id)
    {
        const auto result = RemoveWindowSubclass(native, proc, id);
        assert(result);
        ++detached;
        return result;
    }

    BOOL foreground(HWND native)
    {
        if (reset_on_foreground)
        {
            reset_on_foreground->reset();
            assert(IsMenu(last_menu)); // Active callback still owns its physical menu.
        }
        return TRUE; // Avoid taking foreground focus in this automatic callback test.
    }

    BOOL trackMenu(HMENU menu, UINT flags, int, int, int, HWND, const RECT*)
    {
        assert(IsMenu(menu) && (flags & TPM_RETURNCMD) && (flags & TPM_NONOTIFY));
        if (reset_in_menu)
        {
            reset_in_menu->reset();
            assert(IsMenu(menu));
        }
        if (reset_window_in_menu)
        {
            reset_window_in_menu->reset();
            assert(IsMenu(menu));
        }
        return static_cast<BOOL>(menu_command);
    }
} // namespace tray_test

namespace
{
    using namespace lux::window;
    using namespace tray_test;

    static_assert(!std::is_constructible_v<TrayIcon, LuxWindow&>);
    static_assert(!std::is_copy_constructible_v<TrayIcon> && !std::is_move_constructible_v<TrayIcon>);

    void balanced()
    {
        assert(menus == menus_deleted && icons == icons_deleted && attached == detached);
    }

    void refused(LuxWindow& window, ETrayError code)
    {
        item_calls = 0;
        const auto native = static_cast<HWND>(window.nativeHandle());
        const auto userdata = GetWindowLongPtrW(native, GWLP_USERDATA);
        auto tray = TrayIcon::create(window);
        std::cout << "fault expected=" << static_cast<unsigned>(code)
                  << " actual=" << (tray ? -1 : static_cast<int>(tray.error())) << std::endl;
        assert(!tray && tray.error() == code);
        assert(GetWindowLongPtrW(native, GWLP_USERDATA) == userdata);
        balanced();
    }
} // namespace

int main()
{
    using namespace lux::window;
    using namespace tray_test;
    auto runtime = GlfwRuntime::create();
    assert(runtime);
    auto window = LuxWindow::create({320, 240, "Tray lifetime regression"});
    assert(window);
    (*window)->hide(true);
    const auto native = static_cast<HWND>((*window)->nativeHandle());
    const auto userdata = GetWindowLongPtrW(native, GWLP_USERDATA);
    fail_menu = true;
    refused(**window, ETrayError::MENU_CREATION_FAILED);
    fail_menu = false;
    for (unsigned item = 1; item <= 3; ++item)
    {
        fail_item = item;
        refused(**window, ETrayError::MENU_ITEM_FAILED);
    }
    fail_item = 0;
    fail_icon = true;
    refused(**window, ETrayError::ICON_UNAVAILABLE);
    fail_icon = false;
    fail_registration = true;
    refused(**window, ETrayError::REGISTRATION_FAILED);
    fail_registration = false;
    fail_subclass = true;
    refused(**window, ETrayError::CALLBACK_REGISTRATION_FAILED);
    fail_subclass = false;

    auto tray = TrayIcon::create(**window);
    assert(tray && GetWindowLongPtrW(native, GWLP_USERDATA) == userdata);
    const auto acquired = menus;
    auto duplicate = TrayIcon::create(**window);
    assert(!duplicate && duplicate.error() == ETrayError::ALREADY_ATTACHED && menus == acquired);
    // An unrelated native command must never be interpreted as this tray's menu choice.
    SendMessageW(native, WM_COMMAND, 1001, 0);
    assert(!(*window)->shouldClose());
    SendMessageW(native, WM_CLOSE, 0, 0);
    assert(!(*window)->shouldClose()); // Successful tray owns HIDE; original policy is EXIT.
    tray->reset();
    balanced();
    SendMessageW(native, WM_CLOSE, 0, 0);
    assert((*window)->shouldClose()); // The previous policy is restored.
    window->reset();

    window = LuxWindow::create({320, 240, "Independent tray A"});
    auto second = LuxWindow::create({320, 240, "Independent tray B"});
    assert(window && second);
    (*window)->hide(true);
    (*second)->hide(true);
    tray = TrayIcon::create(**window);
    auto other = TrayIcon::create(**second);
    assert(tray && other);
    const auto first_native = static_cast<HWND>((*window)->nativeHandle());
    menu_command = 1001;
    SendMessageW(first_native, WM_APP + 1, 0, WM_RBUTTONUP);
    assert((*window)->shouldClose() && !(*second)->shouldClose());
    window->reset(); // Revokes the callback and shell registration while the tray owner remains alive.
    tray->reset();
    assert(menus == menus_deleted + 1 && icons == icons_deleted + 1 && attached == detached + 1);
    second->reset();
    other->reset();
    balanced();

    for (bool during_menu : {false, true})
    {
        window = LuxWindow::create({320, 240, "Reentrant tray teardown"});
        assert(window);
        (*window)->hide(true);
        tray = TrayIcon::create(**window);
        assert(tray);
        if (during_menu)
        {
            reset_in_menu = &*tray;
        }
        else
        {
            reset_on_foreground = &*tray;
        }
        menu_command = 1001;
        SendMessageW(static_cast<HWND>((*window)->nativeHandle()), WM_APP + 1, 0, WM_RBUTTONUP);
        assert(!*tray && !(*window)->shouldClose());
        reset_in_menu = nullptr;
        reset_on_foreground = nullptr;
        balanced();
        window->reset();
    }
    window = LuxWindow::create({320, 240, "Native destruction inside menu dispatch"});
    assert(window);
    (*window)->hide(true);
    tray = TrayIcon::create(**window);
    assert(tray);
    reset_window_in_menu = &*window;
    SendMessageW(static_cast<HWND>((*window)->nativeHandle()), WM_APP + 1, 0, WM_RBUTTONUP);
    reset_window_in_menu = nullptr;
    assert(!*window && *tray && icons == icons_deleted && attached == detached);
    tray->reset();
    balanced();
    std::cout
        << "tray: real native rollback, independent owners, close policy, GLFW userdata and reentrant cleanup PASS\n";
}
