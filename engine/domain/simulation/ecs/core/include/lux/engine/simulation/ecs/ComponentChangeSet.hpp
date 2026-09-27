#pragma once

#include <lux/engine/simulation/ecs/Registry.hpp>

#include <entt/entity/registry.hpp>

#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    template <class... Components> struct TComponentList final
    {};

    template <class Component, class... Required, class... Excluded>
    [[nodiscard]] auto componentView(Registry& registry, TComponentList<Required...>, TComponentList<Excluded...>)
    {
        return registry.template view<Component, Required...>(entt::exclude<Excluded...>);
    }

    template <class Component, class Required, class Excluded> class TExtractionChangeSet;

    template <class Component, class... Required, class... Excluded>
    class TExtractionChangeSet<Component, TComponentList<Required...>, TComponentList<Excluded...>> final
    {
        using Storage =
            std::remove_reference_t<decltype(std::declval<Registry&>().template storage<entt::reactive>(entt::id_type{})
            )>;

    public:
        TExtractionChangeSet() = default;
        ~TExtractionChangeSet()
        {
            detach();
        }

        TExtractionChangeSet(const TExtractionChangeSet&) = delete;
        TExtractionChangeSet& operator=(const TExtractionChangeSet&) = delete;
        TExtractionChangeSet(TExtractionChangeSet&&) = delete;
        TExtractionChangeSet& operator=(TExtractionChangeSet&&) = delete;

        template <class Declare> void attach(Registry& registry, entt::id_type id, Declare&& declare)
        {
            detach();
            registry_ = &registry;
            storage_ = &registry.template storage<entt::reactive>(id);
            std::forward<Declare>(declare)(*storage_);

            componentView<Component>(registry, TComponentList<Required...>{}, TComponentList<Excluded...>{})
                .each([this](Entity entity, auto&&...) { storage_->emplace(entity); });
        }

        void detach() noexcept
        {
            if (storage_ != nullptr)
            {
                storage_->reset();
                storage_->clear();
            }
            registry_ = nullptr;
            storage_ = nullptr;
        }

        [[nodiscard]] bool attached() const noexcept
        {
            return registry_ != nullptr;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return storage_ == nullptr || storage_->empty();
        }

        [[nodiscard]] auto view() const
        {
            return storage_->template view<Component, Required...>(entt::exclude<Excluded...>);
        }

        void clear() noexcept
        {
            if (storage_ != nullptr)
            {
                storage_->clear();
            }
        }

        void mark(Entity entity)
        {
            if (registry_ != nullptr && registry_->valid(entity))
            {
                storage_->emplace(entity);
            }
        }

        void markAll()
        {
            if (registry_ == nullptr)
            {
                return;
            }
            componentView<Component>(*registry_, TComponentList<Required...>{}, TComponentList<Excluded...>{})
                .each([this](Entity entity, auto&&...) { storage_->emplace(entity); });
        }

    private:
        Registry* registry_{};
        Storage* storage_{};
    };

    template <class Component, class Required, class Excluded> class TComponentSetLeaveObserver;

    template <class Component, class... Required, class... Excluded>
    class TComponentSetLeaveObserver<Component, TComponentList<Required...>, TComponentList<Excluded...>> final
    {
    public:
        using Callback = void (*)(void*, Entity) noexcept;

        TComponentSetLeaveObserver() = default;
        ~TComponentSetLeaveObserver()
        {
            detach();
        }

        TComponentSetLeaveObserver(const TComponentSetLeaveObserver&) = delete;
        TComponentSetLeaveObserver& operator=(const TComponentSetLeaveObserver&) = delete;
        TComponentSetLeaveObserver(TComponentSetLeaveObserver&&) = delete;
        TComponentSetLeaveObserver& operator=(TComponentSetLeaveObserver&&) = delete;

        void attach(Registry& registry, void* user, Callback callback) noexcept
        {
            detach();
            registry_ = &registry;
            user_ = user;
            callback_ = callback;

            registry.template on_destroy<Component>().template connect<&TComponentSetLeaveObserver::onLeft>(*this);
            (registry.template on_destroy<Required>().template connect<&TComponentSetLeaveObserver::onLeft>(*this),
             ...);
            (registry.template on_construct<Excluded>().template connect<&TComponentSetLeaveObserver::onLeft>(*this),
             ...);
        }

        void detach() noexcept
        {
            if (registry_ == nullptr)
            {
                return;
            }
            registry_->template on_destroy<Component>().template disconnect<&TComponentSetLeaveObserver::onLeft>(*this);
            (registry_->template on_destroy<Required>().template disconnect<&TComponentSetLeaveObserver::onLeft>(*this),
             ...);
            (registry_->template on_construct<Excluded>().template disconnect<&TComponentSetLeaveObserver::onLeft>(*this
             ),
             ...);
            registry_ = nullptr;
            user_ = nullptr;
            callback_ = nullptr;
        }

        [[nodiscard]] bool attached() const noexcept
        {
            return registry_ != nullptr;
        }

    private:
        void onLeft(Registry&, Entity entity) noexcept
        {
            if (callback_ != nullptr)
            {
                callback_(user_, entity);
            }
        }

        Registry* registry_{};
        void* user_{};
        Callback callback_{};
    };
} // namespace lux::simulation::ecs
