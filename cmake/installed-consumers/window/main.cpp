#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <cassert>
#include <iostream>
#include <type_traits>

namespace
{
    using namespace lux::window;

    class ConsumerWindow final : public LuxWindow
    {
    public:
        using Result = lux::cxx::expected<std::unique_ptr<ConsumerWindow>, EWindowInitError>;

        static Result create(bool fail_after_prepare) noexcept
        {
            auto native = prepareNative({320, 240, "Installed derived window"});
            if (!native)
            {
                return lux::cxx::unexpected(native.error());
            }
            if (fail_after_prepare)
            {
                // Real cross-component candidate destruction must link and execute in the provider.
                return lux::cxx::unexpected(EWindowInitError::BACKEND_CREATE_FAILED);
            }
            return std::unique_ptr<ConsumerWindow>(new ConsumerWindow(std::move(*native)));
        }

    private:
        explicit ConsumerWindow(NativeWindowOwner native) noexcept : LuxWindow(std::move(native)) {}
    };

    static_assert(!std::is_default_constructible_v<GlfwRuntime>);
    static_assert(!std::is_constructible_v<LuxWindow, InitParameter>);
    static_assert(!std::is_copy_constructible_v<LuxWindow> && !std::is_move_constructible_v<LuxWindow>);
}

int main()
{
    using namespace lux::window;
    auto runtime = GlfwRuntime::create();
    assert(runtime);
    auto duplicate = GlfwRuntime::create();
    assert(!duplicate && duplicate.error() == EGlfwInitError::ALREADY_ACTIVE);
    assert(!LuxWindow::requiredVulkanInstanceExtensions().empty());
    auto invalid = LuxWindow::create({0, 240, "Rejected extent"});
    assert(!invalid && invalid.error() == EWindowInitError::BACKEND_CREATE_FAILED);
    auto refused = ConsumerWindow::create(true);
    assert(!refused && refused.error() == EWindowInitError::BACKEND_CREATE_FAILED);
    {
        auto created = ConsumerWindow::create(false);
        assert(created && (*created)->nativeHandle());
        (*created)->hide(true);
        std::uint32_t width{}, height{};
        (*created)->size(width, height);
        assert(width == 320 && height == 240);
    }
    runtime->reset();
    runtime = GlfwRuntime::create();
    assert(runtime);
    auto window = LuxWindow::create({320, 240, "Installed window"});
    assert(window);
    (*window)->hide(true);
    window->reset();
    std::cout << "installed complete owner, derived factory rollback and recreate PASS\n";
}
