#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/systems/render/RenderDataCollector.hpp>
#include <lux/engine/systems/render/RenderReaction.hpp>
#include <lux/engine/systems/render/visibility.h>

#include <concepts>
#include <functional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace lux::engine::render
{
    class RenderSystem;

    struct ReactionMeta final
    {
        using Result = lux::render::RenderResult<std::unique_ptr<RenderReaction>>;
        cxx::type_info type;
        std::function<Result(RenderSystem&, lux::render::CapabilityImplementationId)> construct;
    };

    struct CollectorMeta final
    {
        using Result = lux::render::RenderResult<std::unique_ptr<RenderDataCollector>>;
        cxx::type_info type;
        std::function<Result(RenderSystem&, lux::render::FrameDataDestination)> construct;
    };

    struct RenderCapabilityMeta final
    {
        cxx::type_info capability;
        std::vector<ReactionMeta> reactions;
        std::vector<CollectorMeta> collectors;
    };

    // Cold factories borrow stable professional services through the last behavior's retirement.
    // Registration never constructs a behavior. Factories may subscribe and record existing components,
    // but must not start tasks, send requests or dispatch callbacks before construction succeeds.
    // Captured services must not move after registration; the registry never owns them.
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC RenderCapabilityMetaRegistry final
    {
    public:
        using Result = lux::render::RenderResult<void>;
        using SceneReaction = std::pair<cxx::type_info, std::function<ReactionMeta::Result(RenderSystem&)>>;

        // Cold registration for a capability needing no Engine behavior. Repeated registration is idempotent;
        // adding Reactions/Collectors later uses the same entry. A hash collision is rejected.
        [[nodiscard]] Result addCapability(cxx::type_info capability) noexcept;

        // An already prepared factory may own immutable command IDs while borrowing its services.
        // It follows exactly the same synchronous construction contract as the dependency overload.
        template <class T, class Factory>
            requires std::derived_from<T, RenderReaction> &&
                requires(Factory& construct, RenderSystem& system, lux::render::CapabilityImplementationId implementation)
                {
                    { std::invoke(construct, system, implementation) } noexcept ->
                        std::same_as<lux::render::RenderResult<std::unique_ptr<T>>>;
                }
        [[nodiscard]] Result addReaction(Factory construct) noexcept
        {
            if constexpr (std::is_pointer_v<Factory>)
            {
                if (!construct) return cxx::unexpected(lux::render::RenderError{lux::render::kCapabilityDeclaration});
            }
            return add(T::capability, ReactionMeta{
                cxx::make_basic_type_info<T>(),
                [construct = std::move(construct)](
                    RenderSystem& system, lux::render::CapabilityImplementationId implementation
                ) mutable noexcept -> ReactionMeta::Result
                {
                    auto created = std::invoke(construct, system, implementation);
                    if (!created) return cxx::unexpected(created.error());
                    return std::unique_ptr<RenderReaction>{std::move(*created)};
                }
            });
        }

        template <class T, class... Dependencies>
            requires std::derived_from<T, RenderReaction> &&
                requires(RenderSystem& system, lux::render::CapabilityImplementationId implementation,
                         Dependencies&... dependencies) { T::create(system, implementation, dependencies...); }
        [[nodiscard]] Result addReaction(Dependencies&... dependencies) noexcept
        {
            return addReaction<T>(
                [&dependencies...](RenderSystem& system, lux::render::CapabilityImplementationId implementation)
                    noexcept(noexcept(T::create(system, implementation, dependencies...)))
                {
                    return T::create(system, implementation, dependencies...);
                }
            );
        }

        template <class T, class... Dependencies> requires std::derived_from<T, RenderReaction>
        [[nodiscard]] Result addSceneReaction(Dependencies&... dependencies) noexcept
        {
            static_assert(noexcept(T::create(std::declval<RenderSystem&>(), dependencies...)));
            return addSceneReaction(SceneReaction{
                cxx::make_basic_type_info<T>(),
                [&dependencies...](RenderSystem& system) noexcept -> ReactionMeta::Result
                {
                    auto created = T::create(system, dependencies...);
                    if (!created)
                    {
                        return cxx::unexpected(created.error());
                    }
                    return std::unique_ptr<RenderReaction>{std::move(*created)};
                }
            });
        }

        [[nodiscard]] std::span<const SceneReaction> sceneReactions() const noexcept { return scene_reactions_; }

        template <class T, class... Dependencies> requires std::derived_from<T, RenderDataCollector>
        [[nodiscard]] Result addCollector(Dependencies&... dependencies) noexcept
        {
            static_assert(noexcept(T::create(std::declval<RenderSystem&>(),
                std::declval<lux::render::FrameDataDestination>(), dependencies...)));
            return add(T::capability, CollectorMeta{
                cxx::make_basic_type_info<T>(),
                [&dependencies...](RenderSystem& system, lux::render::FrameDataDestination destination)
                    noexcept -> CollectorMeta::Result
                {
                    auto created = T::create(system, destination, dependencies...);
                    if (!created)
                    {
                        return cxx::unexpected(created.error());
                    }
                    return std::unique_ptr<RenderDataCollector>{std::move(*created)};
                }
            });
        }

        [[nodiscard]] const RenderCapabilityMeta* find(cxx::type_info capability) const noexcept;

    private:
        [[nodiscard]] Result addSceneReaction(SceneReaction metadata) noexcept;
        [[nodiscard]] Result add(cxx::type_info capability, ReactionMeta metadata) noexcept;
        [[nodiscard]] Result add(cxx::type_info capability, CollectorMeta metadata) noexcept;
        std::unordered_map<std::uint64_t, RenderCapabilityMeta> capabilities_;
        std::vector<SceneReaction> scene_reactions_;
    };
}
