#pragma once

#include "Support.hpp"
#include <exec/create.hpp>
#include <lux/engine/render/resources/view/Parameters.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include "SdkFullscreen.hpp"
#include "SdkGreen.hpp"
#include "SdkRed.hpp"
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>

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

    // A separate runtime stream with no mutable inputs clears the column outside the draw viewport.
    struct ClearBackground final
    {
        using Configuration = GraphPassId;
        using LinkedData = DrawStreamSlot;
        struct Workspace final {};

        static RenderResult<LinkedData> resolve(const Configuration& pass, InstanceLinkContext& link) noexcept
        {
            return link.resolveDrawStream(pass);
        }

        static RenderResult<void> buildPrograms(const LinkedData& stream, RasterProgramBuildContext& build) noexcept
        {
            ++CodeStorage::clear_builds;
            return build.setPrograms(stream, {}, {});
        }

        static RenderResult<void> prepare(const LinkedData& stream, FramePreparation& frame, Workspace&) noexcept
        {
            return frame.setRasterStream(stream, {});
        }
    };

    struct ProgramDraws final
    {
        struct Configuration final
        {
            GraphPassId pass;
            GraphStorageSnapshotId code, camera;
            lux::rdesc::EShaderTarget target;
            bool stale;
        };

        struct LinkedData final
        {
            DrawStreamSlot stream;
            StorageSnapshotSlot<CodeSnapshot> code;
            StorageSnapshotSlot<ViewGpuParameters> camera;
            lux::rdesc::EShaderTarget target;
            bool stale;
        };

        struct Workspace final {};

        static RenderResult<LinkedData> resolve(const Configuration& input, InstanceLinkContext& link) noexcept
        {
            auto stream = link.resolveDrawStream(input.pass);
            if (!stream) return lux::cxx::unexpected(stream.error());
            auto code = link.resolveStorageSnapshot<CodeSnapshot>(input.code);
            if (!code) return lux::cxx::unexpected(code.error());
            const auto camera = link.resolveStorageSnapshot<ViewGpuParameters>(input.camera);
            if (!camera) return lux::cxx::unexpected(camera.error());
            return LinkedData{*stream, *code, *camera, input.target, input.stale};
        }

        using DrawMapping = std::array<std::uint32_t, 2>;

        static RenderResult<std::unique_ptr<DrawMapping>>
        buildPrograms(
            const LinkedData& linked,
            RasterProgramBuildContext& selection,
            const DrawMapping* previous
        ) noexcept
        {
            auto code = selection.readStorage(linked.code);
            if (!code) return lux::cxx::unexpected(code.error());
            ++CodeStorage::builds;
            const auto& snapshot = code->value.get();
            const auto mapping = snapshot.swapped ? DrawMapping{0, 1} : DrawMapping{1, 0};
            if (previous)
            {
                auto reused = selection.reusePrograms(linked.stream);
                if (!reused) return lux::cxx::unexpected(reused.error());
                return std::make_unique<DrawMapping>(mapping);
            }
            auto vertex = kSdkFullscreen.load(linked.target);
            std::vector<lux::rdesc::ShaderProgramArtifact> shaders{
                kSdkRed.load(linked.target), kSdkGreen.load(linked.target)
            };
            CHECK(vertex.stages.front().entry == "Fullscreen");
            CHECK(shaders[0].stages.front().entry == "Red" && shaders[1].stages.front().entry == "Green");
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
                programs[i].source = ResourceProgramSource::from<CodeStorage, &CodeStorage::lookup>(item);
            }
            auto built = selection.setPrograms(linked.stream, std::move(shaders), programs);
            if (!built) return lux::cxx::unexpected(built.error());
            return std::make_unique<DrawMapping>(mapping);
        }

        static RenderResult<void>
        prepare(const LinkedData& linked, FramePreparation& frame, Workspace&, const DrawMapping* mapping) noexcept
        {
            CHECK(mapping);
            std::array<RasterStreamDraw, 2> draws{};
            for (std::uint32_t i = 0; i < draws.size(); ++i)
            {
                auto program = frame.rasterProgram(linked.stream, (*mapping)[i]);
                if (!program) return lux::cxx::unexpected(program.error());
                draws[i] = {*program, {}, {}, {}, DrawVertices{3}};
            }
            const auto camera = frame.readStorage(linked.camera);
            if (!camera) return lux::cxx::unexpected(camera.error());
            const auto& viewport = camera->value.get().viewport;
            const RasterViewport selected{viewport[0], viewport[1], viewport[2], viewport[3]};
            const RasterStreamSection section{{0, 2}, 0, selected};
            return frame.setRasterStream(linked.stream, {draws, {}, {}, {}, std::span{&section, 1}});
        }
    };

    // These builders have valid signatures but cannot produce the values required by ProgramDraws.
    struct WrongProgramInputs final
    {
        using LinkedData = std::uint32_t;
        using DrawMapping = ProgramDraws::DrawMapping;

        static RenderResult<std::unique_ptr<DrawMapping>>
        build(const LinkedData&, RasterProgramBuildContext&, const DrawMapping*) noexcept
        {
            CHECK(false);
            return lux::cxx::unexpected(RenderError{kFrameContract});
        }
    };

    struct MissingDrawMapping final
    {
        using LinkedData = ProgramDraws::LinkedData;

        static RenderResult<void> build(const LinkedData&, RasterProgramBuildContext&) noexcept
        {
            CHECK(false);
            return lux::cxx::unexpected(RenderError{kFrameContract});
        }
    };
} // namespace sdk_programs
