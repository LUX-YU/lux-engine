#include <algorithm>
#include <lux/engine/render/graph/CompiledPrograms.hpp>
#include <bit>
#include <cstring>
#include <functional>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/RasterDraws.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/features/RasterMeshDraws.hpp>
#include <lux/engine/render/features/ShadowInputs.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/storage/RasterMaterialStorage.hpp>

namespace lux::render::detail
{
    namespace
    {
        struct PreparedProgram final
        {
            RasterProgramKey key;
            PassBindings placement;
            nri::Pipeline* pipeline = nullptr;
        };

        struct MeshProgramInput final
        {
            std::vector<std::pair<std::uint32_t, std::uint64_t>> groups;
            std::uint32_t material_index = 0, vertex_stride = 0;
            std::vector<MeshAttribute> attributes;
            std::array<PreparedProgram, 2> programs;
        };

        RenderResult<void> prepareMeshPrograms(
            std::span<MeshProgramInput> inputs,
            std::span<const rdesc::MaterialProgramSet> materials,
            const nri::DeviceDesc& limits,
            const RasterPassTemplate& stage,
            const MeshDrawConfiguration& configuration,
            std::span<const nri::Format> colors,
            nri::Format depth,
            std::uint32_t samples
        ) noexcept
        {
            using ShaderPlacement = std::pair<rdesc::SpirvArtifact, PassBindings>;
            std::vector<std::array<ShaderPlacement, kMeshWindingCount>> shaders(materials.size());
            for (std::size_t index = 0; index < materials.size(); ++index)
            {
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    auto shader = buildMeshShader(
                        materials[index], configuration.purpose, configuration.paged, winding != 0
                    );
                    if (!shader) return cxx::unexpected(shader.error());
                    auto placement = planRasterLayout(limits, shader->interface, stage);
                    if (!placement) return cxx::unexpected(placement.error());
                    auto relocated = relocateShader(*shader, placement->bindings);
                    if (!relocated) return cxx::unexpected(relocated.error());
                    shaders[index][winding] = {std::move(*relocated), std::move(*placement)};
                }
            }
            for (auto& input : inputs)
            {
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    const auto& [shader, placement] = shaders[input.material_index][winding];
                    auto graphics = meshGraphicsState(
                        input.vertex_stride,
                        input.attributes,
                        materials[input.material_index],
                        shader.interface,
                        configuration.front_counter_clockwise,
                        winding != 0
                    );
                    if (!graphics) return cxx::unexpected(graphics.error());
                    if (configuration.views.index != kGraphInvalidIndex)
                    {
                        if (!limits.features.dynamicDepthBias)
                        {
                            return cxx::unexpected(RenderError{kDeviceUnsupported});
                        }
                        graphics->depth_bias_constant = 1;
                    }
                    auto& program = input.programs[winding];
                    program.key = {shader, std::move(*graphics), {colors.begin(), colors.end()}, depth, samples};
                    program.placement = placement;
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
        MeshDrawConfiguration configuration
    ) noexcept
        : device_(device), resources_(resources), instance_(instance), programs_(programs),
          configuration_(configuration)
    {
    }

    RasterMeshDraws::~RasterMeshDraws() noexcept
    {
        std::vector<const PassBindings*> bindings;
        for (const auto& program : program_bindings_)
            if (program) bindings.push_back(&program->bindings);
        instance_.releaseRasterBindings(bindings);
        std::vector<nri::Pipeline*> uses;
        for (const auto& program : program_bindings_) if (program) uses.push_back(program->pipeline);
        programs_.release(uses);
    }

    const MeshDrawGroupSource& RasterMeshDraws::source() const noexcept
    {
        const auto input = instance_.resourceInput(configuration_.inputs[0], 0);
        return *reinterpret_cast<const MeshDrawGroupSource*>(input.parameters.data());
    }

    bool RasterMeshDraws::requiresPrograms() const noexcept
    {
        return prepared_revision_ != source().revision;
    }

