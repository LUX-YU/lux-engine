#include <exception>
#include <iterator>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/TrayIcon.hpp>
#include <type_traits>
#include <utility>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off: Win32 extension headers require the umbrella definitions first.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
// clang-format on

namespace lux::window
{
    namespace
    {
        constexpr UINT tray_message = WM_APP + 1;
        constexpr UINT_PTR subclass_id = 1;
        constexpr UINT exit_command = 1001;
        constexpr UINT hide_command = 1002;
        constexpr UINT restore_command = 1003;

        struct MenuDeleter final
        {
            void operator()(HMENU menu) const noexcept
            {
                DestroyMenu(menu);
            }
        };

        struct IconDeleter final
        {
            void operator()(NOTIFYICONDATAW* data) const noexcept
            {
                Shell_NotifyIconW(NIM_DELETE, data);
                delete data;
            }
        };

        using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;
        using Icon = std::unique_ptr<NOTIFYICONDATAW, IconDeleter>;
    } // namespace

    struct TrayIcon::Impl final : std::enable_shared_from_this<Impl>
    {
        LuxWindow* window;
        Menu menu;
        Icon icon;
        EExitBehavior previous_exit;
        HWND attached_window{};

        Impl(LuxWindow& owner, Menu prepared_menu, Icon prepared_icon) noexcept
            : window(&owner), menu(std::move(prepared_menu)), icon(std::move(prepared_icon)),
              previous_exit(owner.exit_behavior_)
        {
        }

        void detach() noexcept
        {
            if (auto native = std::exchange(attached_window, nullptr))
            {
                if (!RemoveWindowSubclass(native, windowProc, subclass_id))
                {
                    std::terminate(); // A live native callback cannot retain a soon-to-be-released state pointer.
                }
                window->setExitBehavior(previous_exit);
                window = nullptr;
            }
            icon.reset();
        }

        static LRESULT CALLBACK
        windowProc(HWND native, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) noexcept
        {
            // SetForegroundWindow and TrackPopupMenu can synchronously call application code.
            // The registration is revoked immediately on teardown, while this invocation retains its menu.
            const auto pinned = reinterpret_cast<Impl*>(data)->shared_from_this();
            auto& state = *pinned;
            if (message == WM_NCDESTROY)
            {
                // Revoke the borrow before native destruction can finish or this HWND can be reused.
                state.attached_window = nullptr;
                state.window = nullptr;
                if (!RemoveWindowSubclass(native, windowProc, id))
                {
                    std::terminate();
                }
                state.icon.reset();
                return DefSubclassProc(native, message, wparam, lparam);
            }
            if (message == WM_SYSCOMMAND && (wparam & 0xfff0u) == SC_MINIMIZE)
            {
                state.window->hide(true);
                return 0;
            }
            if (message == tray_message && lparam == WM_RBUTTONUP)
            {
                POINT point{};
                if (!GetCursorPos(&point))
                {
                    return 0;
                }
                SetForegroundWindow(native);
                if (!state.attached_window)
                {
                    return 0;
                }
                const auto selected = TrackPopupMenu(
                    state.menu.get(),
                    TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                    point.x,
                    point.y,
                    0,
                    native,
                    nullptr
                );
                if (!state.attached_window)
                {
                    return 0;
                }
                // Commands are returned by this menu only; unrelated window WM_COMMAND IDs are untouched.
                switch (selected)
                {
                case exit_command:
                    state.window->exit();
                    break;
                case hide_command:
                    state.window->hide(true);
                    break;
                case restore_command:
                    state.window->hide(false);
                    break;
                default:
                    break;
                }
                return 0;
            }
            return DefSubclassProc(native, message, wparam, lparam);
        }
    };

    TrayIcon::CreateResult TrayIcon::create(LuxWindow& window) noexcept
    {
        const auto native = static_cast<HWND>(window.nativeHandle());
        DWORD_PTR existing{};
        if (GetWindowSubclass(native, Impl::windowProc, subclass_id, &existing))
        {
            return lux::cxx::unexpected(ETrayError::ALREADY_ATTACHED);
        }
        Menu menu(CreatePopupMenu());
        if (!menu)
        {
            return lux::cxx::unexpected(ETrayError::MENU_CREATION_FAILED);
        }
        const bool has_items = AppendMenuW(menu.get(), MF_STRING, exit_command, L"Exit") &&
                               AppendMenuW(menu.get(), MF_STRING, hide_command, L"Hide") &&
                               AppendMenuW(menu.get(), MF_STRING, restore_command, L"Restore");
        if (!has_items)
        {
            return lux::cxx::unexpected(ETrayError::MENU_ITEM_FAILED);
        }
        // LoadIconW's stock icon is shared by Windows and must not be destroyed by this owner.
        const auto stock_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        if (!stock_icon)
        {
            return lux::cxx::unexpected(ETrayError::ICON_UNAVAILABLE);
        }
        auto descriptor = std::make_unique<NOTIFYICONDATAW>();
        descriptor->cbSize = sizeof(NOTIFYICONDATAW);
        descriptor->hWnd = native;
        descriptor->uID = 1;
        descriptor->uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        descriptor->uCallbackMessage = tray_message;
        descriptor->hIcon = stock_icon;
        GetWindowTextW(native, descriptor->szTip, static_cast<int>(std::size(descriptor->szTip)));
        if (!Shell_NotifyIconW(NIM_ADD, descriptor.get()))
        {
            return lux::cxx::unexpected(ETrayError::REGISTRATION_FAILED);
        }
        auto prepared = std::make_shared<Impl>(window, std::move(menu), Icon(descriptor.release()));
        if (!SetWindowSubclass(native, Impl::windowProc, subclass_id, reinterpret_cast<DWORD_PTR>(prepared.get())))
        {
            return lux::cxx::unexpected(ETrayError::CALLBACK_REGISTRATION_FAILED);
        }
        prepared->attached_window = native;
        window.setExitBehavior(EExitBehavior::HIDE);
        return std::unique_ptr<TrayIcon>(new TrayIcon(std::move(prepared)));
    }

    TrayIcon::TrayIcon(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    TrayIcon::~TrayIcon() noexcept
    {
        impl_->detach();
    }
} // namespace lux::window
