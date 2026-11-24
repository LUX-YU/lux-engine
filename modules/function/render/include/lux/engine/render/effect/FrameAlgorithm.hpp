#pragma once

#include <lux/engine/render/resource/ResourceDefinition.hpp>

#include <lux/engine/render/effect/EffectFrameWriter.hpp>
#include <lux/engine/render/graph/GraphError.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/runtime/Target.hpp>
#include <algorithm>
#include <functional>
#include <vector>

namespace lux::render
{
    struct RasterPassTemplate;
    namespace detail { struct CompiledRasterStream; }

    // Borrows one declared stream during cold resolution. Retain only the returned slots.
    class LUX_RENDER_PUBLIC RasterOperands final
    {
    public:
        RasterOperands(const RasterOperands&) = delete;
        RasterOperands(RasterOperands&&) noexcept = default;

        [[nodiscard]] DrawStreamSlot stream() const noexcept { return pass_.compiled; }
        [[nodiscard]] RenderResult<RasterBufferSlot> buffer(GraphResourceId) const noexcept;
        [[nodiscard]] RenderResult<RasterImageSlot> image(GraphResourceId) const noexcept;
        [[nodiscard]] RenderResult<RasterProgramSlot> programAt(std::uint32_t stream_index) const noexcept;
        [[nodiscard]] RenderResult<RasterSamplerSlot> sampler(std::uint32_t) const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> argument(std::string_view) const noexcept;

        [[nodiscard]] RenderResult<RasterResourceSlot> resource(std::string_view binding, ResourceNameView type) const noexcept;

    private:
        friend struct detail::CompiledRasterStream;

        struct Pass final { GraphPassId source; DrawStreamSlot compiled; };

        RasterOperands(
            Pass pass, const RasterPassTemplate& layout,
            const detail::CompiledRasterStream& access, std::uint32_t first_source
        ) noexcept : pass_(pass), layout_(layout), access_(access), first_source_(first_source) {}

        Pass pass_;
        const RasterPassTemplate& layout_;
        const detail::CompiledRasterStream& access_;
        std::uint32_t first_source_;
    };

    template <class... T> struct EffectFrameInputs final
    {
        const TargetDesc& target;
        std::tuple<RevisionedValue<std::reference_wrapper<const T>>...> values;
    };

    namespace extension_detail
    {
        using AlgorithmWorkspace = std::unique_ptr<void, void (*)(void*) noexcept>;

        template <class... T>
        inline constexpr std::array<cxx::basic_type_info, sizeof...(T)> resource_view_types{
            cxx::make_basic_type_info<T>()...
        };

        struct InlineResultType final
        {
            std::size_t bytes;
            const ConstantBlockContract* block;
            std::uint32_t revision_offset = kGraphInvalidIndex;
        };

        template <ShaderConstantBlock T> constexpr InlineResultType resultType(std::type_identity<T>) noexcept
        {
            return {sizeof(T), &kConstantBlockContract<T>};
        }

        template <ShaderConstantBlock T>
        constexpr InlineResultType resultType(std::type_identity<RevisionedValue<T>>) noexcept
        {
            using Result = RevisionedValue<T>;
            static_assert(std::is_standard_layout_v<Result> && offsetof(Result, value) == 0);
            return {
                sizeof(Result),
                &kConstantBlockContract<T>, offsetof(Result, revision)
            };
        }

        template <class... T>
        inline constexpr std::array<InlineResultType, sizeof...(T)> inline_result_types{
            resultType(std::type_identity<T>{})...
        };

        struct SelectedResourceViews final
        {
            const TargetDesc& target;
            std::span<const RevisionedValue<const void*>> sources;
            std::span<const std::uint32_t> slots;

            template <class T> auto get(std::size_t ordinal) const noexcept
            {
                const auto selected = sources[slots[ordinal]];
                return RevisionedValue{std::cref(*static_cast<const T*>(selected.value)), selected.revision};
            }

            template <class... T> EffectFrameInputs<T...> inputs(std::type_identity<EffectFrameInputs<T...>>) const noexcept
            {
                return [&]<std::size_t... I>(std::index_sequence<I...>) noexcept
                {
                    return EffectFrameInputs<T...>{target, {get<T>(I)...}};
                }(std::index_sequence_for<T...>{});
            }
        };

        struct FrameAlgorithmOps final
        {
            RenderResult<void> (*run)(const void*, const SelectedResourceViews&, EffectFrameWriter&, void*) noexcept;
            RenderResult<AlgorithmWorkspace> (*create_workspace)(const void*, const TargetDesc&, const InstalledDrawSources&) noexcept;
            std::span<const cxx::basic_type_info> inputs;
            std::span<const InlineResultType> outputs;
        };

        struct LinkedFrameAlgorithm final
        {
            ConfigValue inputs;
            const FrameAlgorithmOps* methods;
        };

        constexpr std::span<const InlineResultType> outputTypes(std::type_identity<RenderResult<void>>) noexcept
        {
            return {};
        }

        template <class... T>
        constexpr std::span<const InlineResultType>
        outputTypes(std::type_identity<RenderResult<std::tuple<T...>>>) noexcept
        {
            return inline_result_types<T...>;
        }

