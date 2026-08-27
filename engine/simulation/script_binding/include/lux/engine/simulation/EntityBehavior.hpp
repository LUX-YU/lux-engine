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
        [[nodiscard]] const ScriptInstanceHostContext& hostContext() const
            noexcept
        {
            return *host_;
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
