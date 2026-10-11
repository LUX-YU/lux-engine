#include "GraphNative.hpp"
#include <algorithm>
#include <chrono>

namespace lux::render::vulkan
{
    namespace detail
    {
        NativeGraphBacking::NativeGraphBacking(
            const NativeCompileInputs& inputs,
            std::vector<NativeShaderProgram> candidates
        ) noexcept
            : logical(inputs.logical), device(inputs.device.native()), queues(inputs.queues),
              programs(std::move(candidates)), imports(inputs.imports.begin(), inputs.imports.end())
        {
            samplers.assign(inputs.samplers.begin(), inputs.samplers.end());
            for (auto& import : imports)
            {
                import.views = {}; // never persist the caller's span
                import.ranges = {};
            }
        }

        NativeGraphBacking::~NativeGraphBacking() noexcept
        {
            // Includes partially submitted candidates: every accepted pass pins
            // the entire composite until its own real completion or device loss.
            for (auto& slot : slots)
            {
                for (const auto& ticket : slot.queue_tickets)
                {
                    if (!ticket)
                    {
                        continue;
                    }
                    auto& queue = const_cast<SubmissionQueue&>(ticket->owner());
                    auto done = queue.wait(*ticket, UINT64_MAX);
                    if ((!done || !*done) && !queue.deviceLost())
                    {
                        std::terminate();
                    }
                }
            }
        }
    } // namespace detail

    RenderResult<ExecutableGraphPlan> compileVulkanGraph(
        const NativeCompileInputs& inputs,
        std::vector<NativeShaderProgram> programs
    ) noexcept
    {
        const auto started = std::chrono::steady_clock::now();
        auto previous = started;
        const auto elapsed = [&]() noexcept
        {
            const auto now = std::chrono::steady_clock::now();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
            previous = now;
            return static_cast<std::uint64_t>(ns);
        };
        const auto& identity = inputs.logical.identity();
        const bool invalid_owner = inputs.allocator.device() != inputs.device.native();
        const bool invalid_capacity = inputs.frame_capacity < 2 || inputs.frame_capacity > 3;
        const bool invalid_values = inputs.initial_values.passes.size() != identity.passes.size();
        if (invalid_owner || invalid_capacity || invalid_values)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (auto* queue : inputs.queues)
        {
            if (!queue || queue->device() != inputs.device.native())
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
        }
        std::vector<bool> covered(identity.passes.size());
        for (const auto& program : programs)
        {
            const auto& key = program.identity();
            const bool invalid = key.device_scope != inputs.device.native() ||
                                 key.graph != inputs.logical.cacheIdentity() || !key.pass.isValid() ||
                                 key.pass.value() > covered.size();
            if (invalid)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto index = key.pass.value() - 1;
            if (covered[index])
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            covered[index] = true;
        }
        for (auto pass : inputs.logical.executionOrder())
        {
            const auto& declaration = identity.passes[pass.value() - 1];
            const bool needs_program =
                declaration.kind == EPassKind::GRAPHICS || declaration.kind == EPassKind::COMPUTE;
            if (covered[pass.value() - 1] != needs_program)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {pass.value()}});
            }
        }
        auto result = std::make_unique<detail::NativeGraphBacking>(inputs, std::move(programs));
        result->statistics.compile_ns[0] = elapsed();
        auto resources = detail::compileResources(*result, inputs);
        result->statistics.compile_ns[1] = elapsed();
        if (!resources)
        {
            return cxx::unexpected(resources.error());
        }
        auto recipes = detail::compileRecipes(*result, inputs);
        result->statistics.compile_ns[2] = elapsed();
        if (!recipes)
        {
            return cxx::unexpected(recipes.error());
        }
        auto sync = detail::compileSync(*result);
        result->statistics.compile_ns[3] = elapsed();
        if (!sync)
        {
            return cxx::unexpected(sync.error());
        }
        return ExecutableGraphPlan{std::move(result)};
    }
} // namespace lux::render::vulkan
