#pragma once

#include <lux/engine/render/effect/CpuCallFrame.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
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
        [[nodiscard]] RenderResult<RasterProgramSlot> program(std::uint32_t) const noexcept;
        [[nodiscard]] RenderResult<RasterSamplerSlot> sampler(std::uint32_t) const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> argument(std::string_view) const noexcept;

        template <class Storage>
        [[nodiscard]] RenderResult<DrawResourceSlot<Storage>> resource(std::string_view binding) const noexcept
        {
            auto address = source(binding, cxx::make_basic_type_info<Storage>());
            if (!address) return cxx::unexpected(address.error());
            return DrawResourceSlot<Storage>{*address};
        }

    private:
        friend struct detail::CompiledRasterStream;

        struct Pass final { GraphPassId source; DrawStreamSlot compiled; };

        RasterOperands(
            Pass pass, const RasterPassTemplate& layout,
            const detail::CompiledRasterStream& access, std::uint32_t first_source
        ) noexcept : pass_(pass), layout_(layout), access_(access), first_source_(first_source) {}

        [[nodiscard]] RenderResult<extension_detail::DrawSourceAddress>
        source(std::string_view, cxx::basic_type_info) const noexcept;

        Pass pass_;
        const RasterPassTemplate& layout_;
        const detail::CompiledRasterStream& access_;
        std::uint32_t first_source_;
    };

    namespace extension_detail
    {
        using AlgorithmWorkspace = std::unique_ptr<void, void (*)(void*) noexcept>;

        struct CpuValueType final
        {
            cxx::basic_type_info type;
            std::size_t bytes; // Zero denotes a borrowed Resource snapshot.
        };

        template <class... T>
        inline constexpr std::array<CpuValueType, sizeof...(T)> cpu_value_types{{
            CpuValueType{cxx::make_basic_type_info<std::remove_cvref_t<T>>(), std::is_reference_v<T> ? 0 : sizeof(T)}...
        }};

        struct CpuResultType final
        {
            CpuValueType value;
            const ConstantBlockContract* block = nullptr;
            std::uint32_t revision_offset = kGraphInvalidIndex;
        };

        template <CpuPreparationValue T> constexpr CpuResultType resultType(std::type_identity<T>) noexcept
        {
            CpuResultType result{{cxx::make_basic_type_info<T>(), sizeof(T)}};
            if constexpr (ShaderConstantBlock<T>) result.block = &kConstantBlockContract<T>;
            return result;
        }

        template <ShaderConstantBlock T>
        constexpr CpuResultType resultType(std::type_identity<RevisionedValue<T>>) noexcept
        {
            using Result = RevisionedValue<T>;
            static_assert(std::is_standard_layout_v<Result> && offsetof(Result, value) == 0);
            return {
                {cxx::make_basic_type_info<Result>(), sizeof(Result)},
                &kConstantBlockContract<T>, offsetof(Result, revision)
            };
        }

        template <CpuPreparationValue... T>
        inline constexpr std::array<CpuResultType, sizeof...(T)> cpu_result_types{
            resultType(std::type_identity<T>{})...
        };

        struct PreparationMethods final
        {
            RenderResult<void> (*prepare)(const void*, CpuCallFrame&, void*, const void*) noexcept;
            RenderResult<AlgorithmWorkspace> (*create_workspace)(const void*, const TargetDesc&) noexcept;
            cxx::basic_type_info mapping_type;
            std::span<const CpuValueType> inputs;
            std::span<const CpuResultType> outputs;
        };

        struct PreparationCall final
        {
            ConfigValue inputs;
            const PreparationMethods* methods;
        };

        template <class T> concept HasWorkspace = requires { typename T::Workspace; };
        template <class T> concept HasDrawMapping = requires { typename T::DrawMapping; };

        template <class Output, class Linked, class Frame, class... Tail>
        std::type_identity<std::remove_cvref_t<Frame>>
        frameArgument(RenderResult<Output> (*)(const Linked&, Frame, Tail...) noexcept);

        template <class Output, class... Args>
        std::type_identity<RenderResult<Output>> callResult(RenderResult<Output> (*)(Args...) noexcept);

        template <class T> using FrameArgument = decltype(frameArgument(&T::prepare));

        constexpr std::span<const CpuValueType> inputTypes(std::type_identity<CpuCallFrame>) noexcept { return {}; }

        template <CpuFrameInput... T>
        constexpr std::span<const CpuValueType> inputTypes(std::type_identity<CpuFrameInputs<T...>>) noexcept
        {
            return cpu_value_types<T...>;
        }

        constexpr std::span<const CpuResultType> outputTypes(std::type_identity<RenderResult<void>>) noexcept
        {
            return {};
        }

        template <CpuPreparationValue... T>
        constexpr std::span<const CpuResultType>
        outputTypes(std::type_identity<RenderResult<std::tuple<T...>>>) noexcept
        {
            return cpu_result_types<T...>;
        }

        template <class T, class... Args>
        concept PreparesFrame = requires(
            const typename T::LinkedData& inputs, typename FrameArgument<T>::type& frame, Args... args
        )
        {
            inputTypes(FrameArgument<T>{});
            { T::prepare(inputs, frame, args...) } noexcept;
            outputTypes(std::type_identity<decltype(T::prepare(inputs, frame, args...))>{});
        };

        template <class T, class... Args>
        concept FrameAlgorithm = (!HasDrawMapping<T> && PreparesFrame<T, Args...>) ||
            (HasDrawMapping<T> && PreparesFrame<T, Args..., const typename T::DrawMapping*>);

        template <class T>
        concept OwnsWorkspace = HasWorkspace<T> && std::is_object_v<typename T::Workspace> &&
            std::same_as<typename T::Workspace, std::remove_cvref_t<typename T::Workspace>> &&
            std::is_nothrow_destructible_v<typename T::Workspace>;

        template <class T>
        using CreateWorkspace = RenderResult<std::unique_ptr<typename T::Workspace>> (*)(
            const typename T::LinkedData&, const TargetDesc&
        ) noexcept;

        template <class Data, class Config, std::size_t N>
        std::span<const RasterOperands, N> rasterArguments(
            RenderResult<Data> (*)(const Config&, std::span<const RasterOperands, N>) noexcept
        );

        template <class T> using RasterArguments = decltype(rasterArguments(&T::resolve));

        template <class T>
        concept NeedsCpuBinding = requires(const typename T::Configuration& config, RasterArguments<T> streams)
        {
            { T::resolve(config, streams) } noexcept -> std::same_as<RenderResult<typename T::LinkedData>>;
        };
    }

    template <class T>
    concept PreparationKernel = std::is_object_v<typename T::LinkedData> &&
        std::same_as<typename T::LinkedData, std::remove_cvref_t<typename T::LinkedData>> &&
        ((!extension_detail::HasWorkspace<T> && extension_detail::FrameAlgorithm<T>) ||
         (extension_detail::OwnsWorkspace<T> && extension_detail::FrameAlgorithm<T, typename T::Workspace&>)) &&
        (extension_detail::NeedsCpuBinding<T> ||
         (std::same_as<typename T::Configuration, typename T::LinkedData> && !requires { &T::resolve; })) &&
        requires(typename T::Configuration& config, typename T::LinkedData& linked)
        {
            { ConfigValue::make(std::move(config)) } noexcept -> std::same_as<RenderResult<ConfigValue>>;
            { ConfigValue::make(std::move(linked)) } noexcept -> std::same_as<RenderResult<ConfigValue>>;
        };

    namespace extension_detail
    {
        template <class T, auto Create>
        concept CpuWorkspaceFactory =
            (std::same_as<decltype(Create), std::nullptr_t> &&
             (!HasWorkspace<T> || std::default_initializable<typename T::Workspace>)) ||
            (OwnsWorkspace<T> && std::same_as<decltype(Create), CreateWorkspace<T>> && Create != nullptr);

        using BindCpuCall = RenderResult<PreparationCall> (*)(ConfigValue, std::span<const RasterOperands>) noexcept;

        template <class T, CreateWorkspace<T> Create>
        RenderResult<AlgorithmWorkspace> createWorkspace(const void* inputs, const TargetDesc& target) noexcept
        {
            using State = typename T::Workspace;
            std::unique_ptr<State> state;
            if constexpr (Create != nullptr)
            {
                auto created = Create(*static_cast<const typename T::LinkedData*>(inputs), target);
                if (!created) return cxx::unexpected(created.error());
                if (!*created) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                state = std::move(*created);
            }
            else state = std::make_unique<State>();
            return AlgorithmWorkspace{state.release(), [](void* value) noexcept { delete static_cast<State*>(value); }};
        }

        template <class T>
        RenderResult<void>
        prepareCpuCall(const void* data, CpuCallFrame& frame, void* workspace, const void* map) noexcept
        {
            const auto& inputs = *static_cast<const typename T::LinkedData*>(data);
            decltype(auto) selected = frame.arguments(FrameArgument<T>{});
            const auto run = [&](auto&... state) noexcept
            {
                if constexpr (extension_detail::HasDrawMapping<T>)
                    return T::prepare(inputs, selected, state..., static_cast<const typename T::DrawMapping*>(map));
                else return T::prepare(inputs, selected, state...);
            };
            auto result = [&]() noexcept
            {
                if constexpr (HasWorkspace<T>) return run(*static_cast<typename T::Workspace*>(workspace));
                else return run();
            }();
            if (!result) return cxx::unexpected(result.error());
            if constexpr (!std::same_as<decltype(result), RenderResult<void>>)
            {
                std::apply([&frame](const auto&... values) noexcept
                {
                    std::uint32_t ordinal = 0;
                    (frame.storeOutput(ordinal++, std::as_bytes(std::span{std::addressof(values), 1})), ...);
                }, *result);
            }
            return {};
        }

        template <class T, auto Create>
        RenderResult<PreparationCall>
        bindCpuCall(ConfigValue configuration, std::span<const RasterOperands> streams) noexcept
        {
            using Result = decltype(callResult(&T::prepare));
            static constexpr PreparationMethods methods{&prepareCpuCall<T>, []() constexpr -> decltype(PreparationMethods::create_workspace)
            {
                if constexpr (HasWorkspace<T>) return &createWorkspace<T, Create>;
                else return nullptr;
            }(), []() constexpr
            {
                if constexpr (extension_detail::HasDrawMapping<T>)
                    return cxx::make_basic_type_info<typename T::DrawMapping>();
                else return cxx::make_basic_type_info<void>();
            }(), inputTypes(FrameArgument<T>{}), outputTypes(Result{})};
            if constexpr (NeedsCpuBinding<T>)
            {
                using Sources = RasterArguments<T>;
                if constexpr (Sources::extent != std::dynamic_extent)
                    if (streams.size() != Sources::extent) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                const auto& config = *static_cast<const typename T::Configuration*>(configuration.data());
                auto linked = T::resolve(config, Sources{streams});
                if (!linked) return cxx::unexpected(linked.error());
                auto inputs = ConfigValue::make(std::move(*linked));
                if (!inputs) return cxx::unexpected(inputs.error());
                return PreparationCall{std::move(*inputs), &methods};
            }
            else
            {
                if (!streams.empty()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                return PreparationCall{std::move(configuration), &methods};
            }
        }
    }

    struct CpuCallDependencies final
    {
        std::vector<GraphCpuDataId> inputs;
        std::vector<GraphCpuDataId> outputs;
        // The result tuple supplies ordinary outputs first, then one complete block per listed pass.
        std::vector<GraphPassId> inline_blocks;
        // A live preparation retains all of its GPU passes. Inline block passes are included automatically.
        std::vector<GraphPassId> passes{};
        // Each list is addressed in declaration order within this call.
        std::vector<GraphResourceId> uploads{}, frame_sizes{};
        std::vector<GraphStorageSnapshotId> storage_inputs{};
        // Ordered, exclusive draw stream outputs. Each also retains its raster pass.
        std::vector<GraphPassId> draw_streams{};
    };
} // namespace lux::render
