#include <lux/engine/ui/UISession.hpp>
#include <lux/engine/ui/detail/UiPresentationData.hpp>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <fstream>
#include <string_view>

extern "C" __declspec(dllimport) void lux_er1_function_ui_allocation_fail_after(std::size_t) noexcept;
extern "C" __declspec(dllimport) std::size_t lux_er1_function_ui_allocation_disarm() noexcept;
namespace
{
    std::size_t allocations{}, releases{};
    void *allocate(std::size_t bytes, void *)
    {
        auto *result = std::malloc(bytes);
        if (result)
            ++allocations;
        return result;
    }
    void release(void *address, void *)
    {
        if (address)
            ++releases;
        std::free(address);
    }
    bool draw(lux::ui::UISession &session)
    {
        auto frame = session.beginFrame({{640, 480}, 1.0F / 60.0F, {1, 1}});
        frame.finish();
        return bool(session.captureFrame());
    }
} // namespace
int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
        return 2;
    const auto index = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    const bool atlas_failure = std::string_view{argv[1]} == "atlas";
    ImGui::SetAllocatorFunctions(allocate, release);
    lux::ui::UiFontSource source;
    if (argc == 3)
    {
        std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0)
            return 2;
        source.bytes.resize(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(source.bytes.data()), source.bytes.size()))
            return 2;
        source.ranges = {{0x20, 0x7E}, {0x3000, 0x303F}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}};
    }
    const auto checksum = [&] {
        std::uint64_t result{1469598103934665603ULL};
        for (const auto value : source.bytes)
            result = (result ^ value) * 1099511628211ULL;
        return result;
    };
    const auto input_checksum = checksum();
    bool caught{}, restored{}, reusable{}, cleaned{};
    int structured_error{-1};
    std::size_t attempts{};
    {
        lux::ui::UISession a;
        if (!draw(a))
            return 2;
        ImGuiContext *a_context{};
        {
            auto atlas = lux::ui::detail::captureUiFontAtlas(a);
            if (!atlas)
                return 2;
            a_context = static_cast<ImGuiContext *>(atlas->context);
        }
        ImGui::SetCurrentContext(a_context);
        const auto before = allocations - releases;
        lux_er1_function_ui_allocation_fail_after(index);
        try
        {
            if (atlas_failure)
            {
                auto captured = lux::ui::detail::captureUiFontAtlas(a);
                if (!captured)
                {
                    structured_error = static_cast<int>(captured.error());
                    caught = captured.error() == lux::ui::EUiInitError::ALLOCATION_FAILURE;
                }
                if (!caught)
                    return 2;
            }
            else if (argc == 3)
            {
                auto b = lux::ui::UISession::create({}, &source);
                if (!b)
                {
                    structured_error = static_cast<int>(b.error());
                    caught = b.error() == lux::ui::EUiInitError::ALLOCATION_FAILURE;
                    if (!caught)
                        return 2;
                }
            }
            else
            {
                lux::ui::UISession b;
            }
        }
        catch (const std::bad_alloc &)
        {
            caught = true;
        }
        attempts = lux_er1_function_ui_allocation_disarm();
        restored = ImGui::GetCurrentContext() == a_context;
        const auto after = allocations - releases;
        cleaned = before == after;
        reusable = draw(a);
        {
            auto b = lux::ui::UISession::create({}, argc == 3 ? &source : nullptr);
            reusable = b && draw(**b) && draw(a) && reusable;
        }
        const bool input_retained = input_checksum == checksum();
        cleaned &= input_retained;
        std::printf("UI construction index=%zu caught_bad_alloc=%d attempts=%zu imgui_live=%zu->%zu "
                    "previous_context_restored=%d a_and_retry_b_usable=%d structured_error=%d input_retained=%d\n",
                    index, caught, attempts, before, after, restored, reusable, structured_error, input_retained);
    }
    const bool passed = restored && reusable && cleaned && allocations == releases;
    std::printf("UI construction %s allocations=%zu releases=%zu remaining=%zu\n", passed ? "PASS" : "FAIL",
                allocations, releases, allocations - releases);
    return passed ? 0 : 1;
}
