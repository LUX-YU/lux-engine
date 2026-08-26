#pragma once

#include <lux/engine/task/visibility.h>

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::task
{
    /**
     * Move-only type-erased `void() noexcept` callable.
     *
     * Construction is cold/build-time. Invocation is one pointer load and one
     * indirect call. The callable is invoked through const, which keeps TaskGraph
     * structurally immutable and makes concurrent graph execution an explicit
     * callable-level thread-safety decision.
     */
    class LUX_CORE_TASK_PUBLIC TaskCallable final
    {
    public:
        TaskCallable() noexcept = default;
        ~TaskCallable() noexcept;

        TaskCallable(TaskCallable&& other) noexcept;
        TaskCallable& operator=(TaskCallable&& other) noexcept;

        TaskCallable(const TaskCallable&) = delete;
        TaskCallable& operator=(const TaskCallable&) = delete;

        template <class Fn>
            requires (
                !std::same_as<std::remove_cvref_t<Fn>, TaskCallable> &&
                std::is_move_constructible_v<std::remove_cvref_t<Fn>> &&
                std::is_nothrow_invocable_r_v<
                    void,
                    const std::remove_cvref_t<Fn>&
                >
            )
        explicit TaskCallable(Fn&& function)
        {
            using Function = std::remove_cvref_t<Fn>;
            auto object = std::make_unique<Function>(std::forward<Fn>(function));
            object_ = object.release();
            invoke_ = [](const void* value) noexcept
            {
                std::invoke(*static_cast<const Function*>(value));
            };
            destroy_ = [](void* value) noexcept
            {
                delete static_cast<Function*>(value);
            };
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return object_ != nullptr && invoke_ != nullptr && destroy_ != nullptr;
        }

        void operator()() const noexcept
        {
            invoke_(object_);
        }

        void reset() noexcept;

    private:
        void* object_{};
        void (*invoke_)(const void*) noexcept{};
        void (*destroy_)(void*) noexcept{};
    };
}
