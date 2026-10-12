#include "GraphNative.hpp"
#include <algorithm>
#include <chrono>

namespace lux::render::vulkan
{
    namespace detail
    {
        NativeGraphCode::NativeGraphCode(
            const NativeCompileInputs& inputs,
            std::shared_ptr<const std::vector<NativeShaderProgram>> candidates
        ) noexcept
            : logical(inputs.logical), device(inputs.device.native()), queues(inputs.queues),
              program_owner(std::move(candidates)), programs(*program_owner), target_extent(inputs.target_extent),
              buffer_boundaries(inputs.buffer_boundaries.begin(), inputs.buffer_boundaries.end())
        {
        }

        NativeGraphBacking::NativeGraphBacking(
            std::shared_ptr<const NativeGraphCode> compiled,
            const NativeInvocationInputs& inputs
        ) noexcept
            : code(std::move(compiled)), samplers(inputs.samplers.begin(), inputs.samplers.end())
        {
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

    static RenderResult<std::shared_ptr<const detail::NativeGraphCode>> compileNativeCode(
        const NativeCompileInputs& inputs,
        std::shared_ptr<const std::vector<NativeShaderProgram>> programs
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
        for (auto* queue : inputs.queues)
        {
            if (!queue || queue->device() != inputs.device.native())
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
        }
        for (const auto& boundary : inputs.buffer_boundaries)
        {
            if (!boundary.resource.isValid() || boundary.resource.value() > identity.resources.size())
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& resource = identity.resources[boundary.resource.value() - 1];
            const bool invalid_boundary =
                resource.kind() != EGraphResourceKind::BUFFER || resource.origin != EGraphResourceOrigin::IMPORTED ||
                boundary.byte_offset == 0 || boundary.byte_offset >= resource.buffer().byte_size;
            if (invalid_boundary)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        std::vector<bool> covered(identity.passes.size());
        for (const auto& program : *programs)
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
        auto result = std::make_shared<detail::NativeGraphCode>(inputs, std::move(programs));
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
        return std::shared_ptr<const detail::NativeGraphCode>{std::move(result)};
    }

    RenderResult<Graph> compileVulkanGraph(
        const NativeCompileInputs& inputs,
        std::vector<NativeShaderProgram> programs
    ) noexcept
    {
        auto code =
            compileNativeCode(inputs, std::make_shared<const std::vector<NativeShaderProgram>>(std::move(programs)));
        if (!code)
        {
            return cxx::unexpected(code.error());
        }
        return Graph{std::move(*code)};
    }

    RenderResult<Graph> specializeVulkanGraph(
        const NativeCompileInputs& inputs,
        const Graph& shaders
    ) noexcept
    {
        auto specialized = inputs;
        if (specialized.buffer_boundaries.empty())
        {
            specialized.buffer_boundaries = shaders.code_->buffer_boundaries;
        }
        auto code = compileNativeCode(specialized, shaders.code_->program_owner);
        if (!code)
        {
            return cxx::unexpected(code.error());
        }
        return Graph{std::move(*code)};
    }

    RenderResult<GraphExecutionContext> GraphExecutionContext::create(
        const Graph& plan,
        const NativeInvocationInputs& inputs
    ) noexcept
    {
        const bool wrong_owner = inputs.device.native() != plan.device() || inputs.allocator.device() != plan.device();
        const bool wrong_capacity = inputs.capacity == 0;
        const bool wrong_inputs = inputs.imports.size() != plan.logical().imports().size() ||
                                  inputs.initial_values.passes.size() != plan.logical().identity().passes.size();
        if (wrong_owner || wrong_capacity || wrong_inputs)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (std::size_t i = 0; i < inputs.imports.size(); ++i)
        {
            if (inputs.imports[i].resource != plan.logical().imports()[i])
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        auto result = std::make_unique<detail::NativeGraphBacking>(plan.code_, inputs);
        const auto started = std::chrono::steady_clock::now();
        auto allocated = detail::allocateInvocations(*result, inputs);
        const auto allocated_at = std::chrono::steady_clock::now();
        result->statistics.compile_ns[1] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(allocated_at - started).count();
        if (!allocated)
        {
            return cxx::unexpected(allocated.error());
        }
        auto assembled = detail::assembleInvocations(*result, inputs);
        result->statistics.compile_ns[2] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - allocated_at)
                .count();
        if (!assembled)
        {
            return cxx::unexpected(assembled.error());
        }
        return GraphExecutionContext{std::move(result)};
    }

    RenderResult<std::uint32_t> GraphExecutionContext::append(const NativeInvocationInputs& inputs) noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        const bool exceeds_capacity = inputs.capacity > UINT32_MAX - backing_->slots.size();
        const bool different_samplers =
            inputs.samplers.size() != backing_->samplers.size() ||
            !std::equal(
                inputs.samplers.begin(),
                inputs.samplers.end(),
                backing_->samplers.begin(),
                [](const auto& a, const auto& b) { return a.first == b.first && &a.second.get() == &b.second.get(); }
            );
        if (exceeds_capacity || different_samplers)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto candidate = create(Graph{backing_->code}, inputs);
        if (!candidate)
        {
            return cxx::unexpected(candidate.error());
        }
        const auto first = static_cast<std::uint32_t>(backing_->slots.size());
        backing_->slots.reserve(first + inputs.capacity);
        for (auto& slot : candidate->backing_->slots)
        {
            backing_->slots.push_back(std::move(slot));
        }
        const auto& added = candidate->backing_->statistics;
        backing_->statistics.allocations += added.allocations;
        backing_->statistics.allocated_bytes += added.allocated_bytes;
        backing_->statistics.aliased_resources += added.aliased_resources;
        for (std::size_t i = 0; i < added.compile_ns.size(); ++i)
        {
            backing_->statistics.compile_ns[i] += added.compile_ns[i];
        }
        return first;
    }

} // namespace lux::render::vulkan
