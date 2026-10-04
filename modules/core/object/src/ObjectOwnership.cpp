#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectOwnership.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    struct ObjectDeleter::Storage final
    {
        CodeLease code;
        Destroy destroy;
        Storage(CodeLease code, Destroy destroy) : code(std::move(code)), destroy(std::move(destroy)) {}
    };
    ObjectDeleter::ObjectDeleter() noexcept = default;
    ObjectDeleter::ObjectDeleter(CodeLease code, Destroy destroy)
        : storage_(std::make_unique<Storage>(std::move(code), std::move(destroy)))
    {
    }
    ObjectDeleter::~ObjectDeleter() = default;
    ObjectDeleter::ObjectDeleter(ObjectDeleter&&) noexcept = default;
    ObjectDeleter& ObjectDeleter::operator=(ObjectDeleter&& other) noexcept = default;
    void ObjectDeleter::operator()(LuxObject* value) noexcept
    {
        if (storage_)
        {
            storage_->destroy(value);
        }
        else
        {
            delete value;
        }
    }

} // namespace lux::object

namespace lux::object::detail
{
    ObjectResult<void> prepareSharedObject(LuxObject& value, const ObjectDispatcherRef& dispatcher) noexcept
    {
        if (!value.isOnAffinityThread())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        }
        if (value.dispatcherRef() != dispatcher)
        {
            return cxx::unexpected(EObjectTreeError::WRONG_DISPATCHER);
        }
        if (value.owned_edge_)
        {
            return cxx::unexpected(EObjectTreeError::ALREADY_ATTACHED);
        }
        const bool is_busy = !value.acceptsCallbacks() || value.hasActiveTree();
        if (is_busy)
        {
            return cxx::unexpected(EObjectTreeError::BUSY);
        }
        value.changing_children_ = true;
        return {};
    }

    void finishSharedObject(LuxObject& value) noexcept
    {
        value.changing_children_ = false;
    }

    struct AffinityOwner final : Reclamation
    {
        CodeLease code{CodeLease::builtin()};
        ObjectDispatcherRef dispatcher;
        cxx::move_only_function<void()> destroy;
        LuxObject* object{};

        static bool collect(Reclamation& node) noexcept
        {
            auto& value = static_cast<AffinityOwner&>(node);
            if (value.object)
            {
                if (value.object->hasActiveTree())
                {
                    return false;
                }
                value.object->closing_ = true;
                if (auto* state = value.object->state_.load(std::memory_order_acquire))
                {
                    state->closeOwner();
                }
            }
            value.destroy();
            value.destroy = {}; // Callable cleanup and its return remain covered by code.
            releaseReclamation(value.dispatcher);
            delete &value;
            return true;
        }
    };

    std::shared_ptr<void> makeAffinityOwner(
        ObjectDispatcherRef dispatcher,
        void* pointer,
        CodeLease code,
        cxx::move_only_function<void()> destroy,
        LuxObject* object
    ) noexcept
    {
        auto node = std::make_unique<AffinityOwner>();
        node->code = std::move(code);
        node->dispatcher = std::move(dispatcher);
        node->destroy = std::move(destroy);
        node->object = object;
        node->reclaim = &AffinityOwner::collect;
        retainReclamation(node->dispatcher);
        auto* prepared = node.release();
        return std::shared_ptr<void>(
            pointer,
            [prepared](void*) noexcept { scheduleReclamation(prepared->dispatcher, *prepared); }
        );
    }
} // namespace lux::object::detail
