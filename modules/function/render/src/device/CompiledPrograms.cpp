#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        template <class T> void combineHash(std::size_t& seed, const T& value) noexcept
        {
            seed ^= std::hash<T>{}(value) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        }

        std::size_t rasterDigest(const RasterProgramKey& key, std::size_t result) noexcept
        {
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

    struct CompiledPrograms::Shader final
    {
        rdesc::ShaderProgramArtifact artifact;
        std::uint64_t id;
        std::size_t digest;
        std::uint32_t references = 1;
    };

    struct CompiledPrograms::Program final
    {
        std::optional<RasterProgramKey> raster;
        nri::Pipeline* pipeline;
        std::uint64_t layout_id;
        std::uint64_t shader_id;
        std::uint32_t users;
        std::uint64_t last_acquired;
        std::size_t digest;
        std::uint64_t publication;
        SubmissionFrontiers last_uses;
    };

    namespace
    {
        struct LayoutKey final
        {
            std::vector<nri::DescriptorRangeDesc> ranges;
            std::uint32_t parameter_bytes;
            nri::StageBits parameter_stages;
            std::uint32_t private_set;
            std::array<std::uint32_t, 3> shared_sets;
            std::array<nri::DescriptorRangeDesc, 3> shared;

            [[nodiscard]] bool matches(const LayoutKey& other) const noexcept
            {
                const bool same_placement = parameter_bytes == other.parameter_bytes &&
                    parameter_stages == other.parameter_stages && private_set == other.private_set &&
                    shared_sets == other.shared_sets;
                const auto same_range = [](const auto& a, const auto& b) noexcept
                {
                    return a.baseRegisterIndex == b.baseRegisterIndex && a.descriptorNum == b.descriptorNum &&
                        a.descriptorType == b.descriptorType && a.shaderStages == b.shaderStages && a.flags == b.flags &&
                        a.d3dRegisterSpacePlusOne == b.d3dRegisterSpacePlusOne && a.d3dRegisterIndex == b.d3dRegisterIndex;
                };
                return same_placement && std::ranges::equal(shared, other.shared, same_range) &&
                    std::ranges::equal(ranges, other.ranges, same_range);
            }

            [[nodiscard]] std::size_t contentDigest() const noexcept
            {
                std::size_t result = 0;
                combineHash(result, parameter_bytes);
                combineHash(result, parameter_stages);
                combineHash(result, private_set);
                for (auto set : shared_sets) combineHash(result, set);
                const auto hash_range = [&](const nri::DescriptorRangeDesc& range) noexcept
                {
                    combineHash(result, range.baseRegisterIndex);
                    combineHash(result, range.descriptorNum);
                    combineHash(result, range.descriptorType);
                    combineHash(result, range.shaderStages);
                    combineHash(result, range.flags);
                    combineHash(result, range.d3dRegisterSpacePlusOne);
                    combineHash(result, range.d3dRegisterIndex);
                };
                for (const auto& range : shared) hash_range(range);
                for (const auto& range : ranges) hash_range(range);
                return result;
            }
        };

        LayoutKey layoutKey(DeviceSession& session, const PassBindings& bindings) noexcept
        {
            constexpr std::array kinds{
                nri::DescriptorType::TEXTURE, nri::DescriptorType::SAMPLER, nri::DescriptorType::STRUCTURED_BUFFER
            };
            std::array<std::uint32_t, 3> capacities{};
            for (std::size_t kind = 0; kind < kinds.size(); ++kind)
            {
                if (bindings.shared_sets[kind] != kGraphInvalidIndex)
                    capacities[kind] = session.descriptors().capacity(kinds[kind]);
            }
            const auto& enabled = session.enabledFeatures();
            const bool d3d12 = enabled.shader_target == rdesc::EShaderTarget::DXIL;
            auto shared = DescriptorTable::layoutRanges(capacities, enabled.variable_descriptor_count);
            if (d3d12)
            {
                for (const auto& binding : bindings.bindings)
                {
                    if (binding.count != 0) continue;
                    const auto kind = binding.kind == EDescriptorKind::SAMPLED_IMAGE ? 0U :
                        (binding.kind == EDescriptorKind::SAMPLER ? 1U : 2U);
                    if (kind == 2) shared[kind].descriptorType = nri::DescriptorType::STRUCTURED_BUFFER;
                    shared[kind].d3dRegisterSpacePlusOne = binding.shader_space + 1;
                    shared[kind].d3dRegisterIndex = binding.shader_register;
                }
            }
            return {
                descriptorRanges(bindings.private_bindings, d3d12), bindings.parameter_bytes, bindings.parameter_stages,
                bindings.private_bindings.empty() ? 0u : bindings.private_set, bindings.shared_sets, shared
            };
        }
    } // namespace

    struct CompiledPrograms::Layout final
    {
        LayoutKey key;
        nri::PipelineLayout* native;
        std::uint32_t references;
        std::uint32_t max_colors;
        std::uint64_t id;
        std::size_t digest;
    };

    CompiledPrograms::CompiledPrograms(DeviceSession& session, std::uint32_t unused_limit) noexcept
        : session_(session), unused_limit_(unused_limit)
    {
    }

    CompiledPrograms::~CompiledPrograms() noexcept
    {
        // clear() transfers GPU objects to Session cleanup before its admission closes.
        const bool has_live_records = !program_index_.empty() || !unused_.empty() || !layouts_.empty() ||
            !shaders_.empty() || !layout_index_.empty() || !shader_index_.empty();
        if (has_live_records) std::terminate();
    }

    RenderResult<CompiledPrograms::Layout*>
    CompiledPrograms::acquireLayout(PassBindings& bindings, std::uint32_t color_count) noexcept
    {
        auto key = layoutKey(session_, bindings);
        const auto digest = key.contentDigest();
        const auto [first, last] = layout_index_.equal_range(digest);
        for (auto item = first; item != last; ++item)
        {
            auto& layout = *item->second;
            if (!layout.key.matches(key)) continue;
            if (color_count > layout.max_colors)
                return cxx::unexpected(deviceBudget(EDeviceBudget::COLOR_ATTACHMENTS, color_count, layout.max_colors));
            if (layout.references == UINT32_MAX) std::terminate();
            ++layout.references;
            bindings.layout = layout.native;
            return &layout;
        }
        std::uint32_t max_colors = 0;
        auto created = createPipelineLayout(
            session_.device(), session_.nriCore(), bindings, key.ranges, key.shared, color_count, max_colors
        );
        if (!created) return cxx::unexpected(created.error());
        bindings.layout = *created;
        if (layout_serial_ == UINT64_MAX) std::terminate();
        const auto id = ++layout_serial_;
        auto record = std::make_unique<Layout>(Layout{std::move(key), *created, 1, max_colors, id, digest});
        auto* value = record.get();
        layouts_.emplace(id, std::move(record));
        layout_index_.emplace(digest, value);
        return value;
    }

    void CompiledPrograms::dropLayout(Layout& layout) noexcept
    {
        if (!layout.references) std::terminate();
        if (--layout.references != 0) return;
        const auto [first, last] = layout_index_.equal_range(layout.digest);
        for (auto item = first; item != last; ++item)
        {
            if (item->second != &layout) continue;
            session_.nriCore().DestroyPipelineLayout(layout.native);
            layout_index_.erase(item);
            layouts_.erase(layout.id);
            return;
        }
        std::terminate();
    }

    void CompiledPrograms::releaseLayout(Layout* layout) noexcept
    {
        dropLayout(*layout);
    }

    CompiledPrograms::Shader& CompiledPrograms::acquireShader(rdesc::ShaderProgramArtifact artifact) noexcept
    {
        const auto digest = artifact.binaryDigest();
        const auto [first, last] = shader_index_.equal_range(digest);
        for (auto item = first; item != last; ++item)
        {
            auto& shader = *item->second;
            if (shader.artifact != artifact) continue;
            if (shader.references == UINT32_MAX) std::terminate();
            ++shader.references;
            return shader;
        }
        if (shader_serial_ == UINT64_MAX) std::terminate();
        const auto id = ++shader_serial_;
        auto record = std::make_unique<Shader>(Shader{std::move(artifact), id, digest});
        auto* value = record.get();
        shaders_.emplace(id, std::move(record));
        shader_index_.emplace(digest, value);
        return *value;
    }

    void CompiledPrograms::releaseShader(Shader& shader) noexcept
    {
        if (!shader.references) std::terminate();
        if (--shader.references != 0) return;
        const auto [first, last] = shader_index_.equal_range(shader.digest);
        for (auto item = first; item != last; ++item)
        {
            if (item->second != &shader) continue;
            shader_index_.erase(item);
            shaders_.erase(shader.id);
            return;
        }
        std::terminate();
    }

    const rdesc::ShaderProgramArtifact& CompiledPrograms::artifact(const Shader& shader) noexcept
    {
        return shader.artifact;
    }

    CompiledPrograms::Program*
    CompiledPrograms::find(const RasterProgramKey* raster, const Lookup& lookup, std::uint64_t after) noexcept
    {
        const auto found = program_index_.find(lookup.digest_);
        if (found == program_index_.end()) return nullptr;
        for (const auto& entry : found->second)
        {
            auto& program = *entry;
            const bool same_identity = program.shader_id == lookup.shader_->id &&
                program.layout_id == lookup.layout_->id;
            const bool same_kind = program.raster.has_value() == (raster != nullptr);
            if (program.publication <= after || !same_identity || !same_kind) continue;
            if (raster && !program.raster->matches(*raster)) continue;
            if (!program.users && unused_.erase(program.last_acquired) != 1) std::terminate();
            if (program.users == UINT32_MAX || acquisition_serial_ == UINT64_MAX) std::terminate();
            ++program.users;
            program.last_acquired = ++acquisition_serial_;
            return &program;
        }
        return nullptr;
    }

    CompiledPrograms::Lookup CompiledPrograms::lookup(Shader& shader, Layout& layout, const RasterProgramKey* raster)
        noexcept
    {
        Lookup result;
        result.digest_ = 0;
        combineHash(result.digest_, shader.id);
        combineHash(result.digest_, layout.id);
        combineHash(result.digest_, raster != nullptr);
        if (raster) result.digest_ = rasterDigest(*raster, result.digest_);
        result.shader_ = &shader;
        result.layout_ = &layout;
        result.publication_ = publication_serial_;
        result.hit_ = find(raster, result, 0);
        return result;
    }

    CompiledPrograms::Program*
    CompiledPrograms::publish(RasterProgramKey* raster, const Lookup& lookup, nri::Pipeline* pipeline) noexcept
    {
        // Only entries published while native creation was in flight can be new matches.
        if (auto* existing = find(raster, lookup, lookup.publication_))
        {
            session_.nriCore().DestroyPipeline(pipeline);
            return existing;
        }
        auto& layout = *lookup.layout_;
        auto& shader = *lookup.shader_;
        const bool exhausted_references = layout.references == UINT32_MAX || shader.references == UINT32_MAX;
        const bool exhausted_serials = acquisition_serial_ == UINT64_MAX || publication_serial_ == UINT64_MAX;
        if (exhausted_references || exhausted_serials) std::terminate();
        ++layout.references;
        ++shader.references;
        auto value = std::make_unique<Program>(Program{
            raster ? std::optional{std::move(*raster)} : std::nullopt, pipeline, layout.id, shader.id, 1,
            ++acquisition_serial_, lookup.digest_, ++publication_serial_, session_.makeFrontiers()
        });
        auto* program = value.get();
        program_index_[lookup.digest_].push_back(std::move(value));
        return program;
    }

    CompiledPrograms::Program*
    CompiledPrograms::publishRaster(RasterProgramKey key, const Lookup& lookup, nri::Pipeline* pipeline) noexcept
    {
        return publish(&key, lookup, pipeline);
    }

    CompiledPrograms::Program* CompiledPrograms::publishCompute(const Lookup& lookup, nri::Pipeline* pipeline) noexcept
    {
        return publish(nullptr, lookup, pipeline);
    }

    nri::Pipeline* CompiledPrograms::native(const Program& program) noexcept
    {
        return program.pipeline;
    }

    void CompiledPrograms::release(std::span<Program* const> programs) noexcept
    {
        const auto get = [&](std::size_t i) noexcept { return programs[i]; };
        release(programs.size(), get);
    }

    void CompiledPrograms::release(std::size_t count, cxx::function_ref<Program*(std::size_t) noexcept> get) noexcept
    {
        if (!count) return;
        auto accepted = session_.makeFrontiers();
        for (std::size_t q = 0; q < session_.queueCount(); ++q)
            accepted.accept(session_.submissionPoint(q, session_.physicalQueue(q).submitted()));
        for (std::size_t i = 0; i < count; ++i)
        {
            auto* program = get(i);
            if (!program->users) std::terminate();
            program->last_uses.merge(accepted);
            if (--program->users == 0) unused_.emplace(program->last_acquired, program);
        }
        trim();
    }

    void CompiledPrograms::trim() noexcept
    {
        while (unused_.size() > unused_limit_)
        {
            const auto oldest = unused_.begin();
            auto* program = oldest->second;
            unused_.erase(oldest);
            const auto index = program_index_.find(program->digest);
            const auto pipeline = program->pipeline;
            const auto layout_id = program->layout_id;
            auto& shader = *shaders_.find(program->shader_id)->second;
            auto frontiers = std::move(program->last_uses);
            std::erase_if(index->second, [program](const auto& value) noexcept { return value.get() == program; });
            if (index->second.empty()) program_index_.erase(index);
            releaseShader(shader);
            session_.deferRelease(std::move(frontiers), [this, pipeline, layout_id]() noexcept
            {
                session_.nriCore().DestroyPipeline(pipeline);
                dropLayout(*layouts_.find(layout_id)->second);
            });
        }
    }

    void CompiledPrograms::clear() noexcept
    {
        unused_limit_ = 0;
        trim();
        if (!program_index_.empty() || !shaders_.empty()) std::terminate();
    }

} // namespace lux::render::detail
