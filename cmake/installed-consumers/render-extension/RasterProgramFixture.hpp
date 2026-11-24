#pragma once

#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include "Support.hpp"
#include <array>
#include <chrono>
#include <thread>
#include <cstring>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <lux/engine/render/resource/RasterProgramSet.hpp>
#include "SdkFullscreen.hpp"
#include "SdkGreen.hpp"
#include "SdkRed.hpp"
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>

#include "SdkRecordColor.hpp"

namespace sdk_programs
{
    inline void attachVertexShader(lux::rdesc::ShaderProgramArtifact& shader, const lux::rdesc::ShaderProgramArtifact& vertex)
    {
        shader.interface.vertex_inputs = vertex.interface.vertex_inputs;
        shader.interface.entry_points.insert(
            shader.interface.entry_points.begin(), vertex.interface.entry_points.begin(), vertex.interface.entry_points.end()
        );
        shader.stages.insert(shader.stages.begin(), vertex.stages.front());
    }

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

    struct DrawOrder final { std::uint32_t swapped; };

    struct SetDrawOrder final
    {
        static constexpr std::string_view name = "test.sdk.draw_order.set";
        struct Request final
        {
            ResourceInstanceId resource;
            DrawOrder order;
        };
        using Data = Request;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct PrepareRecordPrograms final
    {
        static constexpr std::string_view name = "test.sdk.prepare_record_programs";
        struct Request final { ResourceInstanceId resource; bool clear = false; };
        using Data = Request;
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& request) noexcept { return request.resource; }
    };

    struct NativeRasterProbe final
    {
        explicit NativeRasterProbe(detail::DeviceSession& device) noexcept
            : core(const_cast<nri::CoreInterface&>(device.nriCore()))
        {
            native_create = core.CreateGraphicsPipeline;
            native_destroy = core.DestroyPipeline;
            core.DestroyPipeline = [](nri::Pipeline* pipeline) noexcept
            {
                auto tracked = pipeline;
                const bool observed = pipeline && first_candidate.compare_exchange_strong(tracked, nullptr);
                native_destroy(pipeline);
                if (observed) ++candidate_destroys;
            };
            core.CreateGraphicsPipeline = [](nri::Device& device, const nri::GraphicsPipelineDesc& desc,
                                             nri::Pipeline*& pipeline) noexcept
            {
                if (fail_after_prefix && first_candidate.load()) return nri::Result::OUT_OF_MEMORY;
                const auto result = native_create(device, desc, pipeline);
                if (result == nri::Result::SUCCESS)
                {
                    if (watch_next.exchange(false)) first_candidate = pipeline;
                    ++created;
                }
                return result;
            };
        }

        NativeRasterProbe(const NativeRasterProbe&) = delete;
        NativeRasterProbe& operator=(const NativeRasterProbe&) = delete;

        ~NativeRasterProbe() noexcept
        {
            core.CreateGraphicsPipeline = native_create;
            core.DestroyPipeline = native_destroy;
        }

        static void waitForRetirement(unsigned expected) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (candidate_destroys != expected)
            {
                CHECK(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
        }

        static inline std::atomic_bool watch_next{}, fail_after_prefix{};
        static inline std::atomic<unsigned> candidate_destroys{};
        static inline std::atomic<unsigned> created{};

    private:
        static inline decltype(nri::CoreInterface::CreateGraphicsPipeline) native_create{};
        static inline decltype(nri::CoreInterface::DestroyPipeline) native_destroy{};
        static inline std::atomic<nri::Pipeline*> first_candidate{};
        nri::CoreInterface& core;
    };

    class TriangleBatch final
    {
    public:
        using Descriptor = TriangleBatch;
        using PerFrameData = std::tuple<DrawOrder>;
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.sdk.triangle"}, 0, 0, {}, "Indexed fullscreen draws.", EResourceDomain::DEVICE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

        explicit TriangleBatch(ResourceCreateContext& context) noexcept
            : device_(context.device), probe_(context.device), programs(context) {}
        TriangleBatch(const TriangleBatch&) = delete;
        TriangleBatch& operator=(const TriangleBatch&) = delete;

