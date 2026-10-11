#include "F4Fill.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"
#include "RuntimeBackend.hpp"
#include <chrono>
#include <map>
#include <vk_mem_alloc.h>

using namespace native_graph_test;

namespace
{
    const lux::render::detail::RuntimeBackendState* observed{};
    std::map<std::string, std::uint64_t> created, destroyed;
    unsigned fail_image_after{};
    unsigned fail_descriptor_after{};

    void sample(const char* phase, const VulkanBackend& backend, std::uint32_t actual_views)
    {
        VmaTotalStatistics memory{};
        vmaCalculateStatistics(backend.allocator().native(), &memory);
        std::uint64_t variants = 0, slots = 0;
        if (observed)
        {
            for (const auto& variant : observed->variants)
            {
                if (variant)
                {
                    ++variants;
                    slots += variant->targets.size();
                }
            }
        }
        std::printf(
            "{\"phase\":\"%s\",\"views\":%u,\"variants\":%llu,\"slots\":%llu,"
            "\"compilations\":%llu,\"allocations\":%u,\"used_bytes\":%llu,\"reserved_bytes\":%llu,"
            "\"images_created\":%llu,\"buffers_created\":%llu,\"pools_created\":%llu,"
            "\"sets_allocated\":%llu}\n",
            phase,
            actual_views,
            variants,
            slots,
            observed ? observed->progress.native_compilations : 0,
            memory.total.statistics.allocationCount,
            memory.total.statistics.allocationBytes,
            memory.total.statistics.blockBytes,
            created["image"],
            created["buffer"],
            created["descriptor_pool"],
            created["descriptor_set"]
        );
    }
} // namespace

namespace lux::render::detail
{
    void observeRuntimeFootprint(const RuntimeBackendState& backend) noexcept
    {
        observed = &backend;
    }
} // namespace lux::render::detail

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (std::string_view{operation} == "image" && fail_image_after != 0 && --fail_image_after == 0)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (std::string_view{operation} == "descriptor_pool" && fail_descriptor_after != 0 &&
            --fail_descriptor_after == 0)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        if (result == VK_SUCCESS)
        {
            ++created[operation];
        }
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        ++::destroyed[operation];
    }
} // namespace lux::render::vulkan::test

