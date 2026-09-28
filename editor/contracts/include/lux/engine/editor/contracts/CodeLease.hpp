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

    private:
        CodeLease(bool builtin, std::shared_ptr<const void> owner) noexcept
            : builtin_(builtin), owner_(std::move(owner))
        {}
        bool builtin_;
        std::shared_ptr<const void> owner_;
    };
}
