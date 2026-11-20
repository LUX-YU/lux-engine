#include <lux/engine/render/effect/MeshSurfaceStream.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/features/mesh/MeshGrouping.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>
#include <NRI.h>
#include <algorithm>
#include <tuple>

namespace lux::render::detail
{
    RenderResult<MeshSurfaceStream::Inputs>
    MeshSurfaceStream::resolve(const Configuration& config, InstanceLinkContext& context) noexcept
    {
        const auto stream = context.resolveDrawStream(config.pass);
        if (!stream) return cxx::unexpected(stream.error());
        Inputs resolved{*stream, {}, {}, config.paged, config.shared_members, config.front_counter_clockwise,
            config.purpose};
        for (std::size_t i = 0; i < config.buffers.size(); ++i)
        {
            const auto buffer = context.resolveRasterBuffer(config.pass, config.buffers[i]);
            if (!buffer) return cxx::unexpected(buffer.error());
            resolved.buffers[i] = *buffer;
        }
        for (std::size_t i = 0; i < kMeshRasterInputNames.size(); ++i)
        {
            const auto argument = context.resolveRasterArgument(config.pass, kMeshRasterInputNames[i]);
            if (!argument) return cxx::unexpected(argument.error());
            resolved.arguments[i] = *argument;
        }
        return resolved;
    }

    RenderResult<void>
    MeshSurfaceStream::selectPrograms(RasterProgramSelection& selection, const MeshInstanceSnapshot& instances) noexcept
    {
        ProgramSelectionState next;
        const auto stage = [&](RenderResult<void> declared) noexcept
        {
            if (!declared) return declared;
            std::ranges::sort(next.materials);
            const auto duplicates = std::ranges::unique(next.materials);
            next.materials.erase(duplicates.begin(), duplicates.end());
            pending_programs_.emplace(std::move(next));
            return declared;
        };
        if (selected_programs_)
        {
            const auto& previous = *selected_programs_;
            std::vector<std::uint32_t> used;
            bool has_new_group = false;
            for (const auto& instance : instances.instances)
            {
                if (!instance.render_enabled) continue;
                const GroupKey key{instance.mesh, instance.program};
                const auto group = previous.groups.find(key);
                if (group == previous.groups.end())
                {
                    has_new_group = true;
                    break;
                }
                if (next.groups.emplace(key, group->second).second) used.push_back(group->second.first);
                if (group->second.reads_material) next.materials.emplace_back(group->second.first, instance.material);
            }
            if (!has_new_group)
            {
                std::ranges::sort(used);
                const auto slots = std::ranges::unique(used).begin() - used.begin();
                next.program_count = static_cast<std::size_t>(slots) * kMeshWindingCount;
                if (next.program_count == previous.program_count)
                    return stage(selection.reusePrograms(inputs_.stream));
            }
            next = {};
        }

        struct MaterialShaders final
        {
            const rdesc::MaterialProgramSet* program = nullptr;
            std::array<ShaderCandidateSlot, kMeshWindingCount> shaders;
            bool reads_material = false;
        };
        using LayoutKey = std::tuple<MaterialProgramId, std::uint32_t, std::span<const MeshAttribute>>;
        const auto compare_layout = [](const LayoutKey& left, const LayoutKey& right) noexcept
        {
            const auto& [left_program, left_stride, left_attributes] = left;
            const auto& [right_program, right_stride, right_attributes] = right;
            const auto a = std::tie(left_program, left_stride);
            const auto b = std::tie(right_program, right_stride);
            if (a != b) return a < b;
            return std::lexicographical_compare(
                left_attributes.begin(), left_attributes.end(), right_attributes.begin(), right_attributes.end(),
                [](const MeshAttribute& first, const MeshAttribute& second) noexcept
                {
                    return std::tie(first.location, first.offset, first.format) <
                        std::tie(second.location, second.offset, second.format);
                }
            );
        };
        std::map<MaterialProgramId, MaterialShaders> materials;
        std::map<MeshId, MeshDescription> geometry;
        std::map<LayoutKey, std::uint32_t, decltype(compare_layout)> layouts(compare_layout);
        std::vector<RuntimeRasterProgram> candidates;
        std::vector<rdesc::ShaderProgramArtifact> artifacts;
        for (const auto& instance : instances.instances)
        {
            if (!instance.render_enabled) continue;
            const auto [group, new_group] = next.groups.try_emplace({instance.mesh, instance.program});
            if (new_group)
            {
                const auto [material, new_material] = materials.try_emplace(instance.program);
                auto& shaders = material->second;
                if (new_material)
                {
                    auto program = selection.materialProgram(inputs_.stream, instance.program);
                    if (!program) return cxx::unexpected(program.error());
                    shaders.program = *program;
                    if (artifacts.size() > kGraphInvalidIndex - kMeshWindingCount)
                        return cxx::unexpected(RenderError{kDeviceCapacity});
                    for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                    {
                        auto shader = buildMeshShader(
                            *shaders.program, inputs_.purpose, inputs_.paged, winding != 0
                        );
                        if (!shader) return cxx::unexpected(shader.error());
                        for (const auto& set : shader->interface.sets)
                        {
                            shaders.reads_material |= std::ranges::any_of(set.bindings, [](const auto& field) noexcept
                            {
                                return field.count == 0 && field.name == rdesc::kMaterialTableName;
                            });
                        }
                        shaders.shaders[winding] = {static_cast<std::uint32_t>(artifacts.size())};
                        artifacts.push_back(std::move(*shader));
                    }
                }
                const auto [mesh, new_mesh] = geometry.try_emplace(instance.mesh);
                if (new_mesh)
                {
                    auto selected = selection.meshLayout(inputs_.stream, instance.mesh);
                    if (!selected) return cxx::unexpected(selected.error());
                    mesh->second = std::move(*selected);
                }
                if (candidates.size() > kGraphInvalidIndex - kMeshWindingCount)
                    return cxx::unexpected(RenderError{kDeviceCapacity});
                const auto& layout = mesh->second;
                const auto [use, new_layout] = layouts.try_emplace(
                    LayoutKey{instance.program, layout.vertex_stride, layout.attributes},
                    static_cast<std::uint32_t>(candidates.size())
                );
                if (new_layout)
                {
                    for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                    {
                        const auto shader = shaders.shaders[winding];
                        auto state = meshGraphicsState(
                            layout.vertex_stride, layout.attributes, *shaders.program,
                            artifacts[shader.index].interface, inputs_.front_counter_clockwise, winding != 0
                        );
                        if (!state) return cxx::unexpected(state.error());
                        auto& program = candidates.emplace_back(shader, std::move(*state), ParameterBinding{});
                        program.parameters.setInlineConstant(VisibilityViewConstants{});
                    }
                }
                group->second = {use->second, shaders.reads_material};
            }
            if (group->second.reads_material) next.materials.emplace_back(group->second.first, instance.material);
        }
        next.program_count = candidates.size();
        return stage(selection.setPrograms(inputs_.stream, std::move(artifacts), candidates));
    }

