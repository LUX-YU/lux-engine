#pragma once

#include <concepts>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <memory>
#include <type_traits>

namespace lux::object
{
    class LuxObject;
    namespace detail
    {
        struct AffinityOwner;
        [[nodiscard]] LUX_CORE_PUBLIC ObjectResult<void>
        prepareSharedObject(LuxObject&) noexcept;
        LUX_CORE_PUBLIC void finishSharedObject(LuxObject&) noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC std::shared_ptr<void>
        makeAffinityOwner(void*, CodeLease, cxx::move_only_function<void()>, LuxObject*) noexcept;
    } // namespace detail

    // The allocation is real, not a no-op deleter around a separately erasable Store slot.
    // Last release queues a node prepared here; destruction only runs in collectRetired().
    template <class T, class D>
        requires std::same_as<typename std::unique_ptr<T, D>::pointer, T*> && (!std::is_reference_v<D>) &&
                 std::is_nothrow_move_constructible_v<D> && std::is_nothrow_destructible_v<D>
    [[nodiscard]] ObjectResult<std::shared_ptr<T>> shareOnRuntime(
        std::unique_ptr<T, D>&& candidate,
        const CodeLease& code = CodeLease::builtin()
    ) noexcept
    {
        const bool is_invalid_owner = !candidate || !code.valid();
        if (is_invalid_owner)
        {
            return cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
        }
        if (!ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        }
        LuxObject* object{};
        if constexpr (std::is_base_of_v<LuxObject, T>)
        {
            object = candidate.get();
            auto valid = detail::prepareSharedObject(*object);
            if (!valid)
            {
                return cxx::unexpected(valid.error());
            }
        }
        auto* pointer = candidate.get();
        auto pin = code; // Protect deleter moves even when they execute foreign cleanup callbacks.
        auto owner = detail::makeAffinityOwner(
            pointer,
            std::move(pin),
            [value = std::move(candidate)]() mutable noexcept { value.reset(); },
            object
        );
        if (object)
        {
            detail::finishSharedObject(*object);
        }
        return std::shared_ptr<T>(std::move(owner), pointer);
    }

} // namespace lux::object
