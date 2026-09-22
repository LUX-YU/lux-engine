#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <imgui.h>
#include <iostream>
#include <lux/engine/ui/detail/ImGuiDrawDataSnapshot.hpp>
#include <string_view>

namespace
{
struct Allocations final
{
    std::size_t count{}, bytes{};
    static void *allocate(std::size_t size, void *data)
    {
        auto &self = *static_cast<Allocations *>(data);
        ++self.count;
        self.bytes += size;
        return std::malloc(size);
    }
    static void release(void *memory, void *)
    {
        std::free(memory);
    }
};

int measure()
{
    using Clock = std::chrono::steady_clock;
    Allocations allocations;
    ImGui::SetAllocatorFunctions(&Allocations::allocate, &Allocations::release, &allocations);
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600, 900};
    io.DeltaTime = 1.F / 60;
    unsigned char *atlas{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&atlas, &width, &height);
    lux::ui::detail::ImGuiDrawDataSnapshot snapshot;
    for (const unsigned rectangles : {1000U, 10000U})
    {
        for (const bool warm : {false, true})
        {
            const unsigned frames = warm ? 100 : 1;
            std::chrono::nanoseconds build{}, capture{};
            std::size_t copied{}, growth_count{}, growth_bytes{}, retained{};
            std::uint64_t checksum{};
            for (unsigned frame = 0; frame < frames; ++frame)
            {
                const auto begin = Clock::now();
                ImGui::NewFrame();
                auto *list = ImGui::GetBackgroundDrawList();
                // Both representative sizes fit the default 16-bit index range.
                for (unsigned index = 0; index < rectangles; ++index)
                {
                    const float x = float(index % 100) * 12, y = float(index / 100) * 6;
                    list->AddRectFilled({x, y}, {x + 8, y + 4}, IM_COL32(31, 93, 167, 255));
                }
                ImGui::Render();
                const auto rendered = Clock::now();
                build += rendered - begin;
                const auto before_count = allocations.count, before_bytes = allocations.bytes;
                snapshot.clear();
                assert(snapshot.capture(*ImGui::GetDrawData()));
                capture += Clock::now() - rendered;
                growth_count += allocations.count - before_count;
                growth_bytes += allocations.bytes - before_bytes;
                retained = 0;
                for (const auto *draw : snapshot.drawData().CmdLists)
                {
                    copied += draw->VtxBuffer.Size * sizeof(ImDrawVert) + draw->IdxBuffer.Size * sizeof(ImDrawIdx) +
                              draw->CmdBuffer.Size * sizeof(ImDrawCmd);
                    retained += draw->VtxBuffer.Capacity * sizeof(ImDrawVert) +
                                draw->IdxBuffer.Capacity * sizeof(ImDrawIdx) +
                                draw->CmdBuffer.Capacity * sizeof(ImDrawCmd);
                    checksum += draw->VtxBuffer.Size + draw->IdxBuffer.Size + draw->CmdBuffer.Size;
                }
            }
            if (warm)
            {
                assert(growth_count == 0);
            }
            const auto us = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
            std::cout << "MEASURE snapshot rectangles=" << rectangles << " phase=" << (warm ? "warm" : "growth")
                      << " frames=" << frames << " build_us=" << us(build) << " capture_us=" << us(capture)
                      << " copied_bytes=" << copied << " imgui_capture_allocations=" << growth_count
                      << " imgui_capture_allocated_bytes=" << growth_bytes << " retained_buffer_bytes=" << retained
                      << " checksum=" << checksum << '\n';
        }
    }
    ImGui::DestroyContext(context);
    // ImGui allocator hooks observe the actual ImGui allocation points,
    // including linked-library calls; they do not claim all STL/DLL allocations.
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--cost")
    {
        return measure();
    }
    using lux::ui::detail::ImGuiDrawDataSnapshot;
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {256, 128};
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled({8, 8}, {120, 100}, IM_COL32(255, 64, 32, 255));
    ImGui::Render();
    auto *input = ImGui::GetDrawData();
    assert(input->CmdListsCount == 1);
    ImGuiDrawDataSnapshot snapshot;
    assert(snapshot.capture(*input));
    const auto *list = snapshot.drawData().CmdLists[0];
    const auto *vertices = list->VtxBuffer.Data;
    const auto *indices = list->IdxBuffer.Data;
    const auto *commands = list->CmdBuffer.Data;
    assert(list->_Data == nullptr);
    assert(snapshot.drawData().OwnerViewport == nullptr);
    for (int iteration = 0; iteration != 100; ++iteration)
    {
        snapshot.clear();
        assert(snapshot.capture(*input));
        assert(snapshot.drawData().CmdLists[0] == list);
        assert(list->VtxBuffer.Data == vertices);
        assert(list->IdxBuffer.Data == indices);
        assert(list->CmdBuffer.Data == commands);
    }
    input->CmdLists[0]->CmdBuffer[0].UserCallback = [](const ImDrawList *, const ImDrawCmd *) {};
    assert(!snapshot.capture(*input));
    assert(list->CmdBuffer[0].UserCallback == nullptr); // Original output preserved.
    input->CmdLists[0]->CmdBuffer[0].UserCallback = ImDrawCallback_ResetRenderState;
    assert(snapshot.capture(*input));
    input->CmdLists[0]->CmdBuffer[0].UserCallback = nullptr;
    assert(snapshot.capture(*input));
    ImGui::DestroyContext(context);
    assert(ImGui::GetCurrentContext() == nullptr);
    assert(snapshot.drawData().Valid && list->VtxBuffer.Size != 0);
    ImGuiDrawDataSnapshot moved(std::move(snapshot));
    assert(!snapshot.drawData().Valid);
    assert(moved.drawData().CmdLists[0] == list);
    snapshot = std::move(moved);
    assert(!moved.drawData().Valid);
    assert(snapshot.drawData().CmdLists[0] == list);
    std::cout << "PASS snapshot: stable capacities, callback rejection, detached Context, moves\n";
}
