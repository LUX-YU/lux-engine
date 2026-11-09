#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/core/visibility.h>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::object
{
    class LuxObject;
    struct ObjectIdTag;
    using ObjectId = cxx::SlotKey<ObjectIdTag>;

    enum class EObjectTreeError : std::uint8_t
    {
        INVALID_OBJECT,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        ALREADY_ATTACHED,
        INVALID_TREE
    };
    template <class T> using ObjectResult = cxx::expected<T, EObjectTreeError>;

    namespace detail
    {
        struct ObjectRuntimeState;
        void retainReclamation() noexcept;
        void releaseReclamation() noexcept;
    }

    // Explicitly owned identity directory. ExecutionRuntime outlives this object and every LuxObject.
    // All registration and synchronous address borrowing use its application thread.
    class LUX_CORE_PUBLIC ObjectRuntime final
    {
    public:
        using CreateResult = ObjectResult<std::unique_ptr<ObjectRuntime>>;

        [[nodiscard]] static CreateResult create(execution::ExecutionRuntime& execution) noexcept;
        // Borrows the explicitly created instance; never constructs an implicit runtime.
        [[nodiscard]] static ObjectRuntime& instance() noexcept;
        ~ObjectRuntime() noexcept;
        [[nodiscard]] bool isCurrent() const noexcept;
        [[nodiscard]] ObjectResult<LuxObject*> resolve(ObjectId) const noexcept;
        ObjectRuntime(const ObjectRuntime&) = delete;
        ObjectRuntime& operator=(const ObjectRuntime&) = delete;
        ObjectRuntime(ObjectRuntime&&) = delete;
        ObjectRuntime& operator=(ObjectRuntime&&) = delete;

    private:
        explicit ObjectRuntime(execution::ExecutionRuntime& execution) noexcept;
        [[nodiscard]] ObjectId registerObject(LuxObject&) noexcept;
        void unregisterObject(ObjectId) noexcept;
        friend class LuxObject;
        friend void detail::retainReclamation() noexcept;
        friend void detail::releaseReclamation() noexcept;
        std::unique_ptr<detail::ObjectRuntimeState> state_;
    };
}
