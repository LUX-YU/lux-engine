// This TU delegates to the actual backend; only production-owner TUs are remapped.
#undef glfwInit
#undef glfwTerminate
#undef glfwVulkanSupported
#undef glfwCreateWindow
#undef glfwDestroyWindow
#undef SetWindowSubclass
#undef RemoveWindowSubclass

#include "LifecycleBackend.hpp"
#include <cassert>
#include <iostream>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <string_view>
#include <type_traits>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace lifecycle_test
{
    bool fail_init{}, fail_vulkan{}, fail_window{}, fail_subclass{};
    unsigned init_calls{}, terminate_calls{}, window_attempts{}, created{}, destroyed{}, attached{}, detached{};
#if defined(_WIN32)
    HWND last_native{};
#endif

    int initialize()
    {
        ++init_calls;
        return fail_init ? GLFW_FALSE : glfwInit();
    }

    void terminate()
    {
        ++terminate_calls;
        glfwTerminate();
    }

    int vulkanSupported()
    {
        return fail_vulkan ? GLFW_FALSE : glfwVulkanSupported();
    }

    GLFWwindow* createWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* shared)
    {
        ++window_attempts;
        auto* window = fail_window ? nullptr : glfwCreateWindow(width, height, title, monitor, shared);
        if (window)
        {
            ++created;
#if defined(_WIN32)
            last_native = glfwGetWin32Window(window);
#endif
        }
        return window;
    }

    void destroyWindow(GLFWwindow* window)
    {
        assert(window && glfwGetWindowUserPointer(window) == nullptr);
        ++destroyed;
        glfwDestroyWindow(window);
    }

#if defined(_WIN32)
    BOOL setSubclass(HWND window, SUBCLASSPROC procedure, UINT_PTR id, DWORD_PTR data)
    {
        if (fail_subclass)
        {
            return FALSE;
        }
        const auto result = SetWindowSubclass(window, procedure, id, data);
        attached += result != FALSE;
        return result;
    }

    BOOL removeSubclass(HWND window, SUBCLASSPROC procedure, UINT_PTR id)
    {
        const auto result = RemoveWindowSubclass(window, procedure, id);
        detached += result != FALSE;
        return result;
    }
#endif
} // namespace lifecycle_test

namespace
{
    using namespace lux::window;
    using namespace lifecycle_test;

    static_assert(!std::is_default_constructible_v<GlfwRuntime>);
    static_assert(!std::is_constructible_v<LuxWindow, InitParameter>);
    static_assert(!std::is_copy_constructible_v<LuxWindow> && !std::is_move_constructible_v<LuxWindow>);

    void runtimeLifetime()
    {
        fail_init = true;
        auto failed = GlfwRuntime::create();
        assert(!failed && failed.error() == EGlfwInitError::BACKEND_FAILURE);
        assert(init_calls == 1 && terminate_calls == 0);
        fail_init = false;

        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
        auto runtime = GlfwRuntime::create();
        assert(runtime && init_calls == 2);
        auto duplicate = GlfwRuntime::create();
        assert(!duplicate && duplicate.error() == EGlfwInitError::ALREADY_ACTIVE);
        assert(init_calls == 2 && terminate_calls == 0);
        (void)LuxWindow::requiredVulkanInstanceExtensions();
        (void)LuxWindow::requiredVulkanInstanceExtensions();
        assert(init_calls == 2); // Querying requirements never owns/initializes the platform.
        runtime->reset();
        assert(terminate_calls == 1);
        runtime = GlfwRuntime::create();
        assert(runtime && init_calls == 3);
        runtime->reset();
        assert(terminate_calls == 2);
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
        std::cout << "runtime: failure has no teardown; one live owner; recreate and exact terminate PASS\n";
    }

    class DerivedWindow final : public LuxWindow
    {
    public:
        static bool rejectPreparedCandidate() noexcept
        {
            auto native = prepareNative({320, 240, "Rejected derived candidate"});
            return bool(native); // A later derived factory failure drops the prepared owner.
        }
    };

    void nativeLifetime()
    {
        auto runtime = GlfwRuntime::create();
        assert(runtime);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        fail_vulkan = true;
        auto failed = LuxWindow::create({320, 240, "Vulkan failure"});
        assert(!failed && failed.error() == EWindowInitError::VULKAN_UNAVAILABLE);
        assert(window_attempts == 0 && created == 0 && destroyed == 0);
        fail_vulkan = false;
        fail_window = true;
        failed = LuxWindow::create({320, 240, "Backend failure"});
        assert(!failed && failed.error() == EWindowInitError::BACKEND_CREATE_FAILED);
        assert(window_attempts == 1 && created == 0 && destroyed == 0);
        fail_window = false;
        failed = LuxWindow::create({0, 240, "Actual GLFW invalid extent"});
        assert(!failed && failed.error() == EWindowInitError::BACKEND_CREATE_FAILED);
        assert(window_attempts == 2 && created == 0 && destroyed == 0);
#if defined(_WIN32)
        fail_subclass = true;
        failed = LuxWindow::create({320, 240, "Callback failure"});
        assert(!failed && failed.error() == EWindowInitError::CALLBACK_REGISTRATION_FAILED);
        assert(created == 1 && destroyed == 1 && !IsWindow(last_native));
        assert(attached == 0 && detached == 0);
        fail_subclass = false;
#endif
        const auto before = created;
        assert(DerivedWindow::rejectPreparedCandidate());
        assert(created == before + 1 && destroyed == created);
        assert(attached == detached);
        auto window = LuxWindow::create({320, 240, "Complete window"});
        assert(window && (*window)->nativeHandle() && (*window)->handle());
        assert(std::string_view{(*window)->title()} == "Complete window");
        assert(glfwGetWindowUserPointer((*window)->handle()) == window->get());
        std::uint32_t width{}, height{};
        (*window)->size(width, height);
        assert(width == 320 && height == 240);
#if defined(_WIN32)
        SendMessageW(static_cast<HWND>((*window)->win32Handle()), WM_IME_STARTCOMPOSITION, 0, 0);
        const auto input = (*window)->drainInputEvents();
        assert(!input.empty() && std::holds_alternative<WindowCompositionEvent>(input.back()));
#endif
        window->reset();
        assert(created == destroyed && attached == detached);
#if defined(_WIN32)
        assert(!IsWindow(last_native));
#endif
        std::cout
            << "native: real window + injected create/subclass failures, rollback and callback lifetime PASS\n";
    }
} // namespace

int main(int argc, char** argv)
{
    runtimeLifetime();
    if (argc == 2 && std::string_view{argv[1]} == "--desktop")
    {
        nativeLifetime();
    }
}
