#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/core/visibility.h>

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
        enum class EPostStatus : std::uint8_t { POSTED, CLOSED, FULL };
        class MessageEnvelope;
        struct ObjectRuntimeState;
        struct SignalStorage;
        struct Reclamation;
        void scheduleReclamation(Reclamation&) noexcept;
        void retainReclamation() noexcept;
        void releaseReclamation() noexcept;
        void scheduleSignalMaintenance(SignalStorage&) noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC EPostStatus post(MessageEnvelope&&) noexcept;
    }

    struct ObjectQueueStatistics final
    {
        std::size_t capacity_per_batch{}, pending{}, high_water{}, posted{}, inline_posted{}, full{};
    };

    // First access by the host establishes the process's object thread before workers start.
    // Registration is non-owning. Resolved addresses are synchronous owner-thread borrows.
    class LUX_CORE_PUBLIC ObjectRuntime final
    {
    public:
        [[nodiscard]] static ObjectRuntime& instance() noexcept;
        [[nodiscard]] bool isCurrent() const noexcept;
        [[nodiscard]] ObjectResult<LuxObject*> resolve(ObjectId) const noexcept;
        [[nodiscard]] std::size_t dispatchPending();
        [[nodiscard]] std::size_t collectRetired() noexcept;
        [[nodiscard]] std::size_t pendingRetirements() const noexcept;
        [[nodiscard]] ObjectQueueStatistics statistics() const noexcept;
        void setWake(void (*)() noexcept) noexcept;
        ObjectRuntime(const ObjectRuntime&) = delete;
        ObjectRuntime& operator=(const ObjectRuntime&) = delete;
        ObjectRuntime(ObjectRuntime&&) = delete;
        ObjectRuntime& operator=(ObjectRuntime&&) = delete;

    private:
        ObjectRuntime() noexcept;
        ~ObjectRuntime();
        [[nodiscard]] ObjectId registerObject(LuxObject&) noexcept;
        void unregisterObject(ObjectId) noexcept;
        void close() noexcept;
        void closeAndReclaim() noexcept;
        friend class LuxObject;
        friend void detail::scheduleSignalMaintenance(detail::SignalStorage&) noexcept;
        friend void detail::scheduleReclamation(detail::Reclamation&) noexcept;
        friend void detail::retainReclamation() noexcept;
        friend void detail::releaseReclamation() noexcept;
        friend detail::EPostStatus detail::post(detail::MessageEnvelope&&) noexcept;
        std::unique_ptr<detail::ObjectRuntimeState> state_;
    };
}
