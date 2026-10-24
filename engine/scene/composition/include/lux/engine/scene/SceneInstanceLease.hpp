#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/scene/visibility.h>
#include <memory>
#include <stdexec/execution.hpp>

namespace lux::scene
{
    namespace detail
    {
        struct InstanceLifetime;

        struct RetirementWaiter
        {
            RetirementWaiter* next{};
            std::shared_ptr<const void> code_lifetime;
            void (*complete)(RetirementWaiter*) noexcept {};
        };

        LUX_ENGINE_SCENE_PUBLIC void
        awaitRetirement(const std::shared_ptr<InstanceLifetime>&, RetirementWaiter&) noexcept;
    } // namespace detail

    // Completion and the unique bounded step-result ledger survive removal of the runtime slot.
    // Requesting retirement is not completion. Result access still requires the owner Runtime.
    class LUX_ENGINE_SCENE_PUBLIC InstanceRetirement final
    {
    public:
        class CompletionSender;
        InstanceRetirement() noexcept = default;
        [[nodiscard]] bool complete() const noexcept;
        [[nodiscard]] SceneInstanceId id() const noexcept;
        // Start on the SceneRuntime thread. Completion proves retirement, even if the receiver's
        // stop token is requested; cancellation cannot release a still-borrowed Scene prematurely.
        [[nodiscard]] CompletionSender whenComplete() const noexcept;

    private:
        friend class SceneRuntime;
        friend class SceneInstanceLease;
        explicit InstanceRetirement(std::shared_ptr<detail::InstanceLifetime>) noexcept;
        std::shared_ptr<detail::InstanceLifetime> lifetime_;
    };

    class InstanceRetirement::CompletionSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

        [[nodiscard]] stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        template <class Receiver> class Operation final : private detail::RetirementWaiter
        {
        public:
            Operation(std::shared_ptr<detail::InstanceLifetime> lifetime, Receiver receiver) noexcept
                : lifetime_(std::move(lifetime)), receiver_(std::move(receiver))
            {
                code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(receiver_));
                complete = [](detail::RetirementWaiter* waiter) noexcept
                {
                    auto& operation = *static_cast<Operation*>(waiter);
                    stdexec::set_value(std::move(operation.receiver_));
                };
            }

            void start() & noexcept
            {
                detail::awaitRetirement(lifetime_, *this);
            }

        private:
            std::shared_ptr<detail::InstanceLifetime> lifetime_;
            Receiver receiver_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{lifetime_, std::move(receiver)};
        }

    private:
        friend class InstanceRetirement;

        explicit CompletionSender(std::shared_ptr<detail::InstanceLifetime> lifetime) noexcept
            : lifetime_(std::move(lifetime))
        {
        }

        std::shared_ptr<detail::InstanceLifetime> lifetime_;
    };

    // Sole retirement responsibility. Destruction only records intent and wakes the host;
    // it never invokes system callbacks, waits, allocates, or erases a runtime record.
    class LUX_ENGINE_SCENE_PUBLIC SceneInstanceLease final
    {
    public:
        SceneInstanceLease() noexcept = default;
        ~SceneInstanceLease() noexcept;
        SceneInstanceLease(SceneInstanceLease&&) noexcept;
        SceneInstanceLease& operator=(SceneInstanceLease&&) noexcept;
        SceneInstanceLease(const SceneInstanceLease&) = delete;
        SceneInstanceLease& operator=(const SceneInstanceLease&) = delete;

        [[nodiscard]] SceneInstanceId id() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] InstanceRetirement retire() noexcept;

    private:
        friend class SceneRuntime;
        explicit SceneInstanceLease(std::shared_ptr<detail::InstanceLifetime>) noexcept;
        std::shared_ptr<detail::InstanceLifetime> lifetime_;
    };
} // namespace lux::scene
