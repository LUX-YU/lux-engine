#include "lux/engine/window/TrayIcon.hpp"
#include "lux/engine/window/LuxWindow.hpp"
#include <cassert>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <windows.h>

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT	1001
#define ID_TRAY_HIDE	1002
#define ID_TRAY_RECOVER 1003

namespace lux::window
{
	class TrayIcon::Impl
	{
	public:
		Impl(lux::window::LuxWindow& window)
		{
			assert(!init);
			window.setExitBehavior(lux::window::EExitBehavior::HIDE);

			// Get the window handle
			HWND hWnd = glfwGetWin32Window(window.handle());

			originalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
			SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)WindowProc);
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)&window);
			// Create the tray icon
			CreateTrayIcon(hWnd);

			init = true;
		}

		~Impl()
		{
			// Clean up the tray icon
			CleanupTrayIcon();
		};

		static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
			LONG_PTR lpData = GetWindowLongPtr(hwnd, GWLP_USERDATA);
			lux::window::LuxWindow* window = (lux::window::LuxWindow*)lpData;
			switch (uMsg)
			{
			case WM_SYSCOMMAND:
				if (wParam == SC_MINIMIZE) {
					ShowWindow(hwnd, SW_HIDE);
					return 0;
				}
				break;
			case WM_USER + 1:
				if (lParam == WM_RBUTTONUP) {
					POINT p;
					GetCursorPos(&p);
					SetForegroundWindow(hwnd);
					TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
				}
				break;
			case WM_COMMAND:
				if (LOWORD(wParam) == ID_TRAY_EXIT) {
					Shell_NotifyIcon(NIM_DELETE, &nid);
					window->exit();
					PostQuitMessage(0);
				}
				if (LOWORD(wParam) == ID_TRAY_HIDE) {
					window->hide(true);
				}
				if (LOWORD(wParam) == ID_TRAY_RECOVER) {
					window->hide(false);
				}
				break;
			case WM_DESTROY:
				Shell_NotifyIcon(NIM_DELETE, &nid);
				PostQuitMessage(0);
				break;
			}
			return CallWindowProc(originalWndProc, hwnd, uMsg, wParam, lParam);
		}

		void CreateTrayIcon(HWND hWnd)
		{
			memset(&nid, 0, sizeof(nid));
			nid.cbSize = sizeof(NOTIFYICONDATA);
			nid.hWnd = hWnd;
			nid.uID = 1;
			nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
			nid.uCallbackMessage = WM_USER + 1;
			nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
			strcpy_s(nid.szTip, "File Backup");
			Shell_NotifyIcon(NIM_ADD, &nid);

			hMenu = CreatePopupMenu();
			AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");
			AppendMenu(hMenu, MF_STRING, ID_TRAY_HIDE, "Hide");
			AppendMenu(hMenu, MF_STRING, ID_TRAY_RECOVER, "Recover");
		}

		void CleanupTrayIcon()
		{
			Shell_NotifyIcon(NIM_DELETE, &nid);
		}

	private:
		static bool				init;
		static HMENU			hMenu;
		static NOTIFYICONDATA	nid;
		static WNDPROC			originalWndProc;
	};

	bool			TrayIcon::Impl::init = false;
	HMENU			TrayIcon::Impl::hMenu = nullptr;
	NOTIFYICONDATA	TrayIcon::Impl::nid;
	WNDPROC			TrayIcon::Impl::originalWndProc = nullptr;

	TrayIcon::TrayIcon(lux::window::LuxWindow& window)
	{
		_impl = std::make_unique<Impl>(window);
	}

	TrayIcon::TrayIcon(TrayIcon&&) noexcept = default;
	TrayIcon& TrayIcon::operator=(TrayIcon&&) noexcept = default;

	TrayIcon::~TrayIcon() = default;
}