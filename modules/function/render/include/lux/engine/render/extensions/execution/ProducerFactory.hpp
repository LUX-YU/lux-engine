#pragma once

#include <lux/engine/render/extensions/execution/InstanceLink.hpp>
#include <lux/engine/render/extensions/execution/LinkedProducer.hpp>

namespace lux::render
{
    namespace extension_detail
    {
        template <class T>
        concept ProducerValue = std::is_object_v<T> && std::same_as<T, std::remove_cvref_t<T>>;

        template <class T>
        using WorkspaceResult = RenderResult<std::unique_ptr<typename T::Workspace>>;

        template <class T>
        concept WorkspaceFactory = requires(const typename T::LinkedData& linked, const TargetDesc& target)
        {
            { T::createWorkspace(linked, target) } noexcept -> std::same_as<WorkspaceResult<T>>;
        };

        template <class T>
        concept FrameKernel = requires(const typename T::LinkedData& linked, FramePreparation& frame,
                                       typename T::Workspace& workspace)
        {
            { T::prepare(linked, frame, workspace) } noexcept -> std::same_as<RenderResult<void>>;
        };

        template <class T>
        concept MappedFrameKernel = requires(const typename T::LinkedData& linked, FramePreparation& frame,
                                             typename T::Workspace& workspace, const typename T::DrawMapping* map)
        {
            { T::prepare(linked, frame, workspace, map) } noexcept -> std::same_as<RenderResult<void>>;
        };
    }

    template <class T>
    concept PreparationKernel = extension_detail::ProducerValue<typename T::LinkedData> &&
        extension_detail::ProducerValue<typename T::Workspace> && std::is_nothrow_destructible_v<typename T::Workspace> &&
        (extension_detail::WorkspaceFactory<T> ||
            (!requires { &T::createWorkspace; } && std::default_initializable<typename T::Workspace>)) &&
        (extension_detail::MappedFrameKernel<T> ||
            (!requires { typename T::DrawMapping; } && extension_detail::FrameKernel<T>)) &&
        requires(const typename T::Configuration& config, typename T::LinkedData& linked, InstanceLinkContext& link)
        {
            { T::resolve(config, link) } noexcept -> std::same_as<RenderResult<typename T::LinkedData>>;
            { ConfigValue::make(std::move(linked)) } noexcept -> std::same_as<RenderResult<ConfigValue>>;
        };

    class ProducerFactory final
    {
    public:
        template <PreparationKernel T>
        [[nodiscard]] static constexpr ProducerFactory make() noexcept
        {
            return ProducerFactory{[](const ConfigValue& config, InstanceLinkContext& link) noexcept
                -> RenderResult<LinkedProducer>
            {
                const auto* typed = config.borrow<typename T::Configuration>();
                if (!typed) return cxx::unexpected(RenderError{kInvalidConfig});
                auto resolved = T::resolve(*typed, link);
                if (!resolved) return cxx::unexpected(resolved.error());
                auto inputs = ConfigValue::make(std::move(*resolved));
                if (!inputs) return cxx::unexpected(inputs.error());
                const cxx::basic_type_info* mapping = nullptr;
                if constexpr (extension_detail::MappedFrameKernel<T>)
                {
                    static constexpr auto type = cxx::make_basic_type_info<typename T::DrawMapping>();
                    mapping = &type;
                }
                return LinkedProducer{std::move(*inputs), &instantiate<T>, mapping};
            }};
        }

        [[nodiscard]] RenderResult<LinkedProducer>
        link(const ConfigValue& config, InstanceLinkContext& context) const noexcept
        {
            return link_(config, context);
        }

    private:
        template <PreparationKernel T>
        [[nodiscard]] static RenderResult<ProducerWorkspace>
        instantiate(const void* data, const TargetDesc& target) noexcept
        {
            using Linked = typename T::LinkedData;
            using Workspace = typename T::Workspace;
            using Result = extension_detail::WorkspaceResult<T>;
            auto owned = [&]() noexcept -> Result
            {
                if constexpr (extension_detail::WorkspaceFactory<T>)
                    return T::createWorkspace(*static_cast<const Linked*>(data), target);
                else return std::make_unique<Workspace>();
            }();
            if (!owned) return cxx::unexpected(owned.error());
            if (!*owned) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});

            ProducerWorkspace::Storage storage{owned->release(), [](void* value) noexcept {
                delete static_cast<Workspace*>(value);
            }};
            return ProducerWorkspace{data, &prepare<T>, std::move(storage)};
        }

        template <class T>
        static RenderResult<void>
        prepare(const void* linked, FramePreparation& frame, void* workspace, const void* mapping) noexcept
        {
            const auto& inputs = *static_cast<const typename T::LinkedData*>(linked);
            auto& state = *static_cast<typename T::Workspace*>(workspace);
            if constexpr (extension_detail::MappedFrameKernel<T>)
                return T::prepare(inputs, frame, state, static_cast<const typename T::DrawMapping*>(mapping));
            else return T::prepare(inputs, frame, state);
        }

        using Link = RenderResult<LinkedProducer> (*)(const ConfigValue&, InstanceLinkContext&) noexcept;
        explicit constexpr ProducerFactory(Link link) noexcept : link_(link) {}

        Link link_;
    };
}
