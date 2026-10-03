#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    using namespace lux::window;

    void purePolicy()
    {
        std::array displays{
            DisplayInfo{{"same name", {0, 0, 1920, 1040}}, {0, 0, 1920, 1080}, {1.f, 1.f}, {}, {}, true},
            DisplayInfo{{"same name", {-2560, -100, 2560, 1400}}, {-2560, -100, 2560, 1440}, {2.f, 2.f}}
        };
        auto result = resolveWindowPlacement({}, displays);
        assert(result && result->placement.normal.width == 1440);
        assert(result->placement.normal.y >= 32 && result->placement.mode == EWindowMode::ORDINARY);
        WindowPlacement saved{{-2400, -10, 1000, 700}, EWindowMode::MAXIMIZED, displays[1].hint};
        result = resolveWindowPlacement({saved}, displays);
        assert(result && !result->adjusted && result->placement.normal == saved.normal);
        assert(result->placement.display.work_area.x < 0 && result->placement.mode == EWindowMode::MAXIMIZED);
        // Scale is a separate fact, not a second multiplication of content dimensions.
        result = resolveWindowPlacement({saved, WindowSize{1200, 800}}, displays);
        assert(result && result->placement.normal.width == 1200 && result->placement.normal.height == 800);
        result = resolveWindowPlacement({saved}, std::span{displays}.first(1));
        assert(result && result->adjusted && result->placement.normal.x >= 8);
        assert(result->placement.normal == (WindowRect{460, 182, 1000, 700}));
        saved.normal = {std::numeric_limits<int>::max(), 0, 1000, 800};
        result = resolveWindowPlacement({saved}, displays);
        assert(result && result->adjusted && result->placement.normal.x >= -2552);
        assert(!resolveWindowPlacement({{}, WindowSize{0, 900}}, displays));
        assert(!resolveWindowPlacement({{}, WindowSize{32769, 900}}, displays));
        assert(!resolveWindowPlacement({{}, {}, static_cast<EWindowMode>(255)}, displays));
        assert(!resolveWindowPlacement({}, displays, {-1, 0, 0, 0}));
        assert(!resolveWindowPlacement({}, {}));
        displays[0].hint.work_area = {-10, -20, 2, 2};
        result = resolveWindowPlacement({}, std::span{displays}.first(1));
        assert(result && result->placement.normal == (WindowRect{-9, -19, 1, 1}));
        displays[0].scale.x = std::numeric_limits<float>::quiet_NaN();
        assert(!resolveWindowPlacement({}, std::span{displays}.first(1)));
        std::cout << "pure: negative coordinates, ambiguous names, DPI, disconnect, tiny area, invalid inputs PASS\n";
    }

    void desktop()
    {
        GlfwRuntime runtime;
        assert(runtime.valid());
        auto displays = LuxWindow::displays();
        assert(displays && !displays->empty());
        auto resolved = resolveWindowPlacement({{}, WindowSize{800, 600}}, *displays);
        assert(resolved);
        LuxWindow window(800, 600, "EC3 window placement qualification");
        assert(window.isInitialized());
        unsigned notifications{};
        window.on_placement_changed = [&](const WindowPlacementEvent&) { ++notifications; };
        auto adopted = window.applyPlacement(resolved->placement);
        assert(adopted);
        const auto settled = [&](EWindowMode mode) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            for (;;)
            {
                LuxWindow::pollEvents();
                auto actual = window.state();
                assert(actual);
                if (actual->placement.mode == mode)
                    return *actual;
                assert(std::chrono::steady_clock::now() < deadline);
                LuxWindow::waitEvents(0.01);
            }
        };
        auto ordinary = settled(EWindowMode::ORDINARY);
        const auto normal = ordinary.placement.normal;
        assert(normal.width == 800 && normal.height == 600);
        for (const auto mode : {EWindowMode::MAXIMIZED, EWindowMode::FULLSCREEN, EWindowMode::ORDINARY})
        {
            auto desired = ordinary.placement;
            desired.mode = mode;
            assert(window.applyPlacement(desired));
            const auto actual = settled(mode);
            assert(actual.placement.normal == normal);
            assert(actual.content.width > 0 && actual.content.height > 0);
            assert(actual.scale.x > 0.f && actual.scale.y > 0.f);
            std::uint32_t width{}, height{};
            window.framebufferSize(width, height);
            assert(width && height);
            std::cout << "mode=" << static_cast<int>(mode) << " content=" << actual.content.width << 'x'
                << actual.content.height << " framebuffer=" << width << 'x' << height << " scale="
                << actual.scale.x << ',' << actual.scale.y << " restore=" << normal.x << ',' << normal.y << ','
                << normal.width << ',' << normal.height << '\n';
        }
        assert(notifications > 0);
        const auto before = window.state();
        assert(before);
        auto invalid = before->placement;
        invalid.normal.width = 0;
        assert(!window.applyPlacement(invalid));
        assert(window.state()->placement.normal == before->placement.normal);
        std::cout << "actual platform placement PASS; not a native-input/IME qualification\n";
    }
}

int main(int argc, char** argv)
{
    purePolicy();
    if (argc == 2 && std::string_view{argv[1]} == "--desktop")
        desktop();
}
