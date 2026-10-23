#pragma once

#include <cstdint>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>
#include <memory>

namespace lux::object
{
    class LuxObject;
    enum class EObjectPostStatus : std::uint8_t
    {
        POSTED,
        CLOSED,
        FULL
    };

    class ObjectTarget;
    // POSTED guarantees one owner-thread invocation: a borrowed live target, or nullptr after revocation.
    // Rejection does not invoke the callable. No waiting/retry; shutdown cancels accepted envelopes on owner.
    // A plugin supplies its code lifetime through callable destruction, including rejected delivery.
    [[nodiscard]] LUX_CORE_PUBLIC EObjectPostStatus post(
        const ObjectTarget& target,
        cxx::move_only_function<void(LuxObject*) noexcept> callback,
        std::shared_ptr<const void> code_lifetime = {}
    ) noexcept;

    // Copyable lifetime endpoint, never an object owner or a synchronous pointer borrow.
    class LUX_CORE_PUBLIC ObjectTarget final
    {
    public:
        ObjectTarget() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        friend bool operator==(const ObjectTarget&, const ObjectTarget&) noexcept = default;

    private:
        friend class LuxObject;
        friend LUX_CORE_PUBLIC EObjectPostStatus
        post(const ObjectTarget&, cxx::move_only_function<void(LuxObject*) noexcept>, std::shared_ptr<const void>) noexcept;
        explicit ObjectTarget(cxx::intrusive_ptr<detail::ObjectState>) noexcept;
        cxx::intrusive_ptr<detail::ObjectState> state_;
    };

} // namespace lux::object
