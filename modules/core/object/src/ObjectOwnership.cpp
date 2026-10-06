#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectOwnership.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object::detail
{
    ObjectResult<void> prepareSharedObject(LuxObject& value) noexcept
    {
        if (!value.isOnAffinityThread())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
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
                value.object->beginDestruction();
                if (auto* state = value.object->state_.load(std::memory_order_acquire))
                {
                    state->closeOwner();
                }
            }
            value.destroy();
            value.destroy = {}; // Callable cleanup and its return remain covered by code.
            releaseReclamation();
            delete &value;
            return true;
        }
    };

    std::shared_ptr<void> makeAffinityOwner(
        void* pointer,
        CodeLease code,
        cxx::move_only_function<void()> destroy,
        LuxObject* object
    ) noexcept
    {
        auto node = std::make_unique<AffinityOwner>();
        node->code = std::move(code);
        node->destroy = std::move(destroy);
        node->object = object;
        node->reclaim = &AffinityOwner::collect;
        retainReclamation();
        auto* prepared = node.release();
        return std::shared_ptr<void>(
            pointer,
            [prepared](void*) noexcept { scheduleReclamation(*prepared); }
        );
    }
} // namespace lux::object::detail
