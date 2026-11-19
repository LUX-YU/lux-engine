#include <lux/engine/render/extensions/upload/Preparation.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <algorithm>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <bit>
#include <cstring>
#include <functional>
#include <utility>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>
#include <lux/engine/render/features/mesh/RasterMeshDraws.hpp>
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>

namespace lux::render::detail
{
    namespace
    {
        struct PreparedProgram final
        {
            RasterProgramKey key;
            nri::Pipeline* unpublished = nullptr;
            nri::PipelineLayout* native_layout = nullptr;
            CompiledPrograms::Lookup lookup;
            CompiledPrograms::Program* program = nullptr;
        };

        struct ShaderPlacement final
        {
            std::shared_ptr<const PreparedShader> shader;
            PassBindings bindings;
            CompiledPrograms::Layout* layout = nullptr;
        };

        using MaterialPlacements = std::array<ShaderPlacement, kMeshWindingCount>;

        struct MeshProgramInput final
        {
            std::vector<std::pair<std::uint32_t, std::uint64_t>> groups;
            std::uint32_t material_index = 0, vertex_stride = 0;
            std::vector<MeshAttribute> attributes;
            std::array<PreparedProgram, 2> programs;
        };

        RenderResult<void> prepareMeshPrograms(
            std::span<MeshProgramInput> inputs,
            std::span<const std::shared_ptr<const MaterialProgramRecord>> materials,
            std::span<MaterialPlacements> shaders,
            const nri::DeviceDesc& limits,
            const RasterPassTemplate& stage,
            const MeshDrawSettings& settings,
            bool paged,
            std::span<const nri::Format> colors,
            nri::Format depth,
            std::uint32_t samples
        ) noexcept
        {
            for (std::size_t index = 0; index < materials.size(); ++index)
            {
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    auto shader = buildMeshShader(
                        materials[index]->data, settings.purpose, paged, winding != 0
                    );
                    if (!shader) return cxx::unexpected(shader.error());
                    auto placement = planRasterLayout(limits, shader->interface, stage);
                    if (!placement) return cxx::unexpected(placement.error());
                    auto relocated = relocateShader(*shader, placement->bindings);
                    if (!relocated) return cxx::unexpected(relocated.error());
                    shaders[index][winding] = {
                        std::make_shared<const PreparedShader>(std::move(*relocated)), std::move(*placement)
                    };
                }
            }
            for (auto& input : inputs)
            {
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    const auto& shader = shaders[input.material_index][winding].shader;
                    auto graphics = meshGraphicsState(
                        input.vertex_stride,
                        input.attributes,
                        materials[input.material_index]->data,
                        shader->artifact.interface,
                        settings.front_counter_clockwise,
                        winding != 0
                    );
                    if (!graphics) return cxx::unexpected(graphics.error());
                    if (settings.layered)
                    {
                        if (!limits.features.dynamicDepthBias)
                        {
                            return cxx::unexpected(RenderError{kDeviceUnsupported});
                        }
                        graphics->depth_bias_constant = 1;
                    }
                    auto& program = input.programs[winding];
                    program.key = {shader, std::move(*graphics), {colors.begin(), colors.end()}, depth, samples};
                }
            }
            return {};
        }
    } // namespace

    RasterMeshDraws::RasterMeshDraws(
        DeviceSession& device,
        ResourceRegistry& resources,
        RenderGraphInstance& instance,
        CompiledPrograms& programs,
        const MeshDrawGroupSource& source,
        MeshDrawSettings settings,
        const CompiledPass& pass
    ) noexcept
        : device_(device), resources_(resources), instance_(instance), programs_(programs), source_(source),
          settings_(settings), pass_(pass)
    {
    }

    RasterMeshDraws::~RasterMeshDraws() noexcept
    {
        std::vector<const PassBindings*> bindings;
        for (const auto& program : program_bindings_)
            if (program) bindings.push_back(&program->bindings);
        instance_.releaseRasterBindings(bindings);
        std::vector<CompiledPrograms::Program*> uses;
        for (const auto& program : program_bindings_) if (program) uses.push_back(program->program);
        programs_.release(uses);
    }

    bool RasterMeshDraws::requiresPrograms() const noexcept
    {
        return prepared_revision_ != source_.revision;
    }

    std::uint64_t RasterMeshDraws::programRevision() const noexcept
    {
        return source_.revision;
    }

    exec::task<RenderResult<bool>> RasterMeshDraws::preparePrograms(
        execution::CpuScheduler cpu, std::uint64_t revision, stdexec::inplace_stop_token stop
    ) noexcept
    {
        const auto is_current = [&]() noexcept { return !stop.stop_requested() && revision == source_.revision; };
        if (!is_current()) co_return false;
        const auto owner_scheduler = co_await stdexec::read_env(stdexec::get_scheduler);
        const auto& raster = *instance_.plan().rasterCommands(pass_);
        const auto& stage = std::get<RasterListCommands>(raster.draw).description;
        const auto& scope = *pass_.rasterScope();
        std::vector<nri::Format> colors;
        for (const auto& color : scope.colors)
        {
            const auto& image =
                std::get<GraphImageDesc>(instance_.plan().resources()[color.access.version.resource.index].description);
            colors.push_back(imageFormat(image.format).native);
        }
        const auto depth =
            scope.depth
                ? imageFormat(std::get<GraphImageDesc>(
                                  instance_.plan().resources()[scope.depth->access.version.resource.index].description
                              )
                                  .format)
                      .native
                : nri::Format::UNKNOWN;
        std::vector<MeshProgramInput> inputs;
        std::vector<std::shared_ptr<const MaterialProgramRecord>> materials;
        std::vector<CompiledPrograms::Program*> acquired;
        cxx::scope_exit release_acquired([&]() noexcept { programs_.release(acquired); });
        std::vector<MaterialPlacements> placements;
        cxx::scope_exit release_layouts([&]() noexcept
        {
            for (auto& material : placements)
                for (auto& placement : material)
                    if (placement.layout) programs_.releaseLayout(placement.layout);
        });
        std::vector<PreparedProgram*> missing;
        cxx::scope_exit release_candidates([&]() noexcept
        {
            for (auto* program : missing)
                if (program->unpublished) device_.nriCore().DestroyPipeline(program->unpublished);
        });
        // Resolve identities on the Server and retain immutable program records across scheduler hops.
        {
            std::map<MaterialProgramId, std::uint32_t> material_indices;
            using InputKey = std::tuple<std::uint32_t, std::uint32_t, std::span<const MeshAttribute>>;
            const auto less = [](const InputKey& a, const InputKey& b) noexcept
            {
                if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
                if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) < std::get<1>(b);
                return std::lexicographical_compare(
                    std::get<2>(a).begin(), std::get<2>(a).end(),
                    std::get<2>(b).begin(), std::get<2>(b).end(),
                    [](const MeshAttribute& left, const MeshAttribute& right) noexcept
                    {
                        return std::tie(left.location, left.offset, left.format) <
                               std::tie(right.location, right.offset, right.format);
                    }
                );
            };
            std::map<InputKey, std::uint32_t, decltype(less)> input_indices(less);
            for (std::uint32_t index = 0; index < source_.groups.size(); ++index)
            {
                const auto& group = source_.groups[index];
                if (group.record.candidate_count == 0 ||
                    (index < groups_.size() && groups_[index].generation == group.generation))
                {
                    continue;
                }
                auto meshes = resources_.borrow<IndexedTriangleMeshStorage>(
                    extension_detail::ResourceIdentity::parent(group.mesh)
                );
                const auto* geometry = meshes ? (*meshes)->find(group.mesh) : nullptr;
                if (!geometry)
                {
                    co_return cxx::unexpected(RenderError{kStorageIdentity});
                }
                const auto [material_entry, new_material] = material_indices.emplace(
                    group.program, static_cast<std::uint32_t>(materials.size())
                );
                if (new_material)
                {
                    const auto owner = extension_detail::ResourceIdentity::parent(group.program);
                    auto storage = resources_.borrow<RasterMaterialStorage>(owner);
                    auto material = storage ? (*storage)->borrowProgram(group.program) : nullptr;
                    if (!material) co_return cxx::unexpected(RenderError{kStorageIdentity});
                    materials.push_back(std::move(material));
                }
                InputKey key{material_entry->second, geometry->vertex_stride, geometry->attributes};
                const auto [entry, inserted] = input_indices.emplace(
                    std::move(key), static_cast<std::uint32_t>(inputs.size())
                );
                if (inserted)
                {
                    inputs.push_back({
                        {}, material_entry->second, geometry->vertex_stride, geometry->attributes, {}
                    });
                }
                inputs[entry->second].groups.emplace_back(index, group.generation);
            }
        }
        if (!inputs.empty())
        {
            placements.resize(materials.size());
            // Only immutable snapshots and device facts cross the CPU boundary. Registry,
            // active Effect cache and instance data are accessed only before/after the await.
            auto prepared = co_await (
                stdexec::schedule(cpu) |
                stdexec::then(
                    [&inputs,
                     &materials,
                     &placements,
                     limits = &device_.description(),
                     &stage,
                     settings = settings_,
                     paged = device_.usesDeviceAddresses(),
                     &colors,
                     depth,
                     samples = instance_.plan().outputDescription().samples]() noexcept
                    {
                        return prepareMeshPrograms(
                            inputs, materials, placements, *limits, stage, settings, paged, colors, depth, samples
                        );
                    }
                ) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCpuStopped}); }
                ) | stdexec::continues_on(owner_scheduler) | stdexec::upon_error(NativeDeliveryFailure<void>{})
            );
            if (!is_current()) co_return false;
            if (!prepared) co_return cxx::unexpected(prepared.error());
            for (auto& material : placements)
            {
                for (auto& placement : material)
                {
                    const auto layout = programs_.acquireLayout(
                        placement.bindings, static_cast<std::uint32_t>(colors.size())
                    );
                    if (!layout) co_return cxx::unexpected(layout.error());
                    placement.layout = *layout;
                }
            }
            for (auto& input : inputs)
            {
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    auto& candidate = input.programs[winding];
                    const auto& placement = placements[input.material_index][winding];
                    candidate.native_layout = placement.bindings.layout;
                    candidate.lookup = programs_.lookupRaster(candidate.key, *placement.layout);
                    candidate.program = candidate.lookup.hit();
                    if (candidate.program) acquired.push_back(candidate.program);
                    else missing.push_back(&candidate);
                }
            }
            if (!missing.empty())
            {
                // NRI CreateGraphicsPipeline is thread-safe. Queue work stays on NativeExecution,
                // and cache publication and destruction of unpublished handles stay on the Server.
                auto compiled_programs = co_await (stdexec::schedule(cpu) | stdexec::then(
                    [&missing, stop, device = &device_.device(), core = &device_.nriCore(),
                     limits = &device_.description(),
                     enabled = device_.enabledFeatures(), formats = device_.formatSupport(),
                     vertex_limits = std::array{device_.vertexStrideLimit(), device_.vertexOffsetLimit()}
                    ]() noexcept -> RenderResult<void>
                    {
                        for (auto* program : missing)
                        {
                            if (stop.stop_requested()) break;
                            auto created = createRasterProgram(
                                *device, *core, *limits, enabled, formats, vertex_limits,
                                *program->native_layout, program->key, program->unpublished
                            );
                            if (!created) return created;
                        }
                        return {};
                    }
                ) |
                stdexec::upon_stopped([]() noexcept -> RenderResult<void>
                { return cxx::unexpected(RenderError{kCpuStopped}); }) |
                stdexec::continues_on(owner_scheduler) | stdexec::upon_error(NativeDeliveryFailure<void>{}));
                if (!is_current()) co_return false;
                if (!compiled_programs) co_return cxx::unexpected(compiled_programs.error());
            }
        }
        if (!is_current()) co_return false;
        for (auto* program : missing)
        {
            program->program = programs_.publishRaster(
                std::move(program->key), program->lookup, std::exchange(program->unpublished, nullptr)
            );
            acquired.push_back(program->program);
        }
        missing.clear();
        groups_.resize(source_.groups.size());
        bool changed = !inputs.empty();
        for (std::size_t index = 0; index < groups_.size(); ++index)
        {
            if (source_.groups[index].record.candidate_count == 0 && groups_[index].generation != 0)
            {
                groups_[index] = {};
                changed = true;
            }
        }
        std::map<CompiledPrograms::Program*, std::uint32_t, std::less<>> program_indices;
        for (std::uint32_t i = 0; i < program_bindings_.size(); ++i)
            if (program_bindings_[i]) program_indices.emplace(program_bindings_[i]->program, i);
        for (auto& input : inputs)
        {
            std::array<std::uint32_t, 2> indices;
            for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
            {
                auto& program = input.programs[winding];
                auto existing = program_indices.find(program.program);
                if (existing == program_indices.end())
                {
                    const auto empty = std::ranges::find_if(
                        program_bindings_, [](const auto& value) noexcept { return !value; }
                    );
                    const auto index = static_cast<std::uint32_t>(empty - program_bindings_.begin());
                    if (empty == program_bindings_.end()) program_bindings_.push_back(nullptr);
                    program_bindings_[index] = std::make_unique<Program>(
                        Program{placements[input.material_index][winding].bindings, program.program}
                    );
                    existing = program_indices.emplace(program.program, index).first;
                    // This Program metadata now owns one use; duplicate acquired uses stay in the guard.
                    const auto use = std::ranges::find(acquired, program.program);
                    *use = acquired.back();
                    acquired.pop_back();
                }
                indices[winding] = existing->second;
            }
            for (const auto [index, generation] : input.groups)
            {
                groups_[index].generation = generation;
                groups_[index].programs = indices;
            }
        }
        std::vector<bool> used(program_bindings_.size());
        for (const auto& group : groups_)
            if (group.generation) for (auto index : group.programs) used[index] = true;
        std::vector<const PassBindings*> removed;
        for (std::size_t i = 0; i < program_bindings_.size(); ++i)
            if (program_bindings_[i] && !used[i]) removed.push_back(&program_bindings_[i]->bindings);
        instance_.releaseRasterBindings(removed);
        for (std::size_t i = 0; i < program_bindings_.size(); ++i)
        {
            if (program_bindings_[i] && !used[i])
            {
                programs_.release({&program_bindings_[i]->program, 1});
                program_bindings_[i].reset();
            }
        }
        if (changed) ++draw_layout_revision_;
        if (changed) rebuildDrawOrder();
        prepared_revision_ = revision;
        co_return true;
    }

    void RasterMeshDraws::rebuildDrawOrder() noexcept
    {
        draw_order_.clear();
        for (std::uint32_t group = 0; group < groups_.size(); ++group)
        {
            if (groups_[group].generation == 0)
            {
                continue;
            }
            for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
            {
                draw_order_.push_back(group * kMeshWindingCount + winding);
            }
        }
        // This producer is the built-in opaque/depth mesh path: depth writes, no blend or
        // shader storage writes. Never apply this ordering to UI or extension-owned lists.
        std::ranges::sort(draw_order_, [&](auto left, auto right) noexcept
        {
            const auto& a = *program_bindings_[groups_[left / kMeshWindingCount].programs[left % kMeshWindingCount]];
            const auto& b = *program_bindings_[groups_[right / kMeshWindingCount].programs[right % kMeshWindingCount]];
            if (a.bindings.layout != b.bindings.layout)
            {
                return std::less<nri::PipelineLayout*>{}(a.bindings.layout, b.bindings.layout);
            }
            if (a.program != b.program)
            {
                return std::less<CompiledPrograms::Program*>{}(a.program, b.program);
            }
            const auto& x = groups_[left / kMeshWindingCount];
            const auto& y = groups_[right / kMeshWindingCount];
            if (x.buffers.vertices.buffer != y.buffers.vertices.buffer)
                return std::less<nri::Buffer*>{}(x.buffers.vertices.buffer, y.buffers.vertices.buffer);
            if (x.buffers.indices.buffer != y.buffers.indices.buffer)
                return std::less<nri::Buffer*>{}(x.buffers.indices.buffer, y.buffers.indices.buffer);
            if (x.buffers.vertex_stride && y.buffers.vertex_stride)
            {
                const auto a = std::tuple{x.buffers.vertex_stride, x.buffers.vertices.offset % x.buffers.vertex_stride,
                    x.buffers.index_stride, x.buffers.indices.offset % x.buffers.index_stride};
                const auto b = std::tuple{y.buffers.vertex_stride, y.buffers.vertices.offset % y.buffers.vertex_stride,
                    y.buffers.index_stride, y.buffers.indices.offset % y.buffers.index_stride};
                if (a != b) return a < b;
            }
            return left < right;
        });
    }

    RenderResult<bool>
    RasterMeshDraws::prepare(const InstanceInputs& frame, ResourceUploader&, detail::Preparation&) noexcept
    {
        if (requiresPrograms())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const auto view_count = settings_.layered ? source_.view_count : 1u;
        auto& draws = instance_.rasterDraws(pass_, frame.slot, false);
        if (settings_.layered)
        {
            auto prepared = instance_.prepareImageLayers(GraphResourceId{settings_.depth_slot}, frame.slot, view_count);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            draws.sections.resize(view_count);
            for (std::uint32_t view = 0; view < view_count; ++view)
            {
                draws.sections[view].depth.descriptor = (*prepared)[view];
            }
        }
        else
        {
            // The single graph-owned attachment section is filled by prepareStreamedDraws.
            // Only this small section is regenerated; physical packets retain their storage.
            draws.sections.clear();
        }
        if (auto resolved = resolveFrameResources(frame, draws); !resolved)
        {
            return cxx::unexpected(resolved.error());
        }
        auto& slice = slices_[frame.slot];
        if (slice.view_count != view_count)
        {
            std::vector<const PassBindings*> bindings;
            for (const auto& program : program_bindings_)
                if (program) bindings.push_back(&program->bindings);
            instance_.releaseRasterBindings(bindings);
            for (auto& value : slices_)
            {
                value.view_count = view_count;
                value.draw_revision = 0;
            }
        }
        auto refreshed = refreshDrawPackets(frame, slice, draws);
        if (!refreshed)
        {
            return cxx::unexpected(refreshed.error());
        }
        if (*refreshed)
        {
            const auto& stage = std::get<RasterListCommands>(instance_.plan().rasterCommands(pass_)->draw).description;
            draws.parameters = stage.parameters.values;
        }
        updateViewParameters(frame, draws);
        if (*refreshed)
        {
            mergeDrawPackets(draws);
            instance_.invalidateRasterDraws(pass_, frame.slot);
        }
        slice.draw_revision = draw_layout_revision_;
        return true;
    }

    RenderResult<bool>
    RasterMeshDraws::refreshDrawPackets(const InstanceInputs& frame, Slice& slice, RasterDrawList& draws) noexcept
    {
        const auto view_count = slice.view_count;
        const auto packet_count = view_count * draw_order_.size();
        const bool shared_members = device_.enabledFeatures().indirect_first_instance;
        draws.descriptors.resize(shared_members ? program_bindings_.size() : packet_count);
        const auto& transforms = frame.resources[1];
        const auto& properties = frame.resources[2];
        const auto& alive = frame.resources[3];
        const auto& visible = frame.resources[4];
        const auto& commands = frame.resources[5];
        if (shared_members)
        {
            const std::array inputs{transforms, properties, alive, visible};
            for (std::uint32_t index = 0; index < program_bindings_.size(); ++index)
            {
                const auto& program = program_bindings_[index];
                if (!program) { draws.descriptors[index] = nullptr; continue; }
                auto set = instance_.prepareRasterBindings(frame.slot, program->bindings, 0, inputs);
                if (!set) return cxx::unexpected(set.error());
                draws.descriptors[index] = *set;
            }
        }
        bool rebuild = slice.draw_revision != draw_layout_revision_;
        if (!rebuild && !draws.packets.empty())
        {
            // The first merged packet still begins at the first ordered stream. Compare its actual
            // address instead of retaining another copy of the indirect Buffer/range in Slice.
            const auto order = draw_order_.front();
            const auto& group = source_.groups[order / kMeshWindingCount];
            const auto command = std::uint64_t(group.gpu_index) * source_.view_count * kMeshWindingCount +
                order % kMeshWindingCount;
            const auto offset = command * sizeof(nri::DrawIndexedDesc);
            if (commands.offset > UINT64_MAX - offset)
                return cxx::unexpected(RenderError{kFeatureContract});
            const auto& first = std::get<RasterDrawPacket::Indirect>(draws.packets.front().draw);
            rebuild = first.arguments != commands.buffer || first.offset != commands.offset + offset;
            if (!rebuild)
            {
                // A changed exposed range must still cover every saved command, including merged tails.
                for (const auto& packet : draws.packets)
                {
                    const auto& draw = std::get<RasterDrawPacket::Indirect>(packet.draw);
                    const bool invalid_begin = draw.offset < commands.offset ||
                        draw.offset - commands.offset > commands.size;
                    if (invalid_begin) return cxx::unexpected(RenderError{kFeatureContract});
                    const auto available = commands.size - (draw.offset - commands.offset);
                    const bool invalid_end = available < sizeof(nri::DrawIndexedDesc) ||
                        std::uint64_t(draw.count - 1) * draw.stride > available - sizeof(nri::DrawIndexedDesc);
                    if (invalid_end) return cxx::unexpected(RenderError{kFeatureContract});
                }
            }
        }
        if (!rebuild && shared_members) return false;
        if (rebuild)
        {
            // A failed rebuild must not leave partially overwritten packets eligible for reuse.
            // prepare() publishes this revision only after parameters and merging are complete.
            slice.draw_revision = 0;
            draws.packets.resize(packet_count);
            draws.vertices.resize(packet_count);
        }
        for (std::uint32_t view = 0; view < view_count; ++view)
        {
            for (std::size_t position = 0; position < draw_order_.size(); ++position)
            {
                const auto order = draw_order_[position];
                const auto index = order / kMeshWindingCount;
                const auto winding = order % kMeshWindingCount;
                const auto& group = source_.groups[index];
                const auto& buffers = groups_[index].buffers;
                const auto& vertices = buffers.vertices;
                const auto& indices = buffers.indices;
                const auto program_index = groups_[index].programs[winding];
                const auto& program = *program_bindings_[program_index];
                const auto stream = view * kMeshWindingCount + winding;
                const auto ordinal = static_cast<std::uint32_t>(view * draw_order_.size() + position);
                if (!shared_members)
                {
                    const ResourceInput members{
                        visible.buffer,
                        visible.offset + (group.record.visible_offset + stream * group.record.visible_stride) * 4,
                        std::uint64_t(group.record.visible_stride) * 4
                    };
                    const std::array inputs{transforms, properties, alive, members};
                    const auto key = std::uint64_t(index) * view_count * kMeshWindingCount + stream;
                    auto set = instance_.prepareRasterBindings(frame.slot, program.bindings, key, inputs);
                    if (!set) return cxx::unexpected(set.error());
                    draws.descriptors[ordinal] = *set;
                }
                if (!rebuild) continue;
                auto& packet = draws.packets[ordinal];
                packet.pipeline = CompiledPrograms::native(*program.program);
                packet.bindings = program.bindings.recordBindings();
                packet.first_descriptor = shared_members ? program_index : ordinal;
                packet.descriptor_count = 1;
                packet.first_vertex = ordinal;
                packet.vertex_count = 1;
                packet.parameter_offset = packet.parameter_size = packet.parameter_destination = 0;
                packet.scissor = {};
                const auto vertex_offset = vertices.offset % buffers.vertex_stride;
                const auto index_offset = indices.offset % buffers.index_stride;
                auto& vertex = draws.vertices[packet.first_vertex];
                const bool changed_vertex = vertex.buffer.buffer != vertices.buffer ||
                    vertex.buffer.offset != vertex_offset || vertex.buffer.stride != buffers.vertex_stride;
                if (changed_vertex)
                {
                    vertex = {0, {vertices.buffer, vertex_offset, buffers.vertex_stride}};
                }
                const auto index_type = buffers.index_stride == 2 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
                const bool changed_index = packet.index != indices.buffer || packet.index_offset != index_offset ||
                                           packet.index_type != index_type;
                if (changed_index)
                {
                    packet.index = indices.buffer;
                    packet.index_offset = index_offset;
                    packet.index_type = index_type;
                }
                const auto command = std::uint64_t(group.gpu_index) * source_.view_count * kMeshWindingCount + stream;
                const bool invalid_range = commands.size < sizeof(nri::DrawIndexedDesc) ||
                    command > (commands.size - sizeof(nri::DrawIndexedDesc)) / sizeof(nri::DrawIndexedDesc);
                if (invalid_range) return cxx::unexpected(RenderError{kFeatureContract});
                const auto relative_offset = command * sizeof(nri::DrawIndexedDesc);
                if (commands.offset > UINT64_MAX - relative_offset)
                    return cxx::unexpected(RenderError{kFeatureContract});
                packet.draw = RasterDrawPacket::Indirect{
                    commands.buffer, commands.offset + relative_offset, 1, sizeof(nri::DrawIndexedDesc), nullptr, 0
                };
            }
        }
        if (rebuild)
        {
            for (std::uint32_t view = 0; view < draws.sections.size(); ++view)
            {
                draws.sections[view].first_draw = static_cast<std::uint32_t>(view * draw_order_.size());
                draws.sections[view].draw_count = static_cast<std::uint32_t>(draw_order_.size());
            }
        }
        return rebuild;
    }

    void RasterMeshDraws::mergeDrawPackets(RasterDrawList& draws) noexcept
    {
        const auto limit = device_.description().other.drawIndirectMaxNum;
        if (!device_.enabledFeatures().multi_draw_indirect || limit <= 1) return;

        const auto same_state = [&](const RasterDrawPacket& left, const RasterDrawPacket& right) noexcept
        {
            const bool same_pipeline = left.pipeline == right.pipeline &&
                left.bindings.layout == right.bindings.layout;
            const bool same_index = left.index == right.index && left.index_offset == right.index_offset &&
                left.index_type == right.index_type;
            const bool same_scissor = left.scissor.x == right.scissor.x && left.scissor.y == right.scissor.y &&
                left.scissor.width == right.scissor.width && left.scissor.height == right.scissor.height;
            const bool same_counts = left.descriptor_count == right.descriptor_count &&
                left.vertex_count == right.vertex_count && left.parameter_size == right.parameter_size &&
                left.parameter_destination == right.parameter_destination;
            if (!same_pipeline || !same_index || !same_scissor || !same_counts) return false;
            const auto descriptors = std::span(draws.descriptors);
            if (!std::ranges::equal(
                    descriptors.subspan(left.first_descriptor, left.descriptor_count),
                    descriptors.subspan(right.first_descriptor, right.descriptor_count)
                )) return false;
            for (std::uint32_t i = 0; i < left.vertex_count; ++i)
            {
                const auto& a = draws.vertices[left.first_vertex + i];
                const auto& b = draws.vertices[right.first_vertex + i];
                const bool same_vertex = a.binding == b.binding && a.buffer.buffer == b.buffer.buffer &&
                    a.buffer.offset == b.buffer.offset && a.buffer.stride == b.buffer.stride;
                if (!same_vertex) return false;
            }
            return std::ranges::equal(
                std::span(draws.parameters).subspan(left.parameter_offset, left.parameter_size),
                std::span(draws.parameters).subspan(right.parameter_offset, right.parameter_size)
            );
        };
        std::uint32_t written = 0;
        const auto merge_range = [&](std::uint32_t first, std::uint32_t count) noexcept
        {
            const auto output_begin = written;
            for (std::uint32_t i = first; i < first + count; ++i)
            {
                auto& packet = draws.packets[i];
                const auto& command = std::get<RasterDrawPacket::Indirect>(packet.draw);
                if (written != output_begin)
                {
                    auto& previous = draws.packets[written - 1];
                    auto& batch = std::get<RasterDrawPacket::Indirect>(previous.draw);
                    const bool can_extend = batch.count < limit && batch.arguments == command.arguments &&
                        !batch.count_buffer && !command.count_buffer && command.offset > batch.offset;
                    if (can_extend && same_state(previous, packet))
                    {
                        const auto distance = command.offset - batch.offset;
                        const auto stride = batch.count == 1 ? distance : batch.stride;
                        const bool valid_stride = stride >= sizeof(nri::DrawIndexedDesc) && stride <= UINT32_MAX &&
                            stride % 4 == 0 && distance == std::uint64_t(batch.count) * stride;
                        // Every endpoint was range checked while producing the expanded packets.
                        // A positive arithmetic progression cannot address outside those endpoints.
                        if (valid_stride)
                        {
                            batch.stride = static_cast<std::uint32_t>(stride);
                            ++batch.count;
                            continue;
                        }
                    }
                }
                if (written != i) draws.packets[written] = packet;
                ++written;
            }
        };
        if (draws.sections.empty())
        {
            merge_range(0, static_cast<std::uint32_t>(draws.packets.size()));
        }
        else
        {
            for (auto& section : draws.sections)
            {
                const auto first = written;
                merge_range(section.first_draw, section.draw_count);
                section.first_draw = first;
                section.draw_count = written - first;
            }
        }
        draws.packets.resize(written);
    }

    RenderResult<void>
    RasterMeshDraws::resolveFrameResources(const InstanceInputs& frame, RasterDrawList& draws) noexcept
    {
        bool changed_geometry = false;
        cxx::scope_exit invalidate_geometry([&]() noexcept
        {
            // Also invalidate after partial resolution: an earlier Group may already borrow new buffers.
            if (changed_geometry)
            {
                ++draw_layout_revision_;
                rebuildDrawOrder();
            }
        });
        std::size_t read_count = 0;
        const auto read = [&](RasterDrawList::Read value) noexcept
        {
            if (read_count == draws.reads.size())
            {
                draws.reads.push_back(value);
            }
            else if (draws.reads[read_count] != value)
            {
                draws.reads[read_count] = value;
            }
            ++read_count;
        };
        ResourceInstanceId mesh_storage_id;
        IndexedTriangleMeshStorage* mesh_storage = nullptr;
        for (std::uint32_t index = 0; index < groups_.size(); ++index)
        {
            auto& group = groups_[index];
            if (group.generation == 0) continue;
            const auto mesh = source_.groups[index].mesh;
            const auto storage_id = extension_detail::ResourceIdentity::parent(mesh);
            if (!mesh_storage || mesh_storage_id != storage_id)
            {
                auto storage = resources_.borrow<IndexedTriangleMeshStorage>(storage_id);
                if (!storage) return cxx::unexpected(storage.error());
                mesh_storage = *storage;
                mesh_storage_id = storage_id;
            }
            auto buffers = mesh_storage->bufferViews(mesh);
            if (!buffers) return cxx::unexpected(buffers.error());
            changed_geometry |= group.buffers.vertices.buffer != buffers->vertices.buffer ||
                group.buffers.vertices.offset != buffers->vertices.offset ||
                group.buffers.indices.buffer != buffers->indices.buffer ||
                group.buffers.indices.offset != buffers->indices.offset ||
                group.buffers.vertex_stride != buffers->vertex_stride ||
                group.buffers.index_stride != buffers->index_stride;
            group.buffers = *buffers;
            const auto& vertices = buffers->vertices;
            const auto& indices = buffers->indices;
            read({vertices.buffer, GraphBufferRange{vertices.offset, vertices.size}, EGraphUse::VERTEX, 1});
            read({indices.buffer, GraphBufferRange{indices.offset, indices.size}, EGraphUse::INDEX});
        }
        // Shared external reads are independent of View count. Do not expand a range or
        // omit a changed texture merely because the material program stayed the same.
        read_buffers_.clear();
        read_images_.clear();
        for (const auto material : source_.materials)
        {
            auto storage = resources_.borrow<RasterMaterialStorage>(
                extension_detail::ResourceIdentity::parent(material)
            );
            if (!storage) return cxx::unexpected(storage.error());
            auto collected = (*storage)->collectReads(
                material, settings_.purpose, frame.slot, read_buffers_, read_images_
            );
            if (!collected) return collected;
        }
        std::ranges::sort(read_buffers_, [](const ResourceInput& a, const ResourceInput& b) noexcept
        {
            if (a.buffer != b.buffer) return std::less<>{}(a.buffer, b.buffer);
            return std::tie(a.offset, a.size) < std::tie(b.offset, b.size);
        });
        const auto duplicate_buffers = std::ranges::unique(read_buffers_, {}, [](const ResourceInput& input) noexcept
        {
            return std::tuple{input.buffer, input.offset, input.size};
        });
        read_buffers_.erase(duplicate_buffers.begin(), duplicate_buffers.end());
        std::ranges::sort(read_images_);
        const auto duplicate_images = std::ranges::unique(read_images_);
        read_images_.erase(duplicate_images.begin(), duplicate_images.end());
        for (const auto& buffer : read_buffers_)
            read({buffer.buffer, GraphBufferRange{buffer.offset, buffer.size}, EGraphUse::STORAGE_READ, 2});
        for (const auto* texture : read_images_)
        {
            const auto& image = device_.nriCore().GetTextureDesc(*texture);
            read({texture, GraphImageRange{1, 0, image.mipNum, 0, image.layerNum}, EGraphUse::SAMPLED, 2});
        }
        draws.reads.resize(read_count);
        return {};
    }

    void RasterMeshDraws::updateViewParameters(const InstanceInputs& frame, RasterDrawList& draws) noexcept
    {
        bool changed = false;
        const auto copy = [&](std::byte* destination, const void* source, std::size_t bytes) noexcept
        {
            if (std::memcmp(destination, source, bytes) != 0)
            {
                std::memcpy(destination, source, bytes);
                changed = true;
            }
        };
        if (!settings_.layered)
        {
            static_assert(offsetof(CameraParameters, view) == offsetof(MeshViewParameters, view));
            static_assert(offsetof(CameraParameters, projection) == offsetof(MeshViewParameters, projection));
            static_assert(offsetof(CameraParameters, inverse_view_projection) == sizeof(MeshViewParameters));
            copy(draws.parameters.data(), frame.parameters.data(), sizeof(MeshViewParameters));
            if (changed) instance_.invalidateRasterDraws(pass_, frame.slot);
            return;
        }
        const auto view_count = static_cast<std::uint32_t>(draws.sections.size());
        draws.parameters.resize(view_count * sizeof(MeshViewParameters));
        const auto views = frame.resources[kMeshDrawInputPorts.size()].parameters;
        const auto states = frame.resources[kMeshDrawInputPorts.size() + 1].parameters;
        const auto input = instance_.resourceInput(GraphResourceId{settings_.depth_slot}, frame.slot);
        const auto& image = device_.nriCore().GetTextureDesc(*input.image);
        for (std::uint32_t view = 0; view < view_count; ++view)
        {
            const auto& projection = reinterpret_cast<const MeshVisibilityView*>(views.data())[view];
            const auto& state = reinterpret_cast<const RasterState*>(states.data())[view];
            MeshViewParameters parameters;
            parameters.projection = projection.world_to_clip;
            copy(draws.parameters.data() + view * sizeof(parameters), &parameters, sizeof(parameters));
            auto& section = draws.sections[view];
            section.depth.loadOp = nri::LoadOp::CLEAR;
            section.depth.storeOp = nri::StoreOp::STORE;
            section.depth.clearValue.depthStencil.depth = 1.0f;
            const auto& area = state.viewport;
            const nri::Viewport viewport{
                area.x * image.width, area.y * image.height, area.width * image.width, area.height * image.height,
                area.min_depth, area.max_depth, true
            };
            changed |= section.viewport.x != viewport.x || section.viewport.y != viewport.y ||
                section.viewport.width != viewport.width || section.viewport.height != viewport.height ||
                section.viewport.depthMin != viewport.depthMin || section.viewport.depthMax != viewport.depthMax;
            changed |= !section.depth_bias || section.depth_bias->constant != state.depth_bias_constant ||
                section.depth_bias->slope != state.depth_bias_slope;
            section.viewport = viewport;
            section.depth_bias = nri::DepthBiasDesc{state.depth_bias_constant, 0, state.depth_bias_slope};
            section.parameter_offset = view * sizeof(MeshViewParameters);
            section.parameter_size = sizeof(MeshViewParameters);
            const nri::Rect scissor{
                static_cast<std::int16_t>(section.viewport.x), static_cast<std::int16_t>(section.viewport.y),
                static_cast<nri::Dim_t>(section.viewport.width), static_cast<nri::Dim_t>(section.viewport.height)
            };
            for (auto& packet : std::span(draws.packets).subspan(section.first_draw, section.draw_count))
            {
                const bool scissor_changed = packet.scissor.x != scissor.x || packet.scissor.y != scissor.y ||
                                     packet.scissor.width != scissor.width || packet.scissor.height != scissor.height;
                if (scissor_changed)
                {
                    packet.scissor = scissor;
                    changed = true;
                }
            }
        }
        if (changed) instance_.invalidateRasterDraws(pass_, frame.slot);
    }

    RenderResult<std::unique_ptr<InstanceData>> RasterMeshDraws::create(
        DeviceSession& device,
        ResourceRegistry& resources,
        InstanceAccess& access,
        std::span<const ResourceInstanceId>,
        const void* input
    ) noexcept
    {
        auto& instance = static_cast<RenderGraphInstance&>(access);
        const auto& configuration = *static_cast<const MeshDrawConfiguration*>(input);
        const auto source = instance.resourceInput(configuration.inputs[0], 0);
        const auto* pass = instance.compiledPass(configuration.pass);
        const auto* commands = pass ? instance.plan().rasterCommands(*pass) : nullptr;
        const bool is_raster_stream = commands && std::holds_alternative<RasterListCommands>(commands->draw);
        if (!is_raster_stream || source.parameters.size() != sizeof(MeshDrawGroupSource))
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        // The producer recipe runs first and owns this view for the lifetime of the Instance.
        const auto& groups = *reinterpret_cast<const MeshDrawGroupSource*>(source.parameters.data());
        const MeshDrawSettings settings{
            configuration.purpose, configuration.front_counter_clockwise,
            configuration.views.index != kGraphInvalidIndex, configuration.depth.index
        };
        auto value = std::unique_ptr<RasterMeshDraws>(
            new RasterMeshDraws(device, resources, instance, device.programs(), groups, settings, *pass)
        );
        return std::unique_ptr<InstanceData>(std::move(value));
    }
} // namespace lux::render::detail