    exec::task<RenderResult<void>> RasterMeshDraws::preparePrograms(execution::CpuScheduler cpu) noexcept
    {
        const auto& compiled = *instance_.compiledPass(configuration_.pass);
        const auto& raster = std::get<RasterCommands>(compiled.work);
        const auto& stage = std::get<RasterListCommands>(raster.draw).description;
        std::vector<nri::Format> colors;
        for (const auto& color : raster.colors)
        {
            const auto& image =
                std::get<GraphImageDesc>(instance_.plan().resources()[color.access.version.resource.index].description);
            colors.push_back(imageFormat(image.format).native);
        }
        const auto depth =
            raster.depth
                ? imageFormat(std::get<GraphImageDesc>(
                                  instance_.plan().resources()[raster.depth->access.version.resource.index].description
                              )
                                  .format)
                      .native
                : nri::Format::UNKNOWN;
        for (;;)
        {
            const auto revision = source().revision;
            std::vector<MeshProgramInput> inputs;
            std::vector<rdesc::MaterialProgramSet> materials;
            std::vector<nri::Pipeline*> acquired;
            cxx::scope_exit release_acquired([&]() noexcept { programs_.release(acquired); });
            std::vector<PreparedProgram*> missing;
            cxx::scope_exit release_candidates([&]() noexcept
            {
                for (auto* program : missing)
                {
                    device_.nriCore().DestroyPipeline(program->pipeline);
                    programs_.releaseLayout(program->placement.layout);
                }
            });
            // Lookup keys borrow only during this unsuspended freezing phase.
            {
                std::map<const rdesc::MaterialProgramSet*, std::uint32_t, std::less<>> material_indices;
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
                for (std::uint32_t index = 0; index < source().groups.size(); ++index)
                {
                    const auto& group = source().groups[index];
                    if (group.record.candidate_count == 0 ||
                        (index < groups_.size() && groups_[index].generation == group.generation))
                    {
                        continue;
                    }
                    auto meshes = resources_.borrow<IndexedTriangleMeshStorage>(
                        extension_detail::ResourceIdentity::storage(group.selection.mesh)
                    );
                    auto material_storage = resources_.borrow<RasterMaterialStorage>(
                        extension_detail::ResourceIdentity::storage(group.selection.material_source)
                    );
                    const auto* geometry = meshes ? (*meshes)->find(group.selection.mesh) : nullptr;
                    const auto* material = material_storage
                        ? (*material_storage)->program(group.selection.material_source) : nullptr;
                    if (!geometry || !material)
                    {
                        co_return cxx::unexpected(RenderError{kStorageIdentity});
                    }
                    const auto [material_entry, new_material] = material_indices.emplace(
                        material, static_cast<std::uint32_t>(materials.size())
                    );
                    if (new_material) materials.push_back(*material);
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
                // Only immutable snapshots and device facts cross the CPU boundary. Registry,
                // active Effect cache and instance data are accessed only before/after the await.
                auto prepared = co_await (
                    stdexec::schedule(cpu) |
                    stdexec::then(
                        [&inputs,
                         &materials,
                         limits = &device_.description(),
                         &stage,
                         configuration = configuration_,
                         &colors,
                         depth,
                         samples = instance_.plan().outputDescription().samples]() noexcept
                        {
                            return prepareMeshPrograms(
                                inputs, materials, *limits, stage, configuration, colors, depth, samples
                            );
                        }
                    ) |
                    stdexec::upon_stopped(
                        []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCpuStopped}); }
                    )
                );
                if (!prepared)
                {
                    co_return prepared;
                }
                for (auto& input : inputs)
                {
                    for (auto& program : input.programs)
                    {
                        program.pipeline = programs_.acquireRaster(program.key, program.placement);
                        if (program.pipeline) acquired.push_back(program.pipeline);
                        else
                        {
                            auto layout = programs_.acquireLayout(
                                program.placement, static_cast<std::uint32_t>(program.key.colors.size())
                            );
                            if (!layout) co_return cxx::unexpected(layout.error());
                            missing.push_back(&program);
                        }
                    }
                }
                if (!missing.empty())
                {
                    auto compiled_programs = co_await (stdexec::schedule(cpu) | stdexec::then(
                        [&missing, device = &device_.device(), core = &device_.nriCore(),
                         enabled = device_.enabledFeatures(), formats = device_.formatSupport(),
                         vertex_limits = std::array{device_.vertexStrideLimit(), device_.vertexOffsetLimit()}
                        ]() noexcept -> RenderResult<void>
                        {
                            for (auto* program : missing)
                            {
                                auto pipeline = compileRasterProgram(*device, *core, enabled, formats, vertex_limits,
                                    *program->placement.layout, program->key);
                                if (!pipeline) return cxx::unexpected(pipeline.error());
                                program->pipeline = *pipeline;
                            }
                            return {};
                        }
                    ) | stdexec::upon_stopped([]() noexcept -> RenderResult<void>
                    { return cxx::unexpected(RenderError{kCpuStopped}); }));
                    if (!compiled_programs) co_return compiled_programs;
                }
            }
            if (revision != source().revision)
            {
                continue; // Local guards return compilation borrows and discard unpublished candidates.
            }
            for (auto* program : missing)
            {
                program->pipeline = programs_.publishRaster(
                    std::move(program->key), program->placement.layout, program->pipeline
                );
                acquired.push_back(program->pipeline);
            }
            missing.clear();
            groups_.resize(source().groups.size());
            bool changed = !inputs.empty();
            for (std::size_t index = 0; index < groups_.size(); ++index)
            {
                if (source().groups[index].record.candidate_count == 0 && groups_[index].generation != 0)
                {
                    groups_[index] = {};
                    changed = true;
                }
            }
            std::map<nri::Pipeline*, std::uint32_t, std::less<>> program_indices;
            for (std::uint32_t i = 0; i < program_bindings_.size(); ++i)
                if (program_bindings_[i]) program_indices.emplace(program_bindings_[i]->pipeline, i);
            for (auto& input : inputs)
            {
                std::array<std::uint32_t, 2> indices;
                for (std::uint32_t winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    auto& program = input.programs[winding];
                    auto existing = program_indices.find(program.pipeline);
                    if (existing == program_indices.end())
                    {
                        const auto empty = std::ranges::find_if(
                            program_bindings_, [](const auto& value) noexcept { return !value; }
                        );
                        const auto index = static_cast<std::uint32_t>(empty - program_bindings_.begin());
                        if (empty == program_bindings_.end()) program_bindings_.push_back(nullptr);
                        program_bindings_[index] = std::make_unique<Program>(
                            Program{std::move(program.placement), program.pipeline}
                        );
                        existing = program_indices.emplace(program.pipeline, index).first;
                        // This Program metadata now owns one use; duplicate acquired uses stay in the guard.
                        const auto use = std::ranges::find(acquired, program.pipeline);
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
                    programs_.release({&program_bindings_[i]->pipeline, 1});
                    program_bindings_[i].reset();
                }
            }
            if (changed) ++draw_layout_revision_;
            material_sources_.clear();
            std::map<const rdesc::MaterialProgramSet*, MaterialId, std::less<>> definitions;
            for (const auto& group : source().groups)
            {
                if (group.record.candidate_count == 0) continue;
                const auto material = group.selection.material_source;
                auto storage = resources_.borrow<RasterMaterialStorage>(
                    extension_detail::ResourceIdentity::storage(material)
                );
                if (!storage) co_return cxx::unexpected(storage.error());
                if (definitions.emplace((*storage)->program(material), material).second)
                    material_sources_.push_back(material);
            }
            if (changed) rebuildDrawOrder();
            prepared_revision_ = revision;
            co_return RenderResult<void>{};
        }
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
            if (a.pipeline != b.pipeline)
            {
                return std::less<nri::Pipeline*>{}(a.pipeline, b.pipeline);
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

    RenderResult<FeatureBinding::Preparation>
    RasterMeshDraws::prepare(const FeatureFrameInput& frame, ResourceUploader&) noexcept
    {
        if (requiresPrograms())
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const auto view_count = configuration_.views.index != kGraphInvalidIndex ? source().view_count : 1u;
        auto& draws = instance_.rasterDraws(configuration_.pass, frame.slot, false);
        if (configuration_.views.index != kGraphInvalidIndex)
        {
            auto prepared = instance_.prepareImageLayers(configuration_.depth, frame.slot, view_count);
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
            const auto& compiled = *instance_.compiledPass(configuration_.pass);
            const auto& stage = std::get<RasterListCommands>(std::get<RasterCommands>(compiled.work).draw).description;
            draws.parameters = stage.parameters.values;
        }
        updateViewParameters(frame, draws);
        if (*refreshed)
        {
            mergeDrawPackets(draws);
            instance_.invalidateRasterDraws(configuration_.pass, frame.slot);
        }
        slice.draw_revision = draw_layout_revision_;
        return FeatureBinding::Preparation{};
    }

    RenderResult<bool>
    RasterMeshDraws::refreshDrawPackets(const FeatureFrameInput& frame, Slice& slice, RasterDrawList& draws) noexcept
    {
        const auto view_count = slice.view_count;
        const auto packet_count = view_count * draw_order_.size();
        const bool shared_members = device_.enabledFeatures().indirect_first_instance;
        draws.descriptors.resize(shared_members ? program_bindings_.size() : packet_count);
        const auto transforms = instance_.resourceInput(configuration_.inputs[1], frame.slot);
        const auto properties = instance_.resourceInput(configuration_.inputs[2], frame.slot);
        const auto alive = instance_.resourceInput(configuration_.inputs[3], frame.slot);
        const auto visible = instance_.resourceInput(configuration_.inputs[4], frame.slot);
        const auto commands = instance_.resourceInput(configuration_.inputs[5], frame.slot);
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
            const auto& group = source().groups[order / kMeshWindingCount];
            const auto command = std::uint64_t(group.gpu_index) * source().view_count * kMeshWindingCount +
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
                const auto& group = source().groups[index];
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
                packet.pipeline = program.pipeline;
                packet.bindings = &program.bindings;
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
                const auto command = std::uint64_t(group.gpu_index) * source().view_count * kMeshWindingCount + stream;
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
                left.bindings->layout == right.bindings->layout;
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
    RasterMeshDraws::resolveFrameResources(const FeatureFrameInput& frame, RasterDrawList& draws) noexcept
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
        const auto& draw_source = source();
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
        StorageId mesh_storage_id;
        IndexedTriangleMeshStorage* mesh_storage = nullptr;
        for (std::uint32_t index = 0; index < groups_.size(); ++index)
        {
            auto& group = groups_[index];
            if (group.generation == 0) continue;
            const auto mesh = draw_source.groups[index].selection.mesh;
            const auto storage_id = extension_detail::ResourceIdentity::storage(mesh);
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
        for (const auto material : material_sources_)
        {
            auto storage = resources_.borrow<RasterMaterialStorage>(
                extension_detail::ResourceIdentity::storage(material)
            );
            if (!storage) return cxx::unexpected(storage.error());
            auto collected = (*storage)->collectReads(material, frame.slot, read_buffers_, read_images_);
            if (!collected) return collected;
        }
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

    void RasterMeshDraws::updateViewParameters(const FeatureFrameInput& frame, RasterDrawList& draws) noexcept
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
        if (configuration_.views.index == kGraphInvalidIndex)
        {
            copy(draws.parameters.data(), frame.parameters.data() + camera_offsets_[0], 64);
            copy(draws.parameters.data() + 64, frame.parameters.data() + camera_offsets_[1], 64);
            if (changed) instance_.invalidateRasterDraws(configuration_.pass, frame.slot);
            return;
        }
        const auto view_count = static_cast<std::uint32_t>(draws.sections.size());
        draws.parameters.resize(view_count * sizeof(MeshViewParameters));
        const auto views = instance_.resourceInput(configuration_.views, frame.slot).parameters;
        const auto states = instance_.resourceInput(configuration_.raster_states, frame.slot).parameters;
        const auto input = instance_.resourceInput(configuration_.depth, frame.slot);
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
        if (changed) instance_.invalidateRasterDraws(configuration_.pass, frame.slot);
    }

    RenderResult<FeatureBinding> RasterMeshDraws::create(
        DeviceSession& device,
        FrameExecution&,
        ResourceRegistry& resources,
        RenderGraphInstance& instance,
        CompiledPrograms& programs,
        std::span<const StorageId>,
        const void* input
    ) noexcept
    {
        const auto configuration = *static_cast<const MeshDrawConfiguration*>(input);
        const auto source = instance.resourceInput(configuration.inputs[0], 0);
        if (source.parameters.size() != sizeof(MeshDrawGroupSource))
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        auto value =
            std::unique_ptr<RasterMeshDraws>(new RasterMeshDraws(device, resources, instance, programs, configuration));
        const auto& stage =
            std::get<RasterListCommands>(std::get<RasterCommands>(instance.compiledPass(configuration.pass)->work).draw)
                .description;
        constexpr std::array<std::string_view, 2> names{"view", "projection"};
        for (std::uint32_t i = 0; configuration.views.index == kGraphInvalidIndex && i < names.size(); ++i)
        {
            const auto field =
                std::ranges::find(stage.parameters.instance_layout.fields, names[i], &rdesc::ShaderMember::path);
            if (field == stage.parameters.instance_layout.fields.end() ||
                field->offset > stage.parameters.instance_layout.size ||
                stage.parameters.instance_layout.size - field->offset < 64)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            value->camera_offsets_[i] = field->offset;
        }
        return FeatureBinding{
            {value.release(), [](void* data) noexcept { delete static_cast<RasterMeshDraws*>(data); }},
            FeatureBinding::RasterPreparation{
                [](void* data, const FeatureFrameInput& frame, ResourceUploader& uploads) noexcept
                { return static_cast<RasterMeshDraws*>(data)->prepare(frame, uploads); },
                [](const void* data) noexcept { return static_cast<const RasterMeshDraws*>(data)->requiresPrograms(); },
                [](void* data, execution::CpuScheduler cpu) noexcept
                { return static_cast<RasterMeshDraws*>(data)->preparePrograms(cpu); }
            }
        };
    }
} // namespace lux::render::detail
