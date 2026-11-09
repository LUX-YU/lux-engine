#include <lux/engine/render/graph/CompiledPrograms.hpp>
#include <lux/engine/render/graph/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <unordered_set>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        template <class T> void combineHash(std::size_t& seed, const T& value) noexcept
        {
            seed ^= std::hash<T>{}(value) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        }

        std::size_t programHash(const rdesc::SpirvArtifact& shader) noexcept
        {
            std::size_t result = 0;
            for (const auto& stage : shader.stages)
            {
                combineHash(result, stage.stage);
                combineHash(result, stage.entry);
                for (auto word : stage.words) combineHash(result, word);
            }
            // Exact comparison includes reflected ABI, even when identical code hashes into this bucket.
            return result;
        }

        std::size_t programHash(const RasterProgramKey& key) noexcept
        {
            auto result = programHash(key.shader);
            for (const auto& binding : key.state.vertex_bindings)
            {
                combineHash(result, binding.binding);
                combineHash(result, binding.stride);
                combineHash(result, binding.rate);
            }
            for (const auto& attribute : key.state.vertex_attributes)
            {
                combineHash(result, attribute.location);
                combineHash(result, attribute.binding);
                combineHash(result, attribute.offset);
                combineHash(result, attribute.format);
            }
            const auto& state = key.state;
            combineHash(result, state.depth_test);
            combineHash(result, state.depth_write);
            combineHash(result, state.depth_compare);
            combineHash(result, state.cull_mode);
            combineHash(result, state.front_counter_clockwise);
            combineHash(result, state.depth_bias_constant);
            combineHash(result, state.depth_bias_slope);
            combineHash(result, state.blend.enabled);
            combineHash(result, state.blend.source_color);
            combineHash(result, state.blend.destination_color);
            combineHash(result, state.blend.source_alpha);
            combineHash(result, state.blend.destination_alpha);
            for (auto color : key.colors) combineHash(result, color);
            combineHash(result, key.depth);
            combineHash(result, key.samples);
            return result;
        }
    }

    struct CompiledPrograms::Program final
    {
        using VKey = std::variant<rdesc::SpirvArtifact, RasterProgramKey>;
        VKey key;
        nri::Pipeline* pipeline;
        Layout* layout;
        std::uint32_t users = 0;
        std::uint64_t last_acquired = 0;
        SubmissionFrontiers last_uses;
    };

    struct CompiledPrograms::Layout final
    {
        std::vector<nri::DescriptorRangeDesc> ranges;
        std::uint32_t parameter_bytes;
        nri::StageBits parameter_stages;
        std::array<std::uint32_t, 3> capacities;
        nri::PipelineLayout* native = nullptr;
        std::uint32_t references = 0;

        bool operator==(const Layout& other) const noexcept
        {
            const bool same_parameters = parameter_bytes == other.parameter_bytes &&
                parameter_stages == other.parameter_stages;
            const bool same_ranges = std::ranges::equal(ranges, other.ranges, [](const auto& a, const auto& b) noexcept
            {
                return a.baseRegisterIndex == b.baseRegisterIndex && a.descriptorNum == b.descriptorNum &&
                    a.descriptorType == b.descriptorType && a.shaderStages == b.shaderStages && a.flags == b.flags;
            });
            return same_parameters && capacities == other.capacities && same_ranges;
        }

        std::size_t hash() const noexcept
        {
            std::size_t result = 0;
            combineHash(result, parameter_bytes);
            combineHash(result, parameter_stages);
            for (auto capacity : capacities) combineHash(result, capacity);
            for (const auto& range : ranges)
            {
                combineHash(result, range.baseRegisterIndex);
                combineHash(result, range.descriptorNum);
                combineHash(result, range.descriptorType);
                combineHash(result, range.shaderStages);
                combineHash(result, range.flags);
            }
            return result;
        }
    };

    CompiledPrograms::CompiledPrograms(DeviceSession& session, std::uint32_t unused_limit) noexcept
        : session_(session), unused_limit_(unused_limit)
    {
    }

    CompiledPrograms::~CompiledPrograms() noexcept
    {
        // clear() transfers GPU objects to Session cleanup before its admission closes.
        if (!programs_.empty() || !layouts_.empty()) std::terminate();
    }

    CompiledPrograms::Layout CompiledPrograms::layoutKey(const PassBindings& bindings) const noexcept
    {
        constexpr std::array kinds{nri::DescriptorType::TEXTURE, nri::DescriptorType::SAMPLER,
            nri::DescriptorType::STORAGE_STRUCTURED_BUFFER};
        Layout result{descriptorRanges(bindings.private_bindings), bindings.parameter_bytes,
            bindings.parameter_stages, {}};
        for (std::size_t kind = 0; kind < kinds.size(); ++kind)
            if (bindings.shared_sets[kind] != kGraphInvalidIndex)
                result.capacities[kind] = session_.descriptors().capacity(kinds[kind]);
        return result;
    }

    RenderResult<nri::PipelineLayout*>
    CompiledPrograms::acquireLayout(PassBindings& bindings, std::uint32_t color_count) noexcept
    {
        auto candidate = layoutKey(bindings);
        for (std::size_t kind = 0; kind < candidate.capacities.size(); ++kind)
            if (bindings.shared_sets[kind] != kGraphInvalidIndex && candidate.capacities[kind] == 0)
                return cxx::unexpected(RenderError{kDeviceUnsupported});
        // A new raster program may reuse a Layout but add more color attachments.
        // Those attachments participate in fragment limits without becoming Layout identity.
        std::uint64_t ordinary = color_count;
        for (const auto& binding : bindings.private_bindings)
            if (binding.kind != EDescriptorKind::SAMPLER && (binding.stages & 2u) != 0) ordinary += binding.count;
        const auto combined = ordinary + candidate.capacities[0] + candidate.capacities[2];
        const auto& limits = session_.description().shaderStage;
        const auto combined_limit = std::max(limits.resourceMaxNum, limits.updateAfterSet.resourceMaxNum);
        const bool excessive_resources = ordinary > limits.resourceMaxNum || combined > combined_limit;
        if (excessive_resources)
            return cxx::unexpected(RenderError{kDeviceCapacity, {combined, combined_limit, ordinary}});
        const auto hash = candidate.hash();
        const auto bucket = layout_index_.find(hash);
        if (bucket != layout_index_.end()) for (auto* layout : bucket->second)
        {
            if (*layout == candidate)
            {
                ++layout->references;
                bindings.layout = layout->native;
                return layout->native;
            }
        }
        auto created = createPipelineLayout(session_.device(), session_.nriCore(), bindings,
            session_.enabledFeatures(), candidate.capacities, color_count);
        if (!created) return cxx::unexpected(created.error());
        candidate.native = *created;
        candidate.references = 1;
        auto value = std::make_unique<Layout>(std::move(candidate));
        layout_index_[hash].push_back(value.get());
        layouts_.push_back(std::move(value));
        bindings.layout = *created;
        return *created;
    }

    void CompiledPrograms::dropLayout(Layout& layout) noexcept
    {
        if (!layout.references) std::terminate();
        if (--layout.references != 0) return;
        const auto hash = layout.hash();
        auto found = layout_index_.find(hash);
        std::erase(found->second, &layout);
        if (found->second.empty()) layout_index_.erase(found);
        session_.nriCore().DestroyPipelineLayout(layout.native);
        std::erase_if(layouts_, [&](const auto& value) noexcept { return value.get() == &layout; });
    }

    void CompiledPrograms::releaseLayout(nri::PipelineLayout* native) noexcept
    {
        const auto found = std::ranges::find_if(layouts_, [&](const auto& layout) noexcept
        { return layout->native == native; });
        if (found == layouts_.end()) std::terminate();
        dropLayout(**found);
    }

    template <class Key> nri::Pipeline* CompiledPrograms::acquire(const Key& key, PassBindings& bindings) noexcept
    {
        const auto shape = layoutKey(bindings);
        auto hash = programHash(key);
        combineHash(hash, shape.hash());
        const auto found = program_index_.find(hash);
        if (found == program_index_.end()) return nullptr;
        for (auto* program : found->second)
        {
            const auto* stored = std::get_if<Key>(&program->key);
            if (stored && *stored == key && *program->layout == shape)
            {
                ++program->users;
                program->last_acquired = ++acquisition_serial_;
                bindings.layout = program->layout->native;
                return program->pipeline;
            }
        }
        return nullptr;
    }

    nri::Pipeline* CompiledPrograms::acquireRaster(const RasterProgramKey& key, PassBindings& bindings) noexcept
    {
        return acquire(key, bindings);
    }

    nri::Pipeline* CompiledPrograms::acquireCompute(const rdesc::SpirvArtifact& key, PassBindings& bindings) noexcept
    {
        return acquire(key, bindings);
    }

    template <class Key>
    nri::Pipeline* CompiledPrograms::publish(Key key, nri::PipelineLayout* native, nri::Pipeline* pipeline) noexcept
    {
        const auto found = std::ranges::find_if(layouts_, [&](const auto& layout) noexcept
        { return layout->native == native; });
        if (found == layouts_.end()) std::terminate();
        auto* layout = found->get();
        auto hash = programHash(key);
        combineHash(hash, layout->hash());
        auto& bucket = program_index_[hash];
        for (auto* program : bucket)
        {
            const auto* stored = std::get_if<Key>(&program->key);
            if (stored && *stored == key && program->layout == layout)
            {
                session_.nriCore().DestroyPipeline(pipeline);
                dropLayout(*layout);
                ++program->users;
                program->last_acquired = ++acquisition_serial_;
                return program->pipeline;
            }
        }
        auto program = std::make_unique<Program>(Program{std::move(key), pipeline, layout,
            1, ++acquisition_serial_, {}});
        bucket.push_back(program.get());
        programs_.push_back(std::move(program));
        trim();
        return pipeline;
    }

    nri::Pipeline*
    CompiledPrograms::publishRaster(RasterProgramKey key, nri::PipelineLayout* layout, nri::Pipeline* pipeline) noexcept
    {
        return publish(std::move(key), layout, pipeline);
    }

    nri::Pipeline*
    CompiledPrograms::publishCompute(
        rdesc::SpirvArtifact key,
        nri::PipelineLayout* layout,
        nri::Pipeline* pipeline
    ) noexcept
    {
        return publish(std::move(key), layout, pipeline);
    }

    void
    CompiledPrograms::release(
        std::span<nri::Pipeline* const> pipelines,
        std::span<nri::PipelineLayout* const> direct_layouts
    ) noexcept
    {
        SubmissionFrontiers accepted;
        for (std::size_t q = 0; q < session_.queueCount(); ++q)
            accepted.serials[q] = session_.physicalQueue(q).submitted();
        for (auto* native : pipelines)
        {
            const auto found = std::ranges::find_if(programs_, [&](const auto& value) noexcept
            { return value->pipeline == native; });
            const bool unowned_pipeline = found == programs_.end() || !(*found)->users;
            if (unowned_pipeline) std::terminate();
            auto& program = **found;
            for (std::size_t q = 0; q < session_.queueCount(); ++q)
                program.last_uses.serials[q] = std::max(program.last_uses.serials[q], accepted.serials[q]);
            --program.users;
        }
        for (auto* native : direct_layouts)
            session_.deferRelease(accepted, [this, native]() noexcept { releaseLayout(native); });
        trim();
    }

    void CompiledPrograms::trim() noexcept
    {
        std::vector<Program*> unused;
        for (const auto& program : programs_)
            if (!program->users) unused.push_back(program.get());
        if (unused.size() <= unused_limit_) return;
        std::ranges::sort(unused, {}, &Program::last_acquired);
        unused.resize(unused.size() - unused_limit_);
        const std::unordered_set<Program*> removed(unused.begin(), unused.end());
        std::erase_if(programs_, [&](auto& program) noexcept
        {
            if (!removed.contains(program.get())) return false;
            auto hash = std::visit([](const auto& key) noexcept { return programHash(key); }, program->key);
            combineHash(hash, program->layout->hash());
            auto position = program_index_.find(hash);
            std::erase(position->second, program.get());
            if (position->second.empty()) program_index_.erase(position);
            const auto frontiers = program->last_uses;
            session_.deferRelease(frontiers, [this, value = std::move(program)]() noexcept
            {
                session_.nriCore().DestroyPipeline(value->pipeline);
                // No new cleanup task here: this callback is already past every accepted GPU use.
                dropLayout(*value->layout);
            });
            return true;
        });
    }

    void CompiledPrograms::clear() noexcept
    {
        for (const auto& program : programs_) if (program->users) std::terminate();
        unused_limit_ = 0;
        trim();
    }
} // namespace lux::render::detail
