#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/resource/mesh/Resource.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <lux/engine/render/effect/capabilities/DeferredGeometry.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/SurfaceDraws.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/DrawBuckets.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/VisibilityInputs.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/IndexedDraws.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/PreparedMeshPipelines.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/material/MaterialStorage.hpp>
#include <NRI.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace lux::render::detail
{
    // The instance view supplies group membership. A material Resource supplies published programs.
    // Sibling Jobs share the derived slots and read list only while their installed contracts agree.
    class MeshSurfacePrograms final
    {
        friend class MeshSurfaceStream;

        struct ProgramUse final
        {
            MaterialProgramId program;
            std::uint32_t first;
        };

    public:
        MeshSurfacePrograms(
            const MaterialStorage& material,
            MeshProgramBinding binding,
            const CompiledRasterStream::RuntimeContract& raster
        ) noexcept : material_(material), binding_(binding), raster_(raster) {}

        [[nodiscard]] bool
        matches(const MeshProgramBinding& binding, const CompiledRasterStream::RuntimeContract& raster) const noexcept
        {
            return binding_ == binding && raster_ == raster;
        }

        [[nodiscard]] RenderResult<RasterProgramPublication> select(const MeshDrawTopology&) noexcept;

        void invalidate(MaterialProgramId program, rdesc::EMaterialPurpose purpose) noexcept
        {
            if (binding_.purpose != purpose) return;
            if (std::ranges::find(slots_, program, &ProgramUse::program) != slots_.end()) program_use_revision_ = 0;
        }

    private:
        const MaterialStorage& material_;
        const MeshProgramBinding binding_;
        const CompiledRasterStream::RuntimeContract raster_;
        std::uint64_t surface_revision_ = 0, program_use_revision_ = 0;
        std::uint64_t checked_release_revision_ = 0;
        std::uint64_t generation_ = 0;
        std::vector<PublishedRasterProgram> ready_;
        std::vector<ProgramUse> slots_;
        std::vector<std::pair<std::uint32_t, MaterialId>> materials_;
    };

    std::shared_ptr<MeshSurfacePrograms>
    MaterialStorage::shareSurfacePrograms(
        const MeshProgramBinding& binding,
        const CompiledRasterStream& stream
    ) const noexcept
    {
        for (const auto& use : surface_programs_)
        {
            auto active = use.lock();
            if (active && active->matches(binding, *stream.runtime)) return active;
        }
        std::erase_if(surface_programs_, [](const auto& use) noexcept { return use.expired(); });
        auto created = std::make_shared<MeshSurfacePrograms>(*this, binding, *stream.runtime);
        surface_programs_.emplace_back(created);
        return created;
    }

    void MaterialStorage::invalidateSurfacePrograms(MaterialProgramId program, rdesc::EMaterialPurpose purpose) noexcept
    {
        std::erase_if(surface_programs_, [&](const auto& use) noexcept
        {
            const auto active = use.lock();
            if (!active) return true;
            active->invalidate(program, purpose);
            return false;
        });
    }

    RenderResult<MeshSurfaceStream::Inputs>
    MeshSurfaceStream::resolve(const Configuration& config, const RasterOperands& operands) noexcept
    {
        const auto geometry = operands.resource("mesh.geometry", MeshResource::descriptor.type);
        if (!geometry) return cxx::unexpected(geometry.error());
        const auto records = operands.resource("mesh.materials", MaterialResource::descriptor.type);
        if (!records) return cxx::unexpected(records.error());
        std::optional<RasterResourceSlot> images;
        if (config.texture_descriptors)
        {
            const auto textures = operands.resource("mesh.textures", TextureResource::descriptor.type);
            if (!textures) return cxx::unexpected(textures.error());
            images = *textures;
        }
        Inputs resolved{
            operands.stream(), *geometry, *records, images, {}, {}, config.shared_members,
            config.front_counter_clockwise, config.purpose
        };
        for (std::size_t i = 0; i < config.buffers.size(); ++i)
        {
            const auto buffer = operands.buffer(config.buffers[i]);
            if (!buffer) return cxx::unexpected(buffer.error());
            resolved.buffers[i] = *buffer;
        }
        for (std::size_t i = 0; i < kMeshRasterInputNames.size(); ++i)
        {
            const auto argument = operands.argument(kMeshRasterInputNames[i]);
            if (!argument) return cxx::unexpected(argument.error());
            resolved.arguments[i] = *argument;
        }
        return resolved;
    }

    RenderResult<MeshSurfaceStream>
    MeshSurfaceStream::bind(const Inputs& inputs, const InstalledDrawSources& sources) noexcept
    {
        const auto meshes = sources.bind<IndexedTriangleMeshStorage>(inputs.geometry, EDrawResourceRead::INDEXED_GEOMETRY);
        if (!meshes) return cxx::unexpected(meshes.error());
        if (!meshes->owner) return cxx::unexpected(RenderError{kStorageIdentity});
        auto& output = meshes->output;
        const auto& stream = output.compiledStream();
        if (!stream.runtime) return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {stream.pass}});
        const DrawResourceReads reads = stream.runtime->target.shared_records ?
            DrawResourceReads{EDrawResourceRead::STORAGE_RECORDS} : DrawResourceReads{};
        const auto material = sources.bind<MaterialStorage>(inputs.materials, reads);
        if (!material) return cxx::unexpected(material.error());
        if (!material->owner) return cxx::unexpected(RenderError{kStorageIdentity});
        if (&material->output != &output) return cxx::unexpected(RenderError{kStorageMismatch});
        const SampledTexture2DStorage* textures = nullptr;
        if (inputs.textures)
        {
            const auto images = sources.bind<SampledTexture2DStorage>(*inputs.textures, EDrawResourceRead::SAMPLED_IMAGES);
            if (!images) return cxx::unexpected(images.error());
            if (&images->output != &output) return cxx::unexpected(RenderError{kStorageMismatch});
            textures = images->owner;
        }
        auto instances = output.viewInput(sources, 0, cxx::make_basic_type_info<MeshDrawTopology>());
        if (!instances) return cxx::unexpected(instances.error());
        const MeshProgramBinding binding{instances->owner, meshes->owner, inputs.purpose, inputs.front_counter_clockwise};
        auto programs = material->owner->shareSurfacePrograms(binding, stream);
        const auto& selected = *programs;
        auto read = [programs = std::move(programs)](const void* view) noexcept
        {
            return programs->select(*static_cast<const MeshDrawTopology*>(view));
        };
        if (auto bound = output.bindProgramInput(*instances, std::move(read)); !bound)
            return cxx::unexpected(bound.error());
        return MeshSurfaceStream{inputs, textures, selected, output};
    }

    RenderResult<RasterProgramPublication> MeshSurfacePrograms::select(const MeshDrawTopology& topology) noexcept
    {
        if (!program_use_revision_ || program_use_revision_ != topology.program_use_revision)
        {
            const auto& target = raster_.target;
            std::vector<PublishedRasterProgram> ready;
            std::map<const CompiledRasterProgram*, std::uint32_t> slots;
            std::vector<ProgramUse> next;
            next.reserve(topology.mesh_programs.size());
            for (const auto& use : topology.mesh_programs)
            {
                const auto prepared = material_.publishedMeshPipelines(use.program, binding_.purpose);
                if (!prepared) return cxx::unexpected(prepared.error());
                const auto* mesh = binding_.geometry->find(topology.meshes[use.mesh]);
                if (!mesh) return cxx::unexpected(RenderError{kStorageIdentity});
                const MeshVertexLayout layout{mesh->vertex_stride, mesh->attributes};
                const auto pair = *prepared ? (*prepared)->find(layout, binding_.front_counter_clockwise, target) :
                    std::span<const PublishedRasterProgram>{};
                if (pair.empty())
                {
                    return cxx::unexpected(RenderError{kMeshPipelineNotReady, {
                        extension_detail::ResourceIdentity::local(use.program).index,
                        static_cast<std::uint32_t>(binding_.purpose),
                        extension_detail::ResourceIdentity::local(topology.meshes[use.mesh]).index
                    }});
                }
                const auto first = static_cast<std::uint32_t>(ready.size());
                const auto [slot, inserted] = slots.try_emplace(pair.front().get(), first);
                if (inserted)
                {
                    if (pair.size() > kGraphInvalidIndex - ready.size())
                        return cxx::unexpected(RenderError{kDeviceCapacity});
                    ready.insert(ready.end(), pair.begin(), pair.end());
                }
                next.push_back({use.program, slot->second});
            }
            const bool slots_changed = !std::ranges::equal(slots_, next, {}, &ProgramUse::first, &ProgramUse::first);
            const bool publication_changed = !std::ranges::equal(ready_, ready, sameRasterProgram);
            if (!generation_ || slots_changed || publication_changed)
            {
                if (generation_ == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
                ready_ = std::move(ready);
                ++generation_;
                surface_revision_ = 0;
            }
            slots_ = std::move(next);
            program_use_revision_ = topology.program_use_revision;
        }
        else if (checked_release_revision_ != binding_.geometry->releaseRevision())
        {
            // An instance can still name a released MeshId. A live generation keeps the same arena slices.
            for (const auto id : topology.meshes)
                if (!binding_.geometry->find(id)) return cxx::unexpected(RenderError{kStorageIdentity});
        }
        checked_release_revision_ = binding_.geometry->releaseRevision();
        if (!surface_revision_ || surface_revision_ != topology.surface_revision)
        {
            materials_.clear();
            for (const auto& surface : topology.surfaces)
            {
                const auto first = slots_[surface.source].first;
                // Both windings link the same merged shader interface and material record.
                if (ready_[first]->record_stages) materials_.emplace_back(first, surface.material);
            }
            std::ranges::sort(materials_);
            const auto duplicates = std::ranges::unique(materials_);
            materials_.erase(duplicates.begin(), duplicates.end());
            surface_revision_ = topology.surface_revision;
        }
        return RasterProgramPublication{raster_, ready_, generation_};
    }

    RenderResult<void>
    MeshSurfaceStream::prepare(
        const MeshDrawTopology& topology,
        std::span<const MeshDrawGroupRecord> records,
        std::span<const RasterState> states
    ) noexcept
    {
        auto& stream = output_;
        auto bound = stream.programSlots();
        if (!bound) return cxx::unexpected(bound.error());
        const auto view_count = static_cast<std::uint32_t>(states.size());
        for (const auto& [index, material] : programs_.materials_)
        {
            const auto& program = (*bound)[index];
            const RasterRecordLayout layout{program.record_layout, stream.frame_slot, program.shared_texture_stages != 0};
            auto record = programs_.material_.selectRasterRecord(material, layout);
            if (!record) return cxx::unexpected(record.error());
            if (auto selected = record->record(stream, {index, kMeshWindingCount}, textures_); !selected)
                return cxx::unexpected(selected.error());
        }

        const LayoutVersion version{
            topology.raster_layout_revision, topology.program_use_revision, bound->revision, programs_.generation_, view_count
        };
        bool changed = !version.groups || !version.mesh_programs || layout_version_ != version;
        if (changed)
        {
            const bool geometry_changed = !layout_version_ || !version.mesh_programs ||
                layout_version_->mesh_programs != version.mesh_programs;
            layout_version_.reset();
            if (geometry_changed)
            {
                if (auto bound_geometry = bindGeometry(topology.meshes); !bound_geometry) return bound_geometry;
            }
            if (auto built = buildLayout(topology, records, version); !built) return built;
            layout_version_ = version;
        }
        const bool depth_bias = programs_.raster_.target.dynamic_depth_bias;
        for (auto& section : stream.sections)
        {
            const auto& state = states[section.layer];
            changed |= section.viewport != state.viewport;
            section.viewport = state.viewport;
            if (depth_bias)
            {
                changed |= !section.depth_bias || section.depth_bias->constant != state.depth_bias_constant ||
                    section.depth_bias->slope != state.depth_bias_slope;
                section.depth_bias = RasterDepthBias{state.depth_bias_constant, state.depth_bias_slope};
            }
        }
        if (changed) stream.markCommandsChanged();
        stream.written = true;
        return {};
    }

    RenderResult<void> MeshSurfaceStream::bindGeometry(std::span<const MeshId> ids) noexcept
    {
        auto& stream = output_;
        stream.beginGeometrySelection();
        stream.vertices.clear();
        const auto& meshes = *programs_.binding_.geometry;
        for (const auto id : ids)
        {
            auto slices = meshes.geometry(id);
            if (!slices) return cxx::unexpected(slices.error());
            auto geometry = stream.appendGeometry(meshes.id(), *slices);
            if (!geometry) return cxx::unexpected(geometry.error());
            stream.vertices.push_back({0, geometry->vertices});
        }
        return {};
    }

    RenderResult<void> MeshSurfaceStream::buildLayout(
        const MeshDrawTopology& instance, std::span<const MeshDrawGroupRecord> records, LayoutVersion version
    ) noexcept
    {
        auto& stream = output_;
        const auto view_count = version.views;
        stream.draws.clear();
        stream.bindings.clear();
        stream.sections.clear();
        if (!view_count)
        {
            stream.constants.clear();
            return {};
        }

        const auto per_view = std::uint64_t{instance.group_sources.size()} * kMeshWindingCount;
        const auto field_count = static_cast<std::uint32_t>(inputs_.arguments.size());
        const bool too_many_draws = per_view > kGraphInvalidIndex / view_count;
        if (too_many_draws || view_count > UINT32_MAX / sizeof(MeshViewLayer))
            return cxx::unexpected(RenderError{kDeviceCapacity});
        const auto draw_count = static_cast<std::uint32_t>(per_view * view_count);
        const auto binding_sets = inputs_.shared_members ? std::min(draw_count, 1u) : draw_count;
        if (binding_sets > kGraphInvalidIndex / field_count) return cxx::unexpected(RenderError{kDeviceCapacity});
        stream.draws.reserve(draw_count);
        stream.bindings.reserve(binding_sets * field_count);
        stream.constants.resize(view_count * sizeof(MeshViewLayer));

        const auto appendBindings = [&](RasterBufferSlice members) noexcept
        {
            const RasterStreamRange range{static_cast<std::uint32_t>(stream.bindings.size()), field_count};
            for (std::size_t arg = 0; arg < kMeshRasterMemberInput; ++arg)
            {
                const RasterBufferSlice whole{inputs_.buffers[arg], 0, RasterBufferSlice::whole};
                stream.bindings.push_back({inputs_.arguments[arg], whole});
            }
            stream.bindings.push_back({inputs_.arguments[kMeshRasterMemberInput], members});
            return range;
        };
        const auto members = inputs_.buffers[kMeshRasterMemberInput];
        RasterStreamRange shared{};
        if (inputs_.shared_members && draw_count) shared = appendBindings({members, 0, RasterBufferSlice::whole});
        constexpr auto indirect_bytes = sizeof(nri::DrawIndexedDesc);
        for (std::uint32_t i = 0; i < instance.group_sources.size(); ++i)
        {
            const auto source = instance.group_sources[i];
            const auto vertex = instance.mesh_programs[source].mesh;
            const auto geometry = stream.indexedGeometry(vertex);
            const auto& record = records[i];
            const auto stride = std::uint64_t{record.visible_stride} * sizeof(std::uint32_t);
            for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
            {
                const auto slot = programs_.slots_[source].first + winding;
                const RasterProgramSlot program{inputs_.stream.index, slot, version.programs};
                const auto offset = std::uint64_t{record.visible_offset} * sizeof(std::uint32_t) + stride * winding;
                const auto bindings = inputs_.shared_members ? shared : appendBindings({members, offset, stride});
                const auto command = std::uint64_t{i} * view_count * kMeshWindingCount + winding;
                stream.draws.push_back({
                    program, bindings, {vertex, 1}, {0, sizeof(MeshViewLayer)},
                    RasterStreamIndirect{
                        {inputs_.buffers[kMeshRasterIndirectInput], command * indirect_bytes, indirect_bytes}, {},
                        RasterStreamIndexBinding{geometry.indices, geometry.index_type, ERasterIndexOrigin::BUFFER_BASE}
                    }
                });
            }
        }
        std::ranges::sort(stream.draws, {}, [](const auto& draw) noexcept
        {
            return std::pair{draw.program.index, std::get<RasterStreamIndirect>(draw.draw).arguments.offset};
        });
        // Every layer uses the same program and geometry order. The reserved first section is its template.
        const auto first_layer = std::span<const RasterStreamDraw>{stream.draws};
        for (std::uint32_t view = 0; view < view_count; ++view)
        {
            const MeshViewLayer layer{view};
            const auto constant_offset = view * static_cast<std::uint32_t>(sizeof(layer));
            std::memcpy(stream.constants.data() + constant_offset, &layer, sizeof(layer));
            if (view)
            {
                const auto layer_stream = std::uint64_t{view} * kMeshWindingCount;
                for (auto draw : first_layer)
                {
                    draw.constants.first = constant_offset;
                    std::get<RasterStreamIndirect>(draw.draw).arguments.offset += layer_stream * indirect_bytes;
                    if (!inputs_.shared_members)
                    {
                        const auto member_binding = draw.bindings.first + kMeshRasterMemberInput;
                        auto slice = std::get<RasterBufferSlice>(stream.bindings[member_binding].source);
                        slice.offset += layer_stream * slice.size;
                        draw.bindings = appendBindings(slice);
                    }
                    stream.draws.push_back(draw);
                }
            }
            const auto count = static_cast<std::uint32_t>(per_view);
            stream.sections.push_back({{view * count, count}, view});
        }
        return {};
    }
}
