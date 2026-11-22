#pragma once

#include <lux/engine/render/extensions/execution/InstanceLink.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <vector>

namespace lux::render
{
    namespace extension_detail
    {
        using AlgorithmWorkspace = std::unique_ptr<void, void (*)(void*) noexcept>;

        struct PreparationMethods final
        {
            RenderResult<void> (*prepare)(const void*, FramePreparation&, void*, const void*) noexcept;
            RenderResult<AlgorithmWorkspace> (*create_workspace)(const void*, const TargetDesc&) noexcept;
            cxx::basic_type_info mapping_type;
        };

        struct PreparationCall final
        {
            ConfigValue inputs;
            const PreparationMethods* methods;
        };

        template <class T> concept HasWorkspace = requires { typename T::Workspace; };
        template <class T> concept HasDrawMapping = requires { typename T::DrawMapping; };

        template <class T, class... Args>
        concept PreparesFrame = requires(const typename T::LinkedData& inputs, FramePreparation& frame, Args... args)
        {
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
    }

    template <class T>
    concept PreparationKernel = std::is_object_v<typename T::LinkedData> &&
        std::same_as<typename T::LinkedData, std::remove_cvref_t<typename T::LinkedData>> &&
        ((!extension_detail::HasWorkspace<T> && extension_detail::FrameAlgorithm<T>) ||
         (extension_detail::OwnsWorkspace<T> && extension_detail::FrameAlgorithm<T, typename T::Workspace&>)) &&
        requires(const typename T::Configuration& config, typename T::LinkedData& linked, InstanceLinkContext& link)
        {
            { T::resolve(config, link) } noexcept -> std::same_as<RenderResult<typename T::LinkedData>>;
            { ConfigValue::make(std::move(linked)) } noexcept -> std::same_as<RenderResult<ConfigValue>>;
        };

    class PreparationAlgorithm final
    {
        using Workspace = extension_detail::AlgorithmWorkspace;
        using Invocation = extension_detail::PreparationCall;
        using Methods = extension_detail::PreparationMethods;

    public:
        template <PreparationKernel T> requires (!extension_detail::HasWorkspace<T>)
        [[nodiscard]] static constexpr PreparationAlgorithm stateless() noexcept
        {
            return PreparationAlgorithm{&linkTyped<T, nullptr>};
        }

        template <PreparationKernel T, extension_detail::CreateWorkspace<T> Create = nullptr>
            requires (extension_detail::OwnsWorkspace<T> &&
                      (Create != nullptr || std::default_initializable<typename T::Workspace>))
        [[nodiscard]] static constexpr PreparationAlgorithm withWorkspace() noexcept
        {
            return PreparationAlgorithm{&linkTyped<T, &createWorkspace<T, Create>>};
        }

        [[nodiscard]] RenderResult<Invocation>
        link(const ConfigValue& config, InstanceLinkContext& context) const noexcept
        {
            return link_(config, context);
        }

    private:
        template <class T, decltype(Methods::create_workspace) Create>
        static RenderResult<Invocation> linkTyped(const ConfigValue& config, InstanceLinkContext& context) noexcept
        {
            const auto* typed = config.borrow<typename T::Configuration>();
            if (!typed) return cxx::unexpected(RenderError{kInvalidConfig});
            auto linked = T::resolve(*typed, context);
            if (!linked) return cxx::unexpected(linked.error());
            auto inputs = ConfigValue::make(std::move(*linked));
            if (!inputs) return cxx::unexpected(inputs.error());
            static constexpr Methods methods{&prepare<T>, Create, []() constexpr
            {
                if constexpr (extension_detail::HasDrawMapping<T>)
                    return cxx::make_basic_type_info<typename T::DrawMapping>();
                else return cxx::make_basic_type_info<void>();
            }()};
            return Invocation{std::move(*inputs), &methods};
        }

        template <class T, extension_detail::CreateWorkspace<T> Create>
        static RenderResult<Workspace> createWorkspace(const void* inputs, const TargetDesc& target) noexcept
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
            return Workspace{state.release(), [](void* value) noexcept { delete static_cast<State*>(value); }};
        }

        template <class T>
        static RenderResult<void>
        prepare(const void* data, FramePreparation& frame, void* workspace, const void* map) noexcept
        {
            const auto& inputs = *static_cast<const typename T::LinkedData*>(data);
            const auto run = [&](auto&... state) noexcept
            {
                if constexpr (extension_detail::HasDrawMapping<T>)
                    return T::prepare(inputs, frame, state..., static_cast<const typename T::DrawMapping*>(map));
                else return T::prepare(inputs, frame, state...);
            };
            if constexpr (extension_detail::HasWorkspace<T>)
                return run(*static_cast<typename T::Workspace*>(workspace));
            else return run();
        }

        using Link = RenderResult<Invocation> (*)(const ConfigValue&, InstanceLinkContext&) noexcept;
        explicit constexpr PreparationAlgorithm(Link link) noexcept : link_(link) {}
        Link link_;
    };

    struct GraphPreparation final
    {
        PreparationAlgorithm algorithm;
        ConfigValue configuration;
        std::vector<GraphCpuDataId> inputs;
        std::vector<GraphCpuDataId> outputs;
        std::vector<GraphPassId> inline_blocks;
        // A live preparation retains all of its GPU passes. Inline block passes are included automatically.
        std::vector<GraphPassId> passes{};
        // Each listed buffer is published once, through its linked writer or sizer.
        std::vector<GraphResourceId> gpu_buffers{};
        std::vector<GraphStorageSnapshotId> storage_inputs{};
        // Subset of storage_inputs that drives the runtime Raster program selection.
        std::vector<GraphStorageSnapshotId> program_storage_inputs{};
        RasterProgramBuilder program_builder{};
    };
} // namespace lux::render
