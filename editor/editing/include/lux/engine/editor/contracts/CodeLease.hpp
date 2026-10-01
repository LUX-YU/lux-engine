#pragma once

#include <memory>
#include <utility>

namespace lux::editor::contracts
{
    // Keep this outside plugin-defined objects: their destructor must return before code is released.
    class CodeLease final
    {
    public:
        [[nodiscard]] static CodeLease builtin() noexcept
        {
            return CodeLease{true, {}};
        }
        [[nodiscard]] static CodeLease plugin(std::shared_ptr<const void> owner) noexcept
        {
            return CodeLease{false, std::move(owner)};
        }
        [[nodiscard]] bool valid() const noexcept
        {
            return builtin_ || bool(owner_);
        }
        [[nodiscard]] bool sameOwner(const CodeLease& other) const noexcept
        {
            return builtin_ == other.builtin_ && !owner_.owner_before(other.owner_) &&
                   !other.owner_.owner_before(owner_);
        }

    private:
        CodeLease(bool builtin, std::shared_ptr<const void> owner) noexcept
            : builtin_(builtin), owner_(std::move(owner))
        {}
        bool builtin_;
        std::shared_ptr<const void> owner_;
    };
    // Call at the receiving module boundary. The returned alias owns the incoming control block,
    // then its defining code; even an expired weak alias can be destroyed after that code unloads.
    template <class T> [[nodiscard]] std::shared_ptr<T> pinCodeOwner(CodeLease code, std::shared_ptr<T> value)
    {
        struct Owner final
        {
            CodeLease code;
            std::shared_ptr<T> value;
        };
        auto owner = std::make_shared<Owner>(std::move(code), std::move(value));
        auto* pointer = owner->value.get();
        return std::shared_ptr<T>(std::move(owner), pointer);
    }
}
