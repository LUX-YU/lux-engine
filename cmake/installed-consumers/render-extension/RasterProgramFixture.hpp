#pragma once

#include "Support.hpp"
#include <array>
#include <cstring>
#include <exec/create.hpp>
#include <lux/engine/render/resources/view/Parameters.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include "SdkFullscreen.hpp"
#include "SdkGreen.hpp"
#include "SdkRed.hpp"
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>

#include "SdkRecordColor.hpp"

namespace sdk_programs
{
    struct ColorIndex final { std::uint32_t page, index; };
}

template <> struct lux::render::ConstantBlockTraits<sdk_programs::ColorIndex> final
{
    static ConstantBlockLayout layout(lux::rdesc::EShaderTarget target) noexcept
    {
        constexpr auto scalar = lux::rdesc::EDataScalar::UINT;
        static constexpr std::array members{
            ConstantMemberLayout{"page", 0, scalar}, ConstantMemberLayout{"index", 4, scalar}
        };
        const auto alignment = target == lux::rdesc::EShaderTarget::DXIL ? 16u : 4u;
        return {sizeof(sdk_programs::ColorIndex), alignment, members};
    }
};

namespace sdk_programs
{
    using namespace lux::render;
    using ItemId = ResourceId<struct ShaderItem>;

    struct DrawOrder final { std::uint32_t swapped; };

    struct CodeSnapshot final
    {
        ItemId item;
        bool swapped;
    };

    struct SetDrawOrder final
    {
        static constexpr std::string_view name = "test.sdk.shader_code.set_order";
        struct Request final
        {
            ResourceInstanceId resource;
            DrawOrder order;
        };
        using Data = Request;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct HoldCode final
    {
        static constexpr std::string_view name = "test.sdk.shader_code.hold";
        static constexpr bool requires_idle_resource = true;
        using Data = ResourceInstanceId;
        using Request = ResourceInstanceId;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(Request id) noexcept { return id; }
    };

    struct ReleaseCode final
    {
        static constexpr std::string_view name = "test.sdk.shader_code.resume";
        using Request = EmptyRequest;
        using Response = AckResponse;
    };

    class TriangleGeometry final
    {
    public:
        using Descriptor = TriangleGeometry;
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.triangle"}, 0, 0, {}, "Indexed fullscreen triangle.", EResourceDomain::DEVICE
        };

        explicit TriangleGeometry(detail::DeviceSession& device) noexcept : device_(device) {}
        TriangleGeometry(const TriangleGeometry&) = delete;
        TriangleGeometry& operator=(const TriangleGeometry&) = delete;

        ~TriangleGeometry() noexcept
        {
            if (buffer_)
                device_.deferRelease([destroy = device_.nriCore().DestroyBuffer, buffer = buffer_]() noexcept
                {
                    destroy(buffer);
                });
        }

        static RenderResult<std::unique_ptr<TriangleGeometry>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            auto asset = std::make_unique<TriangleGeometry>(context.device);
            const auto& core = context.device.nriCore();
            const nri::BufferDesc desc{.size = 40, .usage = nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX};
            const auto created = core.CreatePlacedBuffer(
                context.device.device(), NriHostUploadHeap, desc, asset->buffer_
            );
            if (created != nri::Result::SUCCESS) return lux::cxx::unexpected(RenderError{kDeviceFailure});
            auto* mapped = static_cast<std::byte*>(core.MapBuffer(*asset->buffer_, 0, desc.size));
            if (!mapped) return lux::cxx::unexpected(RenderError{kDeviceFailure});
            constexpr std::array positions{-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
            constexpr std::array<std::uint16_t, 3> indices{2, 0, 1};
            std::memcpy(mapped + 8, positions.data(), sizeof(positions));
            std::memcpy(mapped + 32, indices.data(), sizeof(indices));
            core.UnmapBuffer(*asset->buffer_);
            return asset;
        }

        static ResourceRegistration factory() noexcept
        {
            return makeResourceRegistration<Descriptor, TriangleGeometry, &create>(nullptr, nullptr);
        }

        IndexedGeometryInput input(std::span<const GeometryAttribute> attributes) const noexcept
        {
            return {0, {buffer_, 8, 24, 8}, {buffer_, 32, 6, 2}, attributes};
        }

