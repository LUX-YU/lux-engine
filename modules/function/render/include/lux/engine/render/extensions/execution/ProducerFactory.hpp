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
        concept WorkspaceFactory = requires(const typename T::LinkedData& linked, InstanceInstantiationContext& context)
        {
            { T::createWorkspace(linked, context) } noexcept -> std::same_as<WorkspaceResult<T>>;
        };

        template <class T>
        concept RasterRevision = requires(const typename T::LinkedData& linked, const typename T::Workspace& workspace)
        {
            { T::rasterProgramRevision(linked, workspace) } noexcept -> std::same_as<std::uint64_t>;
        };

        template <class T>
        concept RasterSelection = requires(
            const typename T::LinkedData& linked, RasterProgramSelection& selection, typename T::Workspace& workspace
        )
        {
            { T::selectRasterPrograms(linked, selection, workspace) } noexcept -> std::same_as<RenderResult<void>>;
        };

        template <class T>
        concept RasterSettlement = requires(const typename T::LinkedData& linked, typename T::Workspace& workspace)
        {
            { T::settleRasterPrograms(linked, true, workspace) } noexcept -> std::same_as<void>;
        };
    }

    template <class T>
    concept PreparationKernel = extension_detail::ProducerValue<typename T::LinkedData> &&
        extension_detail::ProducerValue<typename T::Workspace> && std::is_nothrow_destructible_v<typename T::Workspace> &&
        (extension_detail::WorkspaceFactory<T> ||
            (!requires { &T::createWorkspace; } && std::default_initializable<typename T::Workspace>)) &&
        (extension_detail::RasterRevision<T> || !requires { &T::rasterProgramRevision; }) &&
        (extension_detail::RasterSelection<T> || !requires { &T::selectRasterPrograms; }) &&
        (extension_detail::RasterSettlement<T> || !requires { &T::settleRasterPrograms; }) &&
        requires(
            const typename T::Configuration& config, typename T::LinkedData& linked, typename T::Workspace& workspace,
            InstanceLinkContext& link, FramePreparation& frame
        )
        {
            { T::resolve(config, link) } noexcept -> std::same_as<RenderResult<typename T::LinkedData>>;
            { ConfigValue::make(std::move(linked)) } noexcept -> std::same_as<RenderResult<ConfigValue>>;
            { T::prepare(std::as_const(linked), frame, workspace) } noexcept -> std::same_as<RenderResult<void>>;
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
                return LinkedProducer{std::move(*inputs), &instantiate<T>};
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
        instantiate(const void* data, InstanceInstantiationContext& context) noexcept
        {
            using Linked = typename T::LinkedData;
            using Workspace = typename T::Workspace;
            using Result = extension_detail::WorkspaceResult<T>;
            auto owned = [&]() noexcept -> Result
            {
                if constexpr (extension_detail::WorkspaceFactory<T>)
                    return T::createWorkspace(*static_cast<const Linked*>(data), context);
                else return std::make_unique<Workspace>();
            }();
            if (!owned) return cxx::unexpected(owned.error());
            if (!*owned) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});

            static constexpr ProducerWorkspace::Ops ops{&prepare<T>, &revision<T>, &select<T>, &settle<T>};
            ProducerWorkspace::Storage storage{owned->release(), [](void* value) noexcept {
                delete static_cast<Workspace*>(value);
            }};
            return ProducerWorkspace{data, ops, std::move(storage)};
        }

        template <class T>
        static RenderResult<void> prepare(const void* linked, FramePreparation& frame, void* workspace) noexcept
        {
            return T::prepare(
                *static_cast<const typename T::LinkedData*>(linked), frame,
                *static_cast<typename T::Workspace*>(workspace)
            );
        }

        template <class T>
        static std::uint64_t revision(const void* linked, const void* workspace) noexcept
        {
            if constexpr (extension_detail::RasterRevision<T>)
                return T::rasterProgramRevision(
                    *static_cast<const typename T::LinkedData*>(linked),
                    *static_cast<const typename T::Workspace*>(workspace)
                );
            else return 0;
        }

        template <class T>
        static RenderResult<void> select(const void* linked, RasterProgramSelection& selection, void* workspace) noexcept
        {
            if constexpr (extension_detail::RasterSelection<T>)
                return T::selectRasterPrograms(
                    *static_cast<const typename T::LinkedData*>(linked), selection,
                    *static_cast<typename T::Workspace*>(workspace)
                );
            else return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }

        template <class T>
        static void settle(const void* linked, bool adopted, void* workspace) noexcept
        {
            if constexpr (extension_detail::RasterSettlement<T>)
                T::settleRasterPrograms(
                    *static_cast<const typename T::LinkedData*>(linked), adopted,
                    *static_cast<typename T::Workspace*>(workspace)
                );
        }

        using Link = RenderResult<LinkedProducer> (*)(const ConfigValue&, InstanceLinkContext&) noexcept;
        explicit constexpr ProducerFactory(Link link) noexcept : link_(link) {}

        Link link_;
    };
}