        ~TriangleBatch() noexcept
        {
            if (buffer_)
                device_.deferRelease([destroy = device_.nriCore().DestroyBuffer, buffer = buffer_]() noexcept
                {
                    destroy(buffer);
                });
        }

        static RenderResult<std::unique_ptr<TriangleBatch>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            auto asset = std::make_unique<TriangleBatch>(context);
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

        static RenderResult<void> apply(TriangleBatch& storage, std::span<const DrawOrder> values) noexcept
        {
            if (std::ranges::any_of(values, [](DrawOrder value) noexcept { return value.swapped > 1; }))
                return lux::cxx::unexpected(RenderError{kFrameContract});
            if (!values.empty()) storage.swapped = values.back().swapped != 0;
            return {};
        }

        static void setOrder(TriangleBatch& storage, const SetDrawOrder::Request& input, AckResponse& reply) noexcept
        {
            const auto result = apply(storage, std::span{&input.order, 1});
            reply.error = result ? RenderError{} : result.error();
        }

        static ResourceRegistration factory() noexcept
        {
            return makeResourceRegistration<Descriptor, TriangleBatch, &create, &apply>(nullptr);
        }

        bool swapped = false;

        IndexedGeometryInput input(std::span<const GeometryAttribute> attributes) const noexcept
        {
            return {0, {buffer_, 8, 24, 8}, {buffer_, 32, 6, 2}, attributes};
        }

    private:
        detail::DeviceSession& device_;
        nri::Buffer* buffer_ = nullptr;
        NativeRasterProbe probe_;

    public:
        RasterProgramSet programs;
        static exec::task<AckResponse> preparePrograms(TriangleBatch&, PrepareRecordPrograms::Request&&) noexcept;
    };

    class ColorPage final
    {
    public:
        using Descriptor = ColorPage;
        using Config = void;

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
            return makeResourceRegistration<Descriptor, ColorPage, &create>();
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
            return makeResourceRegistration<Descriptor, DrawMask, &create>();
        }
    };

    struct ProgramDraws final
    {
        using Configuration = bool;

        struct LinkedData final
        {
            DrawStreamSlot stream;
            DrawResourceSlot<DrawMask> mask;
            DrawResourceSlot<TriangleBatch> geometry;
            std::optional<DrawResourceSlot<ColorPage>> records;
            std::array<RasterProgramSlot, 3> programs;
        };

        struct Workspace final
        {
            std::array<GeometryAttribute, 1> attributes;
            bool probed_geometry = false;
        };

        static RenderResult<LinkedData>
        resolve(const Configuration& input, std::span<const RasterOperands, 1> streams) noexcept
        {
            const auto undeclared = streams.front().resource<DrawMask>("absent");
            CHECK(!undeclared && undeclared.error().type == kGraphUnknownResource);
            const auto wrong_type = streams.front().resource<TriangleBatch>("clip_height");
            CHECK(!wrong_type && wrong_type.error().type == kStorageMismatch);
            auto mask = streams.front().resource<DrawMask>("clip_height");
            if (!mask) return lux::cxx::unexpected(mask.error());
            const auto geometry = streams.front().resource<TriangleBatch>("triangle");
            if (!geometry) return lux::cxx::unexpected(geometry.error());
            LinkedData linked{streams.front().stream(), *mask, *geometry, {}, {}};
            for (std::uint32_t i = 0; !input && i < linked.programs.size(); ++i)
            {
                const auto program = streams.front().programAt(i);
                if (!program) return lux::cxx::unexpected(program.error());
                linked.programs[i] = *program;
            }
            const auto missing = streams.front().programAt(static_cast<std::uint32_t>(linked.programs.size()));
            CHECK(!missing && missing.error().type == kGraphInvalidDescriptor);
            if (input)
            {
                const auto records = streams.front().resource<ColorPage>("colors");
                if (!records) return lux::cxx::unexpected(records.error());
                linked.records = *records;
            }
            return linked;
        }

