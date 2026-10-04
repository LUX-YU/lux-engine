#pragma once

#include <lux/engine/core/visibility.h>
#include <memory>
#include <utility>

namespace lux::object
{
    // The lease lives outside plugin values, callbacks and deleters, including their return paths.
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
        {
        }
        bool builtin_;
        std::shared_ptr<const void> owner_;
    };

    namespace detail
    {
        // This implementation and its control block execute in the object provider, not in a plugin.
        [[nodiscard]] LUX_CORE_PUBLIC std::shared_ptr<const void>
        pinCodeOwner(CodeLease, std::shared_ptr<const void>) noexcept;
    } // namespace detail

    template <class T> [[nodiscard]] std::shared_ptr<T> pinCodeOwner(CodeLease code, std::shared_ptr<T> value) noexcept
    {
        auto* pointer = value.get();
        auto owner = detail::pinCodeOwner(std::move(code), std::move(value));
        return std::shared_ptr<T>(std::move(owner), pointer);
    }
} // namespace lux::object
