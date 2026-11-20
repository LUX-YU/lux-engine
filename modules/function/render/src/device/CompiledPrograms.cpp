#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
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

        std::size_t keyDigest(const std::shared_ptr<const PreparedShader>& key) noexcept
        {
            return key->digest;
        }

        bool matches(const std::shared_ptr<const PreparedShader>& stored,
                     const std::shared_ptr<const PreparedShader>& candidate) noexcept
        {
            return stored->matches(*candidate);
        }

        bool matches(const RasterProgramKey& stored, const RasterProgramKey& candidate) noexcept
        {
            return stored.matches(candidate);
        }

        std::size_t keyDigest(const RasterProgramKey& key) noexcept
        {
            auto result = key.shader->digest;
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
        using VKey = std::variant<std::shared_ptr<const PreparedShader>, RasterProgramKey>;
        VKey key;
        nri::Pipeline* pipeline;
        Layout* layout;
        std::uint32_t users = 0;
        std::uint64_t last_acquired = 0;
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
        std::size_t digest;
    };

    CompiledPrograms::CompiledPrograms(DeviceSession& session, std::uint32_t unused_limit) noexcept
        : session_(session), unused_limit_(unused_limit)
    {
    }

    CompiledPrograms::~CompiledPrograms() noexcept
    {
        // clear() transfers GPU objects to Session cleanup before its admission closes.
        if (!programs_.empty() || !layout_index_.empty()) std::terminate();
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
        auto value = std::make_unique<Layout>(Layout{std::move(key), *created, 1, max_colors, digest});
        auto item = layout_index_.emplace(digest, std::move(value));
        return item->second.get();
    }

    void CompiledPrograms::dropLayout(Layout& layout) noexcept
    {
        if (!layout.references) std::terminate();
        if (--layout.references != 0) return;
        const auto [first, last] = layout_index_.equal_range(layout.digest);
        for (auto item = first; item != last; ++item)
        {
            if (item->second.get() != &layout) continue;
            session_.nriCore().DestroyPipelineLayout(layout.native);
            layout_index_.erase(item);
            return;
        }
        std::terminate();
    }

    void CompiledPrograms::releaseLayout(Layout* layout) noexcept
    {
        dropLayout(*layout);
    }

    template <class Key>
    CompiledPrograms::Program* CompiledPrograms::find(const Key& key, const Lookup& lookup, std::uint64_t after) noexcept
    {
        const auto found = program_index_.find(lookup.digest_);
        if (found == program_index_.end()) return nullptr;
        for (auto* program : found->second)
        {
            if (program->publication <= after || program->layout != lookup.layout_) continue;
            const auto* stored = std::get_if<Key>(&program->key);
            if (!stored || !matches(*stored, key)) continue;
            ++program->users;
            program->last_acquired = ++acquisition_serial_;
            return program;
        }
        return nullptr;
    }

    template <class Key>
    CompiledPrograms::Lookup CompiledPrograms::lookup(const Key& key, Layout& layout) noexcept
    {
        Lookup result;
        result.digest_ = keyDigest(key);
        combineHash(result.digest_, &layout);
        result.layout_ = &layout;
        result.publication_ = publication_serial_;
        result.hit_ = find(key, result, 0);
        return result;
    }

    CompiledPrograms::Lookup CompiledPrograms::lookupRaster(const RasterProgramKey& key, Layout& layout) noexcept
    {
        return lookup(key, layout);
    }

    CompiledPrograms::Lookup
    CompiledPrograms::lookupCompute(const std::shared_ptr<const PreparedShader>& key, Layout& layout) noexcept
    {
        return lookup(key, layout);
    }

    template <class Key>
    CompiledPrograms::Program* CompiledPrograms::publish(Key key, const Lookup& lookup, nri::Pipeline* pipeline) noexcept
    {
        // Only entries published while native creation was in flight can be new matches.
        if (auto* existing = find(key, lookup, lookup.publication_))
        {
            session_.nriCore().DestroyPipeline(pipeline);
            return existing;
        }
        auto* layout = lookup.layout_;
        ++layout->references;
        auto value = std::make_unique<Program>(Program{
            std::move(key), pipeline, layout, 1, ++acquisition_serial_, lookup.digest_, ++publication_serial_,
            session_.makeFrontiers()
        });
        auto* program = value.get();
        program_index_[lookup.digest_].push_back(program);
        programs_.push_back(std::move(value));
        trim();
        return program;
    }

    CompiledPrograms::Program*
    CompiledPrograms::publishRaster(RasterProgramKey key, const Lookup& lookup, nri::Pipeline* pipeline) noexcept
    {
        return publish(std::move(key), lookup, pipeline);
    }

    CompiledPrograms::Program* CompiledPrograms::publishCompute(
        std::shared_ptr<const PreparedShader> key, const Lookup& lookup, nri::Pipeline* pipeline
    ) noexcept
    {
        return publish(std::move(key), lookup, pipeline);
    }

    nri::Pipeline* CompiledPrograms::native(const Program& program) noexcept
    {
        return program.pipeline;
    }

    void CompiledPrograms::release(std::span<Program* const> programs) noexcept
    {
        if (programs.empty()) return;
        auto accepted = session_.makeFrontiers();
        for (std::size_t q = 0; q < session_.queueCount(); ++q)
            accepted.accept(session_.submissionPoint(q, session_.physicalQueue(q).submitted()));
        for (auto* program : programs)
        {
            if (!program->users) std::terminate();
            program->last_uses.merge(accepted);
            --program->users;
        }
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
            auto position = program_index_.find(program->digest);
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