    private:
        detail::DeviceSession& device_;
        nri::Buffer* buffer_ = nullptr;
    };

    class ColorPage final
    {
    public:
        using Descriptor = ColorPage;
        using Config = void;
        using Ops = void;
        static constexpr auto capability = static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY);
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.color_page"}, capability, 0, {}, "Two RGBA records.", EResourceDomain::DEVICE
        };

        explicit ColorPage(detail::DeviceSession& device) noexcept : device_(device) {}
        ColorPage(const ColorPage&) = delete;
        ColorPage& operator=(const ColorPage&) = delete;

        ~ColorPage() noexcept
        {
            device_.deferRelease([device = &device_, slot = slot_, view = view_, buffer = buffer_]() noexcept
            {
                if (!slot.isNull()) device->descriptors().release(nri::DescriptorType::STRUCTURED_BUFFER, slot);
                if (view) device->nriCore().DestroyDescriptor(view);
                if (buffer) device->nriCore().DestroyBuffer(buffer);
            });
        }

        static RenderResult<std::unique_ptr<ColorPage>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            auto page = std::make_unique<ColorPage>(context.device);
            const auto& core = context.device.nriCore();
            constexpr std::array colors{0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 1.f};
            const nri::BufferDesc desc{sizeof(colors), 16, nri::BufferUsageBits::SHADER_RESOURCE};
            const auto created = core.CreatePlacedBuffer(
                context.device.device(), NriHostUploadHeap, desc, page->buffer_
            );
            if (created != nri::Result::SUCCESS) return lux::cxx::unexpected(RenderError{kDeviceFailure});
            auto* mapped = core.MapBuffer(*page->buffer_, 0, desc.size);
            if (!mapped) return lux::cxx::unexpected(RenderError{kDeviceFailure});
            std::memcpy(mapped, colors.data(), sizeof(colors));
            core.UnmapBuffer(*page->buffer_);
            const nri::BufferViewDesc view{page->buffer_, nri::BufferView::STRUCTURED_BUFFER, 0, desc.size};
            if (core.CreateBufferView(view, page->view_) != nri::Result::SUCCESS)
                return lux::cxx::unexpected(RenderError{kDeviceFailure});
            auto slot = context.device.descriptors().allocate(nri::DescriptorType::STRUCTURED_BUFFER);
            if (!slot) return lux::cxx::unexpected(slot.error());
            page->slot_ = *slot;
            context.device.descriptors().write(nri::DescriptorType::STRUCTURED_BUFFER, *slot, page->view_);
            return page;
        }

        static ResourceRegistration factory() noexcept
        {
            return makeResourceRegistration<Descriptor, ColorPage, &create>(nullptr, nullptr);
        }

        ColorIndex select(const RasterRecordWriter& writer) const noexcept
        {
            const auto& layout = writer.layout();
            CHECK(layout.block.runtime_records && layout.block.size == 16 && layout.frame_slot == 0 && !layout.reads_images);
            const RasterRecordInput record{buffer_, {16, 16}, {}};
            CHECK(writer.write(record));
            auto invalid = record;
            invalid.range.offset = 1;
            const auto rejected = writer.write(invalid);
            CHECK(!rejected && rejected.error().type == kGraphInvalidRange);
            invalid.range = {32, 16};
            const auto past_end = writer.write(invalid);
            CHECK(!past_end && past_end.error().type == kGraphInvalidRange);
            return {slot_.index, 1};
        }

    private:
        detail::DeviceSession& device_;
        nri::Buffer* buffer_ = nullptr;
        nri::Descriptor* view_ = nullptr;
        detail::DescriptorTable::Slot slot_{};
    };

    struct DrawMask final
    {
        using Descriptor = DrawMask;
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.draw_mask"}, 0, 0, {}, "Fraction of the output height to draw.",
            EResourceDomain::DEVICE
        };
        float height = 0.5f;

        static RenderResult<std::unique_ptr<DrawMask>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
        {
            return std::make_unique<DrawMask>();
        }

        static ResourceRegistration factory() noexcept
        {
            return makeResourceRegistration<Descriptor, DrawMask, &create>(nullptr, nullptr);
        }
    };

    enum class EDriverGate { OPEN, ARMED, HELD };
    struct CodeStorage final
    {
        using Descriptor = CodeStorage;
        using Config = void;
        using Ops = void;
        using Commands = std::tuple<HoldCode, SetDrawOrder>;
        using PerFrameData = std::tuple<DrawOrder>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.shader_code"}, 0, 0, {}, "Two resident raster uses.", EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

        CodeStorage(ResourceCreateContext& context, ResourceInstanceId id) noexcept
            : core(const_cast<nri::CoreInterface&>(context.device.nriCore())),
              item(extension_detail::ResourceIdentity::make<ItemId>(id, lux::cxx::SlotKey<ShaderItem>{0, 1})),
              uses(context.device)
        {
            native_create = core.CreateGraphicsPipeline;
            core.CreateGraphicsPipeline = [](nri::Device& device, const nri::GraphicsPipelineDesc& desc,
                                             nri::Pipeline*& pipeline) noexcept
            {
                auto expected = EDriverGate::ARMED;
                if (gate.compare_exchange_strong(expected, EDriverGate::HELD)) gate.wait(EDriverGate::HELD);
                const auto result = native_create(device, desc, pipeline);
                if (result == nri::Result::SUCCESS) ++created;
                return result;
            };
        }

        ~CodeStorage() noexcept { core.CreateGraphicsPipeline = native_create; }

        static exec::task<AckResponse> hold(CodeStorage&, HoldCode::Request&&) noexcept
        {
            co_return co_await exec::create<stdexec::set_value_t(AckResponse)>([](auto& context) noexcept
            {
                using Context = std::remove_cvref_t<decltype(context)>;
                CHECK(!pending);
                pending = &context;
                resume = [](void* value) noexcept
                {
                    auto& context = *static_cast<Context*>(value);
                    stdexec::set_value(std::move(context.receiver), AckResponse{});
                };
                held.store(true);
            });
        }

        static void release(const EmptyRequest&, AckResponse& output) noexcept
        {
            if (!pending) { output.error = {kRuntimeState}; return; }
            output = {};
            held.store(false);
            resume(std::exchange(pending, nullptr));
        }

        ResourceProgramUses* lookup(ItemId requested) noexcept
        {
            CHECK(!held.load());
            return requested == item ? &uses : nullptr;
        }

        static RenderResult<void> apply(CodeStorage& storage, std::span<const DrawOrder> values) noexcept
        {
            if (std::ranges::any_of(values, [](DrawOrder value) noexcept { return value.swapped > 1; }))
                return lux::cxx::unexpected(RenderError{kFrameContract});
            if (!values.empty() && storage.swapped != bool(values.back().swapped))
            {
                storage.swapped = values.back().swapped != 0;
                storage.revision.advance();
            }
            return {};
        }

        static void setOrder(CodeStorage& storage, const SetDrawOrder::Request& input, AckResponse& reply) noexcept
        {
            const auto result = apply(storage, std::span{&input.order, 1});
            reply.error = result ? RenderError{} : result.error();
        }

        RenderResult<std::uint64_t> capture(CodeSnapshot& output) const noexcept
        {
            ++captures;
            output = {item, swapped};
            return revision.value();
        }

        static RenderResult<std::unique_ptr<CodeStorage>>
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            return std::make_unique<CodeStorage>(context, id);
        }

        static ResourceRegistration factory() noexcept
        {
            static constexpr auto snapshot = []() noexcept
            {
                auto ops = detail::StorageSnapshotOps::make<CodeStorage, CodeSnapshot, &CodeStorage::capture>();
                ops.program_input = [](void* object) noexcept -> ProgramInputRevision&
                {
                    ++source_links;
                    return static_cast<CodeStorage*>(object)->revision;
                };
                return ops;
            }();
            static const detail::StorageBindingOps binding{.cpu_snapshot = &snapshot};
            static const std::array commands{
                CommandBinding::declare<HoldCode, &hold>(), CommandBinding::declare<SetDrawOrder, &setOrder>()
            };
            static const auto frame = FrameDataCall::declare<CodeStorage, CodeStorage, &apply>();
            return makeResourceRegistration<Descriptor, CodeStorage, &create>(nullptr, &binding, commands, frame);
        }

        static inline std::atomic gate{EDriverGate::OPEN};
        static inline std::atomic<unsigned> created{}, builds{}, clear_builds{}, source_links{}, captures{};
        static inline std::atomic_bool held{};

    private:
        static inline decltype(nri::CoreInterface::CreateGraphicsPipeline) native_create{};
        static inline void* pending = nullptr;
        static inline void (*resume)(void*) noexcept = nullptr;
        nri::CoreInterface& core;
        bool swapped = false;
        ProgramInputRevision revision;
        ItemId item;
        ResourceProgramUses uses;
    };

    template <auto Lookup>
    concept ProgramLookup = requires(ItemId item) { ResourceProgramSource::from<CodeStorage, Lookup>(item); };

    static_assert(ProgramLookup<&CodeStorage::lookup>);
    static_assert(!ProgramLookup<&CodeStorage::capture>);

    struct SetBackground final
    {
        static constexpr std::string_view name = "test.sdk.background.set";
        struct Request final { ResourceInstanceId resource; std::uint32_t value; };
        using Data = Request;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct BackgroundSettings final
    {
        using Descriptor = BackgroundSettings;
        using Config = void;
        using Ops = void;
        using Commands = std::tuple<SetBackground>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.background"}, 0, 0, {}, "Background program input.", EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };

        static RenderResult<std::unique_ptr<BackgroundSettings>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
        {
            return std::make_unique<BackgroundSettings>();
        }

        static void set(BackgroundSettings& state, const SetBackground::Request& input, AckResponse& reply) noexcept
        {
            state.value = input.value;
            state.revision.advance();
            reply = {};
        }

        RenderResult<std::uint64_t> capture(std::uint32_t& output) const noexcept
        {
            output = value;
            return revision.value();
        }

        static ResourceRegistration factory() noexcept
        {
            static constexpr auto snapshot =
                detail::StorageSnapshotOps::make<BackgroundSettings, std::uint32_t, &BackgroundSettings::capture, &BackgroundSettings::revision>();
            static const detail::StorageBindingOps binding{.cpu_snapshot = &snapshot};
            static const std::array commands{CommandBinding::declare<SetBackground, &set>()};
            return makeResourceRegistration<Descriptor, BackgroundSettings, &create>(nullptr, &binding, commands);
        }

        std::uint32_t value = 0;
        ProgramInputRevision revision;
    };

    struct ClearBackground final
    {
        using Configuration = GraphPassId;
        struct LinkedData final { DrawStreamSlot stream; };
        static RenderResult<LinkedData> resolve(const Configuration& input, InstanceLinkContext& link) noexcept
        {
            auto stream = link.resolveDrawStream(input);
            if (!stream) return lux::cxx::unexpected(stream.error());
            return LinkedData{*stream};
        }

        static RenderResult<void> buildPrograms(
            const LinkedData& linked, RasterProgramBuildContext& build, const SnapshotValues<std::uint32_t>& sources
        ) noexcept
        {
            ++CodeStorage::clear_builds;
            if (std::get<0>(sources).value.get() == 2) return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            return build.setPrograms(linked.stream, {}, {});
        }

        static RenderResult<void> prepare(const LinkedData& linked, CpuCallFrame& frame) noexcept
        {
            return frame.setRasterStream(linked.stream, {});
        }
    };

    struct ProgramDraws final
    {
        struct Configuration final
        {
            GraphPassId pass;
            lux::rdesc::EShaderTarget target;
            bool stale, reads_records;
        };

        struct LinkedData final
        {
            DrawStreamSlot stream;
            DrawResourceSlot<DrawMask> mask;
            DrawResourceSlot<TriangleGeometry> geometry;
            std::optional<DrawResourceSlot<ColorPage>> records;
            lux::rdesc::EShaderTarget target;
            bool stale;
        };

        struct Workspace final
        {
            std::array<GeometryAttribute, 1> attributes;
            bool probed_geometry = false;
        };

        static RenderResult<LinkedData> resolve(const Configuration& input, InstanceLinkContext& link) noexcept
        {
            auto stream = link.resolveDrawStream(input.pass);
            if (!stream) return lux::cxx::unexpected(stream.error());
            const auto undeclared = link.resolveDrawResource<DrawMask>(input.pass, "absent");
            CHECK(!undeclared && undeclared.error().type == kGraphUnknownResource);
            const auto wrong_type = link.resolveDrawResource<CodeStorage>(input.pass, "clip_height");
            CHECK(!wrong_type && wrong_type.error().type == kStorageMismatch);
            auto mask = link.resolveDrawResource<DrawMask>(input.pass, "clip_height");
            if (!mask) return lux::cxx::unexpected(mask.error());
            const auto geometry = link.resolveDrawResource<TriangleGeometry>(input.pass, "triangle");
            if (!geometry) return lux::cxx::unexpected(geometry.error());
            LinkedData linked{*stream, *mask, *geometry, {}, input.target, input.stale};
            if (input.reads_records)
            {
                const auto records = link.resolveDrawResource<ColorPage>(input.pass, "colors");
                if (!records) return lux::cxx::unexpected(records.error());
                linked.records = *records;
            }
            return linked;
        }

        using DrawMapping = std::array<std::uint32_t, 2>;

        static RenderResult<std::unique_ptr<DrawMapping>>
        buildPrograms(
            const LinkedData& linked,
            RasterProgramBuildContext& selection,
            const SnapshotValues<CodeSnapshot, CodeSnapshot>& sources,
            const DrawMapping* previous
        ) noexcept
        {
            const auto& [code, alias] = sources;
            CHECK(&code.value.get() == &alias.value.get() && code.revision == alias.revision);
            ++CodeStorage::builds;
            const auto& snapshot = alias.value.get();
            const auto mapping = snapshot.swapped ? DrawMapping{0, 1} : DrawMapping{1, 0};
            if (previous)
            {
                auto reused = selection.reusePrograms(linked.stream);
                if (!reused) return lux::cxx::unexpected(reused.error());
                return std::make_unique<DrawMapping>(mapping);
            }
            auto vertex = kSdkFullscreen.load(linked.target);
            std::vector<lux::rdesc::ShaderProgramArtifact> shaders{
                (linked.records ? kSdkRecordColor : kSdkRed).load(linked.target), kSdkGreen.load(linked.target)
            };
            CHECK(vertex.stages.front().entry == "Fullscreen" && vertex.interface.vertex_inputs.size() == 1);
            CHECK(shaders[0].stages.front().entry == (linked.records ? "RecordColor" : "Red"));
            CHECK(shaders[1].stages.front().entry == "Green");
            for (auto& shader : shaders)
            {
                shader.interface.vertex_inputs = vertex.interface.vertex_inputs;
                shader.interface.entry_points.insert(shader.interface.entry_points.begin(),
                    vertex.interface.entry_points.begin(), vertex.interface.entry_points.end());
                shader.stages.insert(shader.stages.begin(), vertex.stages.front());
            }
            auto item = snapshot.item;
            if (linked.stale)
            {
                using Identity = extension_detail::ResourceIdentity;
                auto key = Identity::local(item);
                ++key.gen;
                item = Identity::make<ItemId>(Identity::parent(item), key);
            }
            std::array<RuntimeRasterProgram, 2> programs{};
            for (std::uint32_t i = 0; i < programs.size(); ++i)
            {
                programs[i].shader = {1 - i};
                programs[i].graphics.cull_mode = ECullMode::NONE;
                programs[i].graphics.vertex_bindings = {{0, 8}};
                programs[i].graphics.vertex_attributes = {{
                    vertex.interface.vertex_inputs.front().location, 0, 0, lux::rdesc::ETextureFormat::RG32_SFLOAT
                }};
                programs[i].geometry_locations = {3};
                programs[i].source = ResourceProgramSource::from<CodeStorage, &CodeStorage::lookup>(item);
            }
            if (linked.records) programs[1].parameters.setInlineConstant(ColorIndex{});
            auto built = selection.setPrograms(linked.stream, std::move(shaders), programs);
            if (!built) return lux::cxx::unexpected(built.error());
            return std::make_unique<DrawMapping>(mapping);
        }

        static RenderResult<void>
        prepare(
            const LinkedData& linked,
            const CpuFrameInputs<const ViewGpuParameters&>& inputs,
            Workspace& workspace,
            const DrawMapping* mapping
        ) noexcept
        {
            auto& frame = inputs.frame;
            CHECK(mapping);
            const auto source = frame.readDrawResource(linked.geometry);
            if (!source) return lux::cxx::unexpected(source.error());
            CHECK(*source);
            workspace.attributes[0] = {3, 0, lux::rdesc::ETextureFormat::RG32_SFLOAT};
            const auto input = (*source)->input(workspace.attributes);
            if (!workspace.probed_geometry)
            {
                auto out_of_bounds = input;
                out_of_bounds.indices.offset = UINT64_MAX - 1;
                const auto bounds = frame.bindIndexedGeometry(linked.geometry, out_of_bounds);
                CHECK(!bounds && bounds.error().type == kGraphInvalidRange);
                auto invalid_stride = input;
                invalid_stride.vertices.stride = 0;
                const auto stride = frame.bindIndexedGeometry(linked.geometry, invalid_stride);
                CHECK(!stride && stride.error().type == kGraphInvalidRange);
                const auto mask = frame.readDrawResource(linked.mask);
                CHECK(mask);
                const auto denied = frame.bindIndexedGeometry(linked.mask, input);
                const auto expected = *mask ? kStorageMismatch : kStorageIdentity;
                CHECK(!denied && denied.error().type == expected);
            }
            auto selected_geometry = frame.bindIndexedGeometry(linked.geometry, input);
            if (!selected_geometry) return lux::cxx::unexpected(selected_geometry.error());
            if (!workspace.probed_geometry)
            {
                auto misaligned = input;
                ++misaligned.vertices.offset;
                const auto rejected = frame.bindIndexedGeometry(linked.geometry, misaligned);
                CHECK(!rejected && rejected.error().type == kGraphInvalidRange);
                workspace.probed_geometry = true;
            }
            workspace.attributes[0] = {}; // Freeze must use the captured layout after the author rewrites its array.
            CHECK(selected_geometry->index_type == EIndexType::UINT16 && selected_geometry->arena_first_index == 16);
            CHECK(selected_geometry->arena_vertex_offset == 1);
            const RasterStreamVertex vertex{0, selected_geometry->vertices};
            ColorIndex color{};
            std::array<RasterStreamDraw, 2> draws{};
            for (std::uint32_t i = 0; i < draws.size(); ++i)
            {
                auto program = frame.rasterProgram(linked.stream, (*mapping)[i]);
                if (!program) return lux::cxx::unexpected(program.error());
                draws[i] = {
                    *program, {}, {0, 1}, {},
                    RasterStreamIndexed{selected_geometry->indices, selected_geometry->index_type, 3}
                };
                if (linked.records && program->index == 1)
                {
                    auto writer = frame.rasterRecord(*linked.records, *program);
                    if (!writer) return lux::cxx::unexpected(writer.error());
                    const auto page = frame.readDrawResource(*linked.records);
                    if (!page) return lux::cxx::unexpected(page.error());
                    color = (*page)->select(*writer);
                    draws[i].constants = {0, sizeof(color)};
                }
            }
            const auto& camera = std::get<0>(inputs.values);
            const auto& viewport = camera.value.get().viewport;
            const auto mask = frame.readDrawResource(linked.mask);
            if (!mask) return lux::cxx::unexpected(mask.error());
            const auto height = *mask ? (*mask)->height * viewport[3] : viewport[3];
            const RasterViewport selected{viewport[0], viewport[1], viewport[2], height};
            const RasterStreamSection section{{0, 2}, 0, selected};
            const auto constants = linked.records ? std::as_bytes(std::span{&color, 1}) : std::span<const std::byte>{};
            return frame.setRasterStream(linked.stream, {
                draws, {}, std::span{&vertex, 1}, constants, std::span{&section, 1}
            });
        }
    };

    // These builders have valid signatures but cannot produce the values required by ProgramDraws.
    struct WrongProgramInputs final
    {
        using LinkedData = std::uint32_t;
        using DrawMapping = ProgramDraws::DrawMapping;

        static RenderResult<std::unique_ptr<DrawMapping>>
        build(
            const LinkedData&, RasterProgramBuildContext&, const SnapshotValues<CodeSnapshot, CodeSnapshot>&,
            const DrawMapping*
        ) noexcept
        {
            CHECK(false);
            return lux::cxx::unexpected(RenderError{kFrameContract});
        }
    };

    struct MissingDrawMapping final
    {
        using LinkedData = ProgramDraws::LinkedData;

        static RenderResult<void>
        build(const LinkedData&, RasterProgramBuildContext&, const SnapshotValues<CodeSnapshot, CodeSnapshot>&) noexcept
        {
            CHECK(false);
            return lux::cxx::unexpected(RenderError{kFrameContract});
        }
    };
} // namespace sdk_programs
