#include <lux/engine/function/render/features/BuiltinKernels.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    void require(bool value, const char* message) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::abort();
        }
    }
} // namespace

int main()
{
    using namespace lux::render;
    auto& registry = KernelRegistry::instance();
    require(registry.idOf("MeshCull") == kInvalidKernelId, "no static registrar");
    const auto declarations = builtinKernelDeclarations();
    require(declarations.size() == 12, "exact original builtin count");
    auto prior = FrameExtensionRegistry::instance().registerSlot("third_party_first");
    require(prior.has_value(), "nonbuiltin first registration");
    require(registry.registerKernels(declarations).has_value(), "explicit strong-referenced builtin table");
    require(registry.registerKernels(declarations).has_value(), "repeated composition");
    std::size_t count{};
    registry.forEach([&](KernelTypeId, const RegisteredKernel&) { ++count; });
    require(count == 12, "no duplicate kernels");
    for (const auto& declaration : declarations)
    {
        require(registry.idOf(declaration.canonical_name) != kInvalidKernelId, "every original name retained");
    }
    auto& slots = FrameExtensionRegistry::instance();
    require(
        slots.idOf("mesh_instance") == 2 && slots.idOf("shadow") == 3,
        "shared authority despite registration order"
    );
    RGFrameContext frame;
    while (slots.idOf("last_slot") == kInvalidExtSlot)
    {
        static unsigned next{};
        auto slot = slots.registerSlot(next == 28 ? "last_slot" : "fill_" + std::to_string(next));
        require(slot.has_value(), "prepare last legal slot");
        ++next;
    }
    require(slots.idOf("last_slot") == kMaxFrameExtensionSlots, "last slot is 32");
    frame.ext_data[slots.idOf("last_slot")] = &frame;
    require(
        frame.ext_data.back() == &frame && frame.ext_data.front() == nullptr,
        "slot zero reserved, slot32 in bounds"
    );
    for (const auto* name :
         {"FullscreenQuad", "SkyboxDraw", "GridDraw", "TonemapPass", "DeferredLighting", "DepthPrepass"})
    {
        RGCompiledGraph graph;
        RGCompiledPass pass;
        ExecutionProgram output;
        ProgramEmitter emitter{output, graph};
        registry.find(registry.idOf(name))->descriptor.emit(emitter, 0, pass, graph);
        require(output.commands.size() == 1 && output.command_data.size() == 16, "original compiled output dimensions");
        require(output.commands[0].type == ExecutionProgram::Command::EType::DRAW_DIRECT, "original native opcode");
        std::array<std::uint32_t, 4> payload;
        std::memcpy(payload.data(), output.command_data.data(), sizeof(payload));
        require(payload == std::array<std::uint32_t, 4>{3, 1, 0, 0}, "exact original fullscreen draw payload");
        std::printf("%s: DRAW_DIRECT %u %u %u %u\n", name, payload[0], payload[1], payload[2], payload[3]);
    }
    std::puts("PASS: 12 explicit kernels, shared slots, complete frame array and unchanged native command payloads");
}
