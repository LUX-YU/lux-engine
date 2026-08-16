#pragma once
/**
 * @file SceneAsyncContext.hpp
 * @brief Domain-blind access to one Scene's asynchronous work lifetime.
 */

#include <lux/engine/runtime/scene/visibility.h>

namespace lux::exec
{
    class AsyncRuntime;
    class AsyncScope;
}

namespace lux::runtime
{
    /// Borrowed, scene-scoped execution seam published through SceneServices.
    ///
    /// Domain leaves retain their typed operation clients separately.  This
    /// context supplies the domain-blind runtime scheduler and the scope which
    /// SceneRuntime closes after every installed domain has stopped accepting
    /// work. It is
    /// deliberately free of Render, Physics, Navigation and partition types.
    class LUX_RUNTIME_SCENE_PUBLIC SceneAsyncContext final
    {
    public:
        SceneAsyncContext(lux::exec::AsyncRuntime& runtime,
                          lux::exec::AsyncScope& scope) noexcept
            : runtime_(&runtime), scope_(&scope)
        {}

        [[nodiscard]] lux::exec::AsyncRuntime& runtime() const noexcept
        {
            return *runtime_;
        }

        [[nodiscard]] lux::exec::AsyncScope& scope() const noexcept
        {
            return *scope_;
        }

      private:
        lux::exec::AsyncRuntime* runtime_{nullptr};
        lux::exec::AsyncScope* scope_{nullptr};
    };
} // namespace lux::runtime
