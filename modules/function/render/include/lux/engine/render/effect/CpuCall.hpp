#pragma once

#include <lux/engine/render/extensions/execution/InstanceLink.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <vector>

namespace lux::render
{
    namespace extension_detail
    {
        using AlgorithmWorkspace = std::unique_ptr<void, void (*)(void*) noexcept>;

        struct PreparationMethods final
        {
            RenderResult<void> (*prepare)(const void*, CpuCallFrame&, void*, const void*) noexcept;
            RenderResult<AlgorithmWorkspace> (*create_workspace)(const void*, const TargetDesc&) noexcept;
            cxx::basic_type_info mapping_type;
            std::span<const cxx::basic_type_info> snapshots;
        };

        struct PreparationCall final
        {
            ConfigValue inputs;
            const PreparationMethods* methods;
        };

        template <class T> concept HasWorkspace = requires { typename T::Workspace; };
        template <class T> concept HasDrawMapping = requires { typename T::DrawMapping; };

        template <class Linked, class Frame, class... Tail>
        std::type_identity<std::remove_cvref_t<Frame>>
        frameArgument(RenderResult<void> (*)(const Linked&, Frame, Tail...) noexcept);

        template <class T> using FrameArgument = decltype(frameArgument(&T::prepare));

        constexpr std::span<const cxx::basic_type_info> frameSnapshotTypes(std::type_identity<CpuCallFrame>) noexcept
        {
            return {};
        }

        template <class... T>
        constexpr std::span<const cxx::basic_type_info>
        frameSnapshotTypes(std::type_identity<CpuFrameInputs<T...>>) noexcept
        {
            return snapshot_types<T...>;
        }

        template <class T, class... Args>
        concept PreparesFrame = requires(
            const typename T::LinkedData& inputs, typename FrameArgument<T>::type& frame, Args... args
        )
        {
            frameSnapshotTypes(FrameArgument<T>{});
            { T::prepare(inputs, frame, args...) } noexcept -> std::same_as<RenderResult<void>>;
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

        template <class T>
        concept NeedsCpuBinding = requires(const typename T::Configuration& config, InstanceLinkContext& link)
        {
            { T::resolve(config, link) } noexcept -> std::same_as<RenderResult<typename T::LinkedData>>;
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

        using BindCpuCall = RenderResult<PreparationCall> (*)(ConfigValue, InstanceLinkContext&) noexcept;

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
            if constexpr (extension_detail::HasWorkspace<T>)
                return run(*static_cast<typename T::Workspace*>(workspace));
            else return run();
        }

        template <class T, auto Create>
        RenderResult<PreparationCall> bindCpuCall(ConfigValue configuration, InstanceLinkContext& context) noexcept
        {
            static constexpr PreparationMethods methods{&prepareCpuCall<T>, []() constexpr -> decltype(PreparationMethods::create_workspace)
            {
                if constexpr (HasWorkspace<T>) return &createWorkspace<T, Create>;
                else return nullptr;
            }(), []() constexpr
            {
                if constexpr (extension_detail::HasDrawMapping<T>)
                    return cxx::make_basic_type_info<typename T::DrawMapping>();
                else return cxx::make_basic_type_info<void>();
            }(), frameSnapshotTypes(FrameArgument<T>{})};
            if constexpr (NeedsCpuBinding<T>)
            {
                auto linked = T::resolve(*static_cast<const typename T::Configuration*>(configuration.data()), context);
                if (!linked) return cxx::unexpected(linked.error());
                auto inputs = ConfigValue::make(std::move(*linked));
                if (!inputs) return cxx::unexpected(inputs.error());
                return PreparationCall{std::move(*inputs), &methods};
            }
            else return PreparationCall{std::move(configuration), &methods};
        }
    }

    struct CpuCallDependencies final
    {
        std::vector<GraphCpuDataId> inputs;
        std::vector<GraphCpuDataId> outputs;
        std::vector<GraphPassId> inline_blocks;
        // A live preparation retains all of its GPU passes. Inline block passes are included automatically.
        std::vector<GraphPassId> passes{};
        // Each listed buffer is published once, through its linked writer or sizer.
        std::vector<GraphResourceId> gpu_buffers{};
        std::vector<GraphStorageSnapshotId> storage_inputs{};
    };
} // namespace lux::render
