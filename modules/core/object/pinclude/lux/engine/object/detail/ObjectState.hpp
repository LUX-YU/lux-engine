#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/object/LuxObject.hpp>

namespace lux::object::detail
{
    struct ObjectControl final
    {
        std::atomic<LuxObject*> object{nullptr};
    };

    struct ObjectSlot final
    {
        std::atomic_bool connected{true};
        lux::cxx::move_only_function<void(const void*)> callback;
        std::weak_ptr<ObjectControl> receiver;
        bool has_receiver{false};
        ObjectDispatcher* dispatcher{nullptr};
        EDelivery delivery{EDelivery::DIRECT};
    };

    struct SignalBucket final
    {
        std::vector<std::shared_ptr<ObjectSlot>> active;
        std::vector<std::shared_ptr<ObjectSlot>> pending;
        std::size_t notify_depth{0};
    };

    struct ObjectState final
    {
        std::shared_ptr<ObjectControl> control;
        std::unordered_map<const SignalHeader*, SignalBucket> signals;
    };
}
