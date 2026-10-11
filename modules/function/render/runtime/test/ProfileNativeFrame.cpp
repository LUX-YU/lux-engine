// Test-only timing wrapper; all validation/preparation remains the production algorithm.
#define prepareSlot prepareSlotUninstrumented
#include "../../vulkan/src/graph/Frame.cpp"
#undef prepareSlot
#include <chrono>

void recordRuntimeImportProfile(double nanoseconds) noexcept;

namespace lux::render::vulkan::detail
{
    RenderResult<void> prepareSlot(
        NativeGraphBacking& plan,
        GraphSlot& slot,
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports,
        const GraphSlot* scene_source
    ) noexcept
    {
        const auto start = std::chrono::steady_clock::now();
        auto result = prepareSlotUninstrumented(plan, slot, bindings, imports, scene_source);
        recordRuntimeImportProfile(
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count()
        );
        return result;
    }
} // namespace lux::render::vulkan::detail