int main(int argc, char** argv)
{
    CHECK(argc == 6);
    const auto width = static_cast<std::uint32_t>(std::stoul(argv[2]));
    const auto height = static_cast<std::uint32_t>(std::stoul(argv[3]));
    const auto fif = static_cast<std::uint32_t>(std::stoul(argv[4]));
    const bool second_extent = std::stoul(argv[5]) != 0;
    Validation validation;
    {
        RuntimeNativeOptions native;
        native.instance = {{}, true, diagnostic, &validation};
        native.capacity.views = 8;
        native.capacity.frames_in_flight = fif;
        auto builder = checked(RenderRuntimeBuilder::create(native));
        auto& backend = builder.backend();
        const auto target = checked(backend.addOffscreenTarget(width, height));
        RenderGraphBuilder graph;
        TextureDesc texture;
        texture.extent_kind = EExtentKind::TARGET_RELATIVE;
        F4Fill fill;
        fill.output.texture = graph.importTarget("footprint.output", texture, renderTargetSemanticId("f5c.output"));
        const auto compute =
            checked(graph.compute("footprint.fill", {{"F4Fill", "default"}}, EExecutionScope::TARGET, fill));
        F4HostRead read;
        read.source.texture = fill.output.texture;
        const auto host = checked(graph.readback("footprint.read", EExecutionScope::TARGET, read));
        CHECK(graph.source(passKey("footprint.read"), "source", PassProducer{passKey("footprint.fill")}));
        auto definition = checked(std::move(graph).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(backend.device(), logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), argv[1], true)
        );
        std::vector<NativePassCommand> commands{
            {compute, DispatchCommand{width, height, 1}, EQueueRole::COMPUTE},
            {host, CopyCommand{}, EQueueRole::TRANSFER}
        };
        const auto path =
            checked(backend.addPath(target, {std::move(logical), std::move(programs), std::move(commands), values, {}})
            );
        const auto scene = checked(backend.addScene());
        sample("path", backend, 0);
        std::vector<RuntimeView> views;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto start = std::chrono::steady_clock::now();
            views.push_back(checked(backend.addView(scene, target, path, values)));
            const auto elapsed = std::chrono::steady_clock::now() - start;
            std::printf(
                "{\"add_view_ns\":%lld}\n",
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
            );
            sample("view", backend, i + 1);
            CHECK(observed);
            CHECK(
                observed->variants[observed->views.find(views.back().local)->variant]->targets.size() == (i + 1) * fif
            );
        }
        // A partially created growth candidate must free its image and preserve
        // both the committed storage and all existing semantic Views.
        VmaTotalStatistics before{};
        vmaCalculateStatistics(backend.allocator().native(), &before);
        fail_image_after = 2;
        auto failed = backend.addView(scene, target, path, values);
        CHECK(!failed && fail_image_after == 0);
        VmaTotalStatistics after{};
        vmaCalculateStatistics(backend.allocator().native(), &after);
        CHECK(before.total.statistics.allocationBytes == after.total.statistics.allocationBytes);
        CHECK(before.total.statistics.allocationCount == after.total.statistics.allocationCount);
        CHECK(observed->views.size() == 4);
        CHECK(observed->variants[observed->views.find(views[0].local)->variant]->targets.size() == 4 * fif);
        sample("failed_growth_rolled_back", backend, 4);
        fail_descriptor_after = 2;
        auto native_failed = backend.addView(scene, target, path, values);
        CHECK(!native_failed && fail_descriptor_after == 0);
        vmaCalculateStatistics(backend.allocator().native(), &after);
        CHECK(before.total.statistics.allocationBytes == after.total.statistics.allocationBytes);
        CHECK(before.total.statistics.allocationCount == after.total.statistics.allocationCount);
        CHECK(observed->views.size() == 4);
        sample("failed_descriptor_growth_rolled_back", backend, 4);
        if (second_extent)
        {
            const auto other = checked(backend.addOffscreenTarget(width / 2, height / 2));
            CHECK(backend.addView(scene, other, path, values));
            sample("second_extent", backend, 5);
        }
        if (width <= 64)
        {
            auto& state = const_cast<lux::render::detail::RuntimeBackendState&>(*observed);
            const auto frame = [&](std::uint64_t serial)
            {
                CHECK(lux::render::detail::drainRuntimeNative(state));
                CHECK(lux::render::detail::executeRuntimeFrame(state, serial, serial * 8'333'333));
                CHECK(lux::render::detail::drainRuntimeNative(state));
                for (const auto& view : state.views)
                {
                    CHECK(view.latest);
                    std::array<float, 4> pixel{};
                    CHECK(state.variants[view.variant]
                              ->invocations
                              .readbackPass(*view.latest, host, 0, 0, std::as_writable_bytes(std::span{pixel})));
                    CHECK((pixel == std::array{1.0f, 2.0f, 3.0f, 1.0f}));
                }
            };
            frame(1);
            // Removal leaves GPU backing in its native owner. Once completion
            // is proven, a new View reuses the same group, not the global SlotMap index.
            const auto old_first = state.views.find(views[1].local)->invocation_first;
            CHECK(lux::render::detail::applyRuntimeControl(
                state,
                {state.owner, views[1].local.index, views[1].local.gen, 0, 0, ERuntimeControl::REMOVE_VIEW}
            ));
            views[1] = checked(backend.addView(scene, target, path, values));
            CHECK(state.views.find(views[1].local)->invocation_first == old_first);
            frame(2);
            const auto old_variant = state.views.find(views[0].local)->variant;
            // A failed resize keeps View mapping, extent, and old output intact.
            fail_image_after = 2;
            auto resize_failed = lux::render::detail::applyRuntimeControl(
                state,
                {state.owner,
                 target.local.index,
                 target.local.gen,
                 width + 1,
                 height + 1,
                 ERuntimeControl::RESIZE_TARGET}
            );
            CHECK(!resize_failed && fail_image_after == 0);
            CHECK(state.views.find(views[0].local)->variant == old_variant);
            CHECK(state.targets.find(target.local)->width == width);
            frame(3);
            CHECK(lux::render::detail::applyRuntimeControl(
                state,
                {state.owner,
                 target.local.index,
                 target.local.gen,
                 width / 2,
                 height / 2,
                 ERuntimeControl::RESIZE_TARGET}
            ));
            frame(4);
            CHECK(lux::render::detail::applyRuntimeControl(
                state,
                {state.owner, target.local.index, target.local.gen, width, height, ERuntimeControl::RESIZE_TARGET}
            ));
            frame(5);
            CHECK(state.views.find(views[0].local)->variant == old_variant);
            for (std::uint32_t i = 0; i < 10; ++i)
            {
                const auto started = std::chrono::steady_clock::now();
                CHECK(lux::render::detail::applyRuntimeControl(
                    state,
                    {state.owner, target.local.index, target.local.gen, 16 + i, 16, ERuntimeControl::RESIZE_TARGET}
                ));
                std::printf(
                    "{\"cold_resize_ns\":%lld,\"width\":%u}\n",
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count(),
                    16 + i
                );
                frame(6 + i);
            }
            CHECK(
                std::count_if(state.variants.begin(), state.variants.end(), [](const auto& v) { return bool(v); }) <=
                state.capacity.native_variants
            );
            CHECK(created["pipeline"] == 1 && created["pipeline_layout"] == 1);
            sample("reuse_resize_oracles", backend, static_cast<std::uint32_t>(state.views.size()));
        }
        // This measurement isolates real cold physical allocation. Rendering,
        // readback and warm-path oracles are separate tests, not inferred here.
    }
    CHECK(validation.errors == 0);
    for (const auto& [operation, count] : created)
    {
        std::printf(
            "{\"native_operation\":\"%s\",\"created\":%llu,\"destroyed\":%llu}\n",
            operation.c_str(),
            count,
            destroyed[operation]
        );
    }
}
