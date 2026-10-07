#pragma once

#include <cstdint>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>

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
    [[nodiscard]] LUX_CORE_PUBLIC EObjectPostStatus
    post(const ObjectTarget&, cxx::move_only_function<void(LuxObject*) noexcept>) noexcept;

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
        post(const ObjectTarget&, cxx::move_only_function<void(LuxObject*) noexcept>) noexcept;
        explicit ObjectTarget(cxx::intrusive_ptr<detail::ObjectState>) noexcept;
        cxx::intrusive_ptr<detail::ObjectState> state_;
    };

} // namespace lux::object