        static std::vector<RasterPassTemplate::Input> arguments(bool records)
        {
            std::vector<RasterPassTemplate::Input> inputs{{"vertices", {}, EGraphUse::VERTEX}, {"indices", {}, EGraphUse::INDEX}};
            if (records)
                inputs.push_back({
                    .name = "colorPages", .kind = lux::rdesc::EDescriptorType::STORAGE_BUFFER,
                    .usage = EGraphUse::STORAGE_READ, .stages = 2, .shared_records = true
                });
            return inputs;
        }

        static RasterProgramSet::Description describe(lux::rdesc::EShaderTarget target, bool records)
        {
            RasterProgramSet::Description result;
            result.colors = {lux::rdesc::ETextureFormat::RGBA8_UNORM};
            result.arguments = arguments(records);
            result.shared_records = records;
            auto vertex = kSdkFullscreen.load(target);
            auto& shaders = result.shaders;
            shaders.push_back((records ? kSdkRecordColor : kSdkRed).load(target));
            shaders.push_back(kSdkGreen.load(target));
            CHECK(vertex.stages.front().entry == "Fullscreen" && vertex.interface.vertex_inputs.size() == 1);
            CHECK(shaders[0].stages.front().entry == (records ? "RecordColor" : "Red"));
            CHECK(shaders[1].stages.front().entry == "Green");
            for (auto& shader : shaders) attachVertexShader(shader, vertex);
            std::vector<RasterPassTemplate::StreamProgram> programs(2);
            for (std::uint32_t i = 0; i < programs.size(); ++i)
            {
                programs[i].program = 1 - i;
                programs[i].graphics.cull_mode = ECullMode::NONE;
                programs[i].graphics.vertex_bindings = {{0, 8}};
                programs[i].graphics.vertex_attributes = {{
                    vertex.interface.vertex_inputs.front().location, 0, 0, lux::rdesc::ETextureFormat::RG32_SFLOAT
                }};
                programs[i].geometry_locations = {3};
            }
            if (records) programs[1].parameters.setInlineConstant(ColorIndex{});
            auto transparent = programs[1];
            transparent.graphics.blend.enabled = true;
            transparent.graphics.blend.source_color = EBlendFactor::ZERO;
            transparent.graphics.blend.destination_color = EBlendFactor::ONE;
            programs.push_back(std::move(transparent));
            result.programs = std::move(programs);
            return result;
        }

        static RenderResult<void>
        prepare(
            const LinkedData& linked,
            const CpuFrameInputs<const ViewGpuParameters&>& inputs,
            Workspace& workspace
        ) noexcept
        {
            auto& frame = inputs.frame;
            if (linked.records)
            {
                auto ready = frame.selectRasterPrograms(linked.geometry, &TriangleBatch::programs);
                if (!ready) return ready;
            }
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
            const std::array indices = (*source)->swapped ? std::array{0u, 2u} : std::array{1u, 0u};
            for (std::uint32_t i = 0; i < draws.size(); ++i)
            {
                auto program = linked.programs[indices[i]];
                if (linked.records)
                {
                    const auto selected = frame.rasterProgram(linked.stream, indices[i]);
                    if (!selected) return lux::cxx::unexpected(selected.error());
                    program = *selected;
                }
                draws[i] = {
                    program, {}, {0, 1}, {},
                    RasterStreamIndexed{selected_geometry->indices, selected_geometry->index_type, 3}
                };
                if (linked.records && program.index != 0)
                {
                    auto writer = frame.rasterRecord(*linked.records, program);
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

    inline exec::task<AckResponse>
    TriangleBatch::preparePrograms(TriangleBatch& storage, PrepareRecordPrograms::Request&& request) noexcept
    {
        auto description = ProgramDraws::describe(storage.device_.enabledFeatures().shader_target, true);
        if (request.clear) description.programs.clear();
        const auto result = co_await storage.programs.replace(std::move(description));
        co_return AckResponse{result ? RenderError{} : result.error()};
    }
} // namespace sdk_programs
