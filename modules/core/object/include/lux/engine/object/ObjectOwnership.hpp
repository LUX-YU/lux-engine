#pragma once

#include <concepts>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <memory>
#include <type_traits>

namespace lux::object
{
    class LuxObject;
    enum class EObjectOwnership : std::uint8_t
    {
        EXTERNAL,
        PARENT_OWNED
    };
    enum class EObjectTreeError : std::uint8_t
    {
        INVALID_OBJECT,
        WRONG_THREAD,
        WRONG_DISPATCHER,
        BUSY,
        CLOSED,
        ALREADY_ATTACHED,
        INVALID_TREE,
        NOT_OWNED,
        OWNED_CHILD
    };
    template <class T> using ObjectResult = cxx::expected<T, EObjectTreeError>;

    namespace detail
    {
        struct AffinityOwner;
        [[nodiscard]] LUX_CORE_PUBLIC ObjectResult<void>
        prepareSharedObject(LuxObject&, const ObjectDispatcherRef&) noexcept;
        LUX_CORE_PUBLIC void finishSharedObject(LuxObject&) noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC std::shared_ptr<void>
        makeAffinityOwner(ObjectDispatcherRef, void*, CodeLease, cxx::move_only_function<void()>, LuxObject*) noexcept;
    } // namespace detail

    // The allocation is real, not a no-op deleter around a separately erasable Store slot.
    // Last release queues a node prepared here; destruction only runs in collectRetired().
    template <class T, class D>
        requires std::same_as<typename std::unique_ptr<T, D>::pointer, T*> && (!std::is_reference_v<D>) &&
                 std::is_nothrow_move_constructible_v<D> && std::is_nothrow_destructible_v<D>
    [[nodiscard]] ObjectResult<std::shared_ptr<T>> shareOnDispatcher(
        const ObjectDispatcherRef& dispatcher,
        std::unique_ptr<T, D>&& candidate,
        const CodeLease& code = CodeLease::builtin()
    ) noexcept
    {
        const bool is_invalid_owner = !candidate || !code.valid();
        if (is_invalid_owner)
        {
            return cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
        }
        if (!dispatcher.isCurrent())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_DISPATCHER);
        }
        LuxObject* object{};
        if constexpr (std::is_base_of_v<LuxObject, T>)
        {
            object = candidate.get();
            auto valid = detail::prepareSharedObject(*object, dispatcher);
            if (!valid)
            {
                return cxx::unexpected(valid.error());
            }
        }
        auto* pointer = candidate.get();
        auto pin = code; // Protect deleter moves even when they execute foreign cleanup callbacks.
        auto owner = detail::makeAffinityOwner(
            dispatcher,
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

    // A real matching destruction operation. The code lease outlives callable destruction and return.
    class LUX_CORE_PUBLIC ObjectDeleter final
    {
    public:
        ObjectDeleter() noexcept;
        ~ObjectDeleter();
        ObjectDeleter(ObjectDeleter&&) noexcept;
        ObjectDeleter& operator=(ObjectDeleter&&) noexcept;
        ObjectDeleter(const ObjectDeleter&) = delete;
        ObjectDeleter& operator=(const ObjectDeleter&) = delete;
        template <class T, class D>
            requires(!std::is_reference_v<D>) && std::is_nothrow_move_constructible_v<D> &&
                    std::is_nothrow_destructible_v<D>
        [[nodiscard]] static ObjectDeleter create(D&& deleter, CodeLease code = CodeLease::builtin()) noexcept
        {
            return ObjectDeleter{
                std::move(code),
                [stored = std::move(deleter)](LuxObject* value) mutable noexcept
                {
                    if constexpr (requires { static_cast<T*>(value); })
                    {
                        stored(static_cast<T*>(value));
                    }
                    else
                    {
                        stored(dynamic_cast<T*>(value));
                    }
                }
            };
        }
        void operator()(LuxObject*) noexcept;

    private:
        using Destroy = cxx::move_only_function<void(LuxObject*)>;
        ObjectDeleter(CodeLease, Destroy);
        // Stable destruction state: moving a prepared owner cannot execute a foreign deleter move.
        struct Storage;
        std::unique_ptr<Storage> storage_;
    };
} // namespace lux::object
