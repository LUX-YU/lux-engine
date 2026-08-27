#pragma once

#include <lux/engine/simulation/ScriptBindingSession.hpp>

#include <concepts>

namespace lux::simulation
{
    /** Opt-in base for one long-lived C++ object owned by an entity mount. */
    class EntityBehavior
    {
      public:
        EntityBehavior() noexcept = default;
        EntityBehavior(const EntityBehavior&) = delete;
        EntityBehavior& operator=(const EntityBehavior&) = delete;
        EntityBehavior(EntityBehavior&&) = delete;
        EntityBehavior& operator=(EntityBehavior&&) = delete;
        ~EntityBehavior() noexcept = default;

      protected:
        [[nodiscard]] bool attached() const noexcept
        {
            return host_ != nullptr && host_->attached();
        }

        [[nodiscard]] const ScriptInstanceHostContext& hostContext() const
            noexcept
        {
            if (!attached())
                scriptBindingContractFailure();
            return *host_;
        }

        [[nodiscard]] ecs::Entity self() const noexcept
        {
            return hostContext().self();
        }

        [[nodiscard]] bool hasComponent(std::uint64_t component_type) const
            noexcept
        {
            return hostContext().read(component_type) != nullptr;
        }

        [[nodiscard]] const void* readComponent(
            std::uint64_t component_type
        ) const noexcept
        {
            return hostContext().read(component_type);
        }

        [[nodiscard]] bool patchComponent(
            std::uint64_t component_type,
            const void* value
        ) const noexcept
        {
            return hostContext().patch(component_type, value);
        }

        [[nodiscard]] bool command(
            EScriptHostCommand command_value,
            std::uint64_t component_type = 0U,
            const void* value = nullptr
        ) const noexcept
        {
            return hostContext().command(
                command_value,
                component_type,
                value
            );
        }

      private:
        ScriptInstanceHostContext* host_{};

        struct Access final
        {
            static void attach(
                EntityBehavior& behavior,
                ScriptInstanceHostContext& host
            ) noexcept
            {
                behavior.host_ = &host;
            }
        };

        template <class Type>
        friend void attachEntityBehavior(
            void*,
            ScriptInstanceHostContext&
        ) noexcept;
    };

    template <class Type>
    void attachEntityBehavior(
        void* object,
        ScriptInstanceHostContext& host
    ) noexcept
    {
        static_assert(std::derived_from<Type, EntityBehavior>);
        EntityBehavior::Access::attach(*static_cast<Type*>(object), host);
    }
}