    void MeshSurfaceStream::settlePrograms(bool adopted) noexcept
    {
        if (!pending_programs_) return;
        if (adopted) selected_programs_ = std::move(pending_programs_);
        pending_programs_.reset();
    }

    RenderResult<void> MeshSurfaceStream::prepare(
        FramePreparation& frame, const MeshDrawGroupSource& source, const ViewGpuParameters& view
    ) noexcept
    {
        const VisibilityViewConstants constants{view.view, view.projection};
        const RasterState state{{view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3]}};
        return prepareSections(frame, source, std::span{&constants, 1}, std::span{&state, 1}, false);
    }

    RenderResult<void> MeshSurfaceStream::prepareShadow(
        FramePreparation& frame, const MeshDrawGroupSource& source, std::span<const MeshVisibilityView> views,
        std::span<const RasterState> states
    ) noexcept
    {
        shadow_constants_.clear();
        shadow_constants_.reserve(views.size());
        for (const auto& view : views) shadow_constants_.push_back({kIdentityTransform, view.world_to_clip});
        return prepareSections(frame, source, shadow_constants_, states, true);
    }

    RenderResult<void> MeshSurfaceStream::prepareSections(
        FramePreparation& frame, const MeshDrawGroupSource& source, std::span<const VisibilityViewConstants> constants,
        std::span<const RasterState> states, bool depth_bias
    ) noexcept
    {
        draws_.clear();
        bindings_.clear();
        vertices_.clear();
        groups_.clear();
        sections_.clear();
        const bool invalid_views = constants.size() != source.view_count || states.size() != source.view_count;
        if (invalid_views) return cxx::unexpected(RenderError{kFrameContract});
        if (!selected_programs_) return cxx::unexpected(RenderError{kFrameContract});
        for (const auto& [index, material] : selected_programs_->materials)
        {
            for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
            {
                auto program = frame.rasterProgram(inputs_.stream, index + winding);
                if (!program) return cxx::unexpected(program.error());
                auto selected = frame.selectMaterial(inputs_.stream, *program, material);
                if (!selected) return cxx::unexpected(selected.error());
            }
        }
        for (const auto& group : source.groups)
        {
            if (group.gpu_index == kGraphInvalidIndex) continue;
            const auto selected = selected_programs_->groups.find({group.mesh, group.program});
            if (selected == selected_programs_->groups.end()) return cxx::unexpected(RenderError{kFrameContract});
            auto mesh = frame.selectMesh(inputs_.stream, group.mesh);
            if (!mesh) return cxx::unexpected(mesh.error());
            vertices_.push_back({0, mesh->vertices});
            groups_.push_back({
                *mesh, selected->second.first, group.gpu_index, group.record.visible_offset, group.record.visible_stride
            });
        }
        for (std::uint32_t view = 0; view < source.view_count; ++view)
        {
            const auto first = static_cast<std::uint32_t>(draws_.size());
            const auto constant_offset = view * static_cast<std::uint32_t>(sizeof(VisibilityViewConstants));
            for (std::uint32_t vertex = 0; vertex < groups_.size(); ++vertex)
            {
                if (bindings_.size() > kGraphInvalidIndex - kMeshWindingCount * inputs_.arguments.size())
                    return cxx::unexpected(RenderError{kDeviceCapacity});
                const auto& group = groups_[vertex];
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    auto program = frame.rasterProgram(inputs_.stream, group.first_program + winding);
                    if (!program) return cxx::unexpected(program.error());
                    const auto binding = static_cast<std::uint32_t>(bindings_.size());
                    for (std::size_t i = 0; i < kMeshRasterMemberInput; ++i)
                    {
                        bindings_.push_back({
                            inputs_.arguments[i], RasterBufferSlice{inputs_.buffers[i], 0, RasterBufferSlice::whole}
                        });
                    }
                    const auto stream = view * kMeshWindingCount + winding;
                    const auto stride = std::uint64_t(group.visible_stride) * sizeof(std::uint32_t);
                    const auto member_offset = std::uint64_t(group.visible_offset) * sizeof(std::uint32_t) + stride * stream;
                    bindings_.push_back({inputs_.arguments[kMeshRasterMemberInput], RasterBufferSlice{
                        inputs_.buffers[kMeshRasterMemberInput], inputs_.shared_members ? 0 : member_offset,
                        inputs_.shared_members ? RasterBufferSlice::whole : stride
                    }});
                    const auto command = std::uint64_t(group.gpu_index) * source.view_count * kMeshWindingCount + stream;
                    constexpr auto indirect_bytes = sizeof(nri::DrawIndexedDesc);
                    draws_.push_back({
                        *program, {binding, 4}, {vertex, 1}, {constant_offset, sizeof(VisibilityViewConstants)},
                        RasterStreamIndirect{
                            {inputs_.buffers[kMeshRasterIndirectInput], command * indirect_bytes, indirect_bytes}, {},
                            RasterStreamIndexBinding{
                                group.geometry.indices, group.geometry.index_type, ERasterIndexOrigin::MESH_ARENA
                            }
                        }
                    });
                }
            }
            auto draws = std::span{draws_}.subspan(first);
            std::ranges::sort(draws, {}, [](const auto& draw) noexcept
            {
                return std::pair{draw.program.index, std::get<RasterStreamIndirect>(draw.draw).arguments.offset};
            });
            const auto& state = states[view];
            auto& section = sections_.emplace_back(
                RasterStreamRange{first, static_cast<std::uint32_t>(draws.size())}, view, state.viewport
            );
            if (depth_bias) section.depth_bias = RasterDepthBias{state.depth_bias_constant, state.depth_bias_slope};
        }
        return frame.setRasterStream(inputs_.stream, {draws_, bindings_, vertices_, std::as_bytes(constants), sections_});
    }
}
