#pragma once
#include "InputContext.hpp"
#include <vector>
#include <algorithm>
#include <cassert>

namespace lux::input
{
    /// A non-owning stack of InputContext pointers.
    /// Contexts are ordered by their priority() value (ascending).
    /// Within the same priority, insertion order is preserved.
    /// The last element (highest priority) is processed first.
    class InputContextStack
    {
    public:
        /// Insert a context in priority order (stable).
        /// Duplicate pushes of the same pointer are silently ignored.
        void push(InputContext* ctx)
        {
            assert(ctx && "InputContextStack: null context pushed");
            if (contains(ctx)) return;
            auto it = std::upper_bound(stack_.begin(), stack_.end(), ctx,
                [](const InputContext* a, const InputContext* b) {
                    return a->priority() < b->priority();
                });
            stack_.insert(it, ctx);
        }

        /// Check whether the stack already contains this context pointer.
        [[nodiscard]] bool contains(const InputContext* ctx) const noexcept
        {
            return std::find(stack_.begin(), stack_.end(), ctx) != stack_.end();
        }

        /// Remove the topmost context whose pointer equals ctx.
        bool pop(InputContext* ctx)
        {
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
                if (*it == ctx) {
                    stack_.erase(std::next(it).base());
                    return true;
                }
            }
            return false;
        }

        /// Remove and return the top-most context.
        InputContext* pop()
        {
            if (stack_.empty()) return nullptr;
            InputContext* top = stack_.back();
            stack_.pop_back();
            return top;
        }

        [[nodiscard]] InputContext*       top()   noexcept { return stack_.empty() ? nullptr : stack_.back(); }
        [[nodiscard]] const InputContext* top()   const noexcept { return stack_.empty() ? nullptr : stack_.back(); }
        [[nodiscard]] bool                empty() const noexcept { return stack_.empty(); }
        [[nodiscard]] std::size_t         size()  const noexcept { return stack_.size(); }

        /// Ordered from lowest (index 0) to highest (index size-1) priority.
        [[nodiscard]] std::span<InputContext* const> active() const noexcept { return stack_; }

    private:
        std::vector<InputContext*> stack_;
    };

} // namespace lux::input
