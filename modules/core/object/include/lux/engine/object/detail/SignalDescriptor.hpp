#pragma once

#include <cstddef>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>

namespace lux::object
{
    class LuxObject;
    template <class Derived, class Base> class Object;
    template <class Owner, class Payload> class Signal;

    namespace reflection
    {
        class SignalView;
    }

    namespace detail
    {
        struct ObjectDiagnosticsAccess;

        using QueuedMessageFactory = MessageEnvelope (*)(lux::cxx::intrusive_ptr<ConnectionControl>,
                                                         const void *) noexcept;

        class SignalDescriptor final
        {
          private:
            template <class Owner, class Payload> friend class ::lux::object::Signal;
            friend class ::lux::object::LuxObject;
            friend class ::lux::object::reflection::SignalView;
            friend struct ObjectState;
            friend struct ObjectDiagnosticsAccess;

            constexpr SignalDescriptor(std::size_t dense_index, std::size_t lineage_size,
                                       lux::cxx::TypeToken owner, lux::cxx::TypeToken payload,
                                       QueuedMessageFactory queued_message_factory) noexcept
                : dense_index_(dense_index), lineage_size_(lineage_size), owner_(owner),
                  payload_(payload), queued_message_factory_(queued_message_factory)
            {
            }

            std::size_t dense_index_{0};
            std::size_t lineage_size_{0};
            lux::cxx::TypeToken owner_;
            lux::cxx::TypeToken payload_;
            QueuedMessageFactory queued_message_factory_{nullptr};
        };
    } // namespace detail
} // namespace lux::object