        template <class Result, class Linked, class... Views, class... State>
        struct FrameSignature<
            RenderResult<Result> (*)(const Linked&, const EffectFrameInputs<Views...>&, EffectFrameWriter&, State&...) noexcept
        >
        {
            static_assert(sizeof...(State) <= 1);
            using Data = Linked;
            using Workspace = std::tuple_element_t<0, std::tuple<State..., void>>;
            static constexpr auto inputs = std::span<const cxx::basic_type_info>{resource_view_types<Views...>};
            static constexpr auto outputs = outputTypes(std::type_identity<RenderResult<Result>>{});

            template <auto Frame>
            static RenderResult<void>
            run(const void* data, const SelectedResourceViews& sources, EffectFrameWriter& writer, void* state) noexcept
            {
                const auto& linked = *static_cast<const Linked*>(data);
                const auto views = sources.inputs(std::type_identity<EffectFrameInputs<Views...>>{});
                auto result = Frame(linked, views, writer, *static_cast<State*>(state)...);
                if (!result) return cxx::unexpected(result.error());
                if constexpr (!std::is_void_v<Result>)
                {
                    auto destination = writer.inline_results_;
                    const auto store = [&destination](const auto& block) noexcept
                    {
                        const auto bytes = std::as_bytes(std::span{&block, 1});
                        std::ranges::copy(bytes, destination.begin());
                        destination = destination.subspan(bytes.size());
                    };
                    std::apply([&](const auto&... blocks) noexcept { (store(blocks), ...); }, *result);
                }
                return {};
            }
        };

        template <class Linked, class Fn> struct FrameLink;

        template <class Linked> struct FrameLink<Linked, std::nullptr_t>
        {
            using Configuration = Linked;
        };

        template <class Linked, class Config, std::size_t N>
        struct FrameLink<Linked, RenderResult<Linked> (*)(const Config&, std::span<const RasterOperands, N>) noexcept>
        {
            using Configuration = Config;
            using Operands = std::span<const RasterOperands, N>;
        };

        using BindFrameAlgorithm = RenderResult<LinkedFrameAlgorithm> (*)(ConfigValue, std::span<const RasterOperands>) noexcept;

        template <auto Frame, auto Link, auto Create> struct FrameBinding final
        {
            using Signature = FrameSignature<decltype(Frame)>;
            using Workspace = typename Signature::Workspace;
            using LinkType = FrameLink<typename Signature::Data, decltype(Link)>;
            using Configuration = typename LinkType::Configuration;
            static_assert(!std::is_void_v<Workspace> || Create == nullptr);

            static RenderResult<AlgorithmWorkspace>
            instantiate(const void* data, const TargetDesc& target, const InstalledDrawSources& sources) noexcept
            {
                std::unique_ptr<Workspace> state;
                if constexpr (Create == nullptr) state = std::make_unique<Workspace>();
                else
                {
                    auto created = Create(*static_cast<const typename Signature::Data*>(data), target, sources);
                    if (!created) return cxx::unexpected(created.error());
                    if (!*created) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    state = std::move(*created);
                }
                return AlgorithmWorkspace{state.release(), [](void* value) noexcept { delete static_cast<Workspace*>(value); }};
            }

            static RenderResult<LinkedFrameAlgorithm> bind(ConfigValue config, std::span<const RasterOperands> streams) noexcept
            {
                static constexpr FrameAlgorithmOps ops{
                    &Signature::template run<Frame>, []() constexpr -> decltype(FrameAlgorithmOps::create_workspace)
                    {
                        if constexpr (std::is_void_v<Workspace>) return nullptr;
                        else return &instantiate;
                    }(), Signature::inputs, Signature::outputs
                };
                if constexpr (Link == nullptr)
                {
                    if (!streams.empty()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    return LinkedFrameAlgorithm{std::move(config), &ops};
                }
                else
                {
                    using Operands = typename LinkType::Operands;
                    if constexpr (Operands::extent != std::dynamic_extent)
                        if (streams.size() != Operands::extent) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                    auto linked = Link(*static_cast<const Configuration*>(config.data()), Operands{streams});
                    if (!linked) return cxx::unexpected(linked.error());
                    auto data = ConfigValue::make(std::move(*linked));
                    if (!data) return cxx::unexpected(data.error());
                    return LinkedFrameAlgorithm{std::move(*data), &ops};
                }
            }
        };
    }

    // Identifies a typed CPU operand in this Effect's Resource manifest.
    struct ResourceViewSlot final
    {
        std::uint32_t index = kGraphInvalidIndex;
    };

    struct FrameAlgorithmUses final
    {
        // One complete shader block per declared pass, in result order.
        std::vector<GraphPassId> inline_blocks;
        // A live preparation retains all of its GPU passes. Inline block passes are included automatically.
        std::vector<GraphPassId> passes{};
        // Each list is addressed in declaration order within this call.
        std::vector<GraphResourceId> uploads{}, frame_sizes{};
        std::vector<ResourceViewSlot> resource_views{};
        // Ordered, exclusive draw stream outputs. Each also retains its raster pass.
        std::vector<GraphPassId> draw_streams{};
    };
} // namespace lux::render
