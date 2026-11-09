#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>

#include <atomic>
#include <memory>

namespace lux::object::detail
{
    struct ObjectRuntimeState final
    {
        explicit ObjectRuntimeState(execution::ExecutionRuntime& execution) noexcept : execution(execution) {}

        execution::ExecutionRuntime& execution;
        cxx::SlotKeyAutoSparseSet<ObjectId, LuxObject*> objects;
        std::atomic_size_t retirement_owners{};
    };

    void retainReclamation() noexcept
    {
        ObjectRuntime::instance().state_->retirement_owners.fetch_add(1, std::memory_order_relaxed);
    }

    void releaseReclamation() noexcept
    {
        ObjectRuntime::instance().state_->retirement_owners.fetch_sub(1, std::memory_order_relaxed);
    }

} // namespace lux::object::detail

namespace lux::object
{
    namespace
    {
        ObjectRuntime* current_runtime = nullptr;
    }

    ObjectRuntime::CreateResult ObjectRuntime::create(execution::ExecutionRuntime& execution) noexcept
    {
        if (!execution.isMainThread())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        }
        if (current_runtime)
        {
            return cxx::unexpected(EObjectTreeError::BUSY);
        }
        if (execution.isStopping())
        {
            return cxx::unexpected(EObjectTreeError::CLOSED);
        }
        auto result = std::unique_ptr<ObjectRuntime>{new ObjectRuntime(execution)};
        current_runtime = result.get();
        return result;
    }

    ObjectRuntime& ObjectRuntime::instance() noexcept
    {
        if (!current_runtime)
        {
            detail::failObjectContract();
        }
        return *current_runtime;
    }

    ObjectRuntime::ObjectRuntime(execution::ExecutionRuntime& execution) noexcept
        : state_(std::make_unique<detail::ObjectRuntimeState>(execution))
    {
    }

    ObjectRuntime::~ObjectRuntime() noexcept
    {
        const bool has_live_objects = !state_->objects.empty();
        const bool has_reclamation = state_->retirement_owners.load(std::memory_order_relaxed) != 0;
        if (!isCurrent() || has_live_objects || has_reclamation)
        {
            detail::failObjectContract();
        }
        current_runtime = nullptr;
    }

    bool ObjectRuntime::isCurrent() const noexcept
    {
        return state_->execution.isMainThread();
    }

    ObjectId ObjectRuntime::registerObject(LuxObject& object) noexcept
    {
        if (!isCurrent() || state_->execution.isStopping())
        {
            detail::failObjectContract();
        }
        const auto id = state_->objects.insert(&object);
        if (id.isNull())
        {
            detail::failObjectContract();
        }
        return id;
    }

    void ObjectRuntime::unregisterObject(ObjectId id) noexcept
    {
        if (!isCurrent())
        {
            detail::failObjectContract();
        }
        state_->objects.erase(id);
    }

    ObjectResult<LuxObject*> ObjectRuntime::resolve(ObjectId id) const noexcept
    {
        if (!isCurrent())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        }
        if (id.isNull())
        {
            return cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
        }
        const auto* found = state_->objects.tryGet(id);
        if (!found)
        {
            return cxx::unexpected(EObjectTreeError::CLOSED);
        }
        return *found;
    }
} // namespace lux::object
