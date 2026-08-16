#include <lux/engine/runtime/frame/MainCloseDriver.hpp>

#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/logging/LogRouter.hpp>
#include <lux/engine/runtime/logging/LogRouterSenders.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadCloseSender.hpp>
#include <lux/engine/runtime/render/scene/ResidencyCloseSender.hpp>
#include <lux/engine/runtime/scene/SceneRuntimeCloseSender.hpp>
#include <lux/engine/runtime/extensions/EngineExtensionsCloseSender.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::runtime
{
    namespace
    {
        template <class Result>
        struct CloseState final
        {
            std::optional<Result> result;
            std::atomic<bool> completed{false};
            FrameCoordinator* coordinator{nullptr};
            // Keeps the connected operation state alive after a watchdog
            // return. The receiver clears the cycle only after terminal
            // delivery, while holding a local reference through that call.
            std::shared_ptr<void> operation_keepalive;
        };

        template <class Result>
        struct CloseReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            std::shared_ptr<CloseState<Result>> state;

            void set_value(Result value) && noexcept
            {
                auto keepalive = state->operation_keepalive;
                state->result.emplace(std::move(value));
                state->completed.store(true, std::memory_order_release);
                state->completed.notify_one();
                state->coordinator->notifyProgress();
                state->operation_keepalive.reset();
                (void)keepalive;
            }

            [[nodiscard]] stdexec::env<> get_env() const noexcept
            {
                return {};
            }
        };

        template <class Result, class Sender>
        struct CloseOperation final
        {
            using Receiver = CloseReceiver<Result>;
            using Operation = stdexec::connect_result_t<Sender, Receiver>;

            CloseOperation(
                Sender sender,
                std::shared_ptr<CloseState<Result>> state)
                : operation(stdexec::connect(
                      std::move(sender),
                      Receiver{std::move(state)}))
            {}

            Operation operation;
        };

        template <class Result, class Sender>
        [[nodiscard]] lux::cxx::expected<Result, EMainCloseError> drive(
            FrameCoordinator& coordinator,
            Sender sender,
            std::chrono::steady_clock::duration watchdog) noexcept
        {
            using StoredSender = std::decay_t<Sender>;
            auto state = std::make_shared<CloseState<Result>>();
            state->coordinator = &coordinator;
            auto operation = std::make_shared<
                CloseOperation<Result, StoredSender>>(
                    std::move(sender), state);
            state->operation_keepalive = operation;
            stdexec::start(operation->operation);

            const auto deadline = std::chrono::steady_clock::now() + watchdog;
            while (!state->completed.load(std::memory_order_acquire))
            {
                const auto observed = coordinator.observeProgress();
                const auto pumped = coordinator.pumpSafePoint();
                if (state->completed.load(std::memory_order_acquire))
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    return lux::cxx::unexpected(
                        EMainCloseError::WATCHDOG_EXPIRED);
                // A bounded mailbox drain may consume its entire budget and
                // leave already-enqueued work behind.  That residual task's
                // wake can predate `observed`, so sleeping here would lose the
                // only progress edge and stall until the watchdog.  Continue
                // only after real work was adopted; an idle pass still blocks
                // on the unified progress epoch.
                if (pumped != 0u)
                    continue;
                (void)coordinator.waitForProgressUntil(observed, deadline);
            }
            return std::move(*state->result);
        }

        struct VoidCloseState final
        {
            std::atomic<bool> completed{false};
            FrameCoordinator* coordinator{nullptr};
            std::shared_ptr<void> operation_keepalive;
        };

        struct VoidCloseReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            std::shared_ptr<VoidCloseState> state;

            void set_value() && noexcept
            {
                auto keepalive = state->operation_keepalive;
                state->completed.store(true, std::memory_order_release);
                state->completed.notify_one();
                state->coordinator->notifyProgress();
                state->operation_keepalive.reset();
                (void)keepalive;
            }

            [[nodiscard]] stdexec::env<> get_env() const noexcept
            {
                return {};
            }
        };

        template <class Sender>
        struct VoidCloseOperation final
        {
            using Operation = stdexec::connect_result_t<
                Sender,
                VoidCloseReceiver>;

            VoidCloseOperation(
                Sender sender,
                std::shared_ptr<VoidCloseState> state)
                : operation(stdexec::connect(
                      std::move(sender),
                      VoidCloseReceiver{std::move(state)}))
            {}

            Operation operation;
        };

        template <class Sender>
        [[nodiscard]] lux::cxx::expected<void, EMainCloseError> driveVoid(
            FrameCoordinator& coordinator,
            Sender sender,
            std::chrono::steady_clock::duration watchdog) noexcept
        {
            using StoredSender = std::decay_t<Sender>;
            auto state = std::make_shared<VoidCloseState>();
            state->coordinator = &coordinator;
            auto operation = std::make_shared<
                VoidCloseOperation<StoredSender>>(
                    std::move(sender), state);
            state->operation_keepalive = operation;
            stdexec::start(operation->operation);

            const auto deadline = std::chrono::steady_clock::now() + watchdog;
            while (!state->completed.load(std::memory_order_acquire))
            {
                const auto observed = coordinator.observeProgress();
                const auto pumped = coordinator.pumpSafePoint();
                if (state->completed.load(std::memory_order_acquire))
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    return lux::cxx::unexpected(
                        EMainCloseError::WATCHDOG_EXPIRED);
                if (pumped != 0u)
                    continue;
                (void)coordinator.waitForProgressUntil(observed, deadline);
            }
            return {};
        }
    }

    MainCloseDriver::MainCloseDriver(
        FrameCoordinator& coordinator,
        lux::exec::AsyncRuntime& runtime,
        std::chrono::steady_clock::duration watchdog) noexcept
        : coordinator_(coordinator)
        , runtime_(runtime)
        , watchdog_(watchdog)
    {}

    lux::cxx::expected<SceneCloseReport, EMainCloseError>
    MainCloseDriver::close(SceneRuntimeCloseSender sender) noexcept
    {
        return drive<SceneCloseReport>(
            coordinator_, std::move(sender), watchdog_);
    }

    lux::cxx::expected<SceneCloseReport, EMainCloseError>
    MainCloseDriver::close(SceneRuntime& owner) noexcept
    {
        return close(owner.closeAsync());
    }

    lux::cxx::expected<ResidencyCloseReport, EMainCloseError>
    MainCloseDriver::close(ResidencyCloseSender sender) noexcept
    {
        return drive<ResidencyCloseReport>(
            coordinator_, std::move(sender), watchdog_);
    }

    lux::cxx::expected<ResidencyCloseReport, EMainCloseError>
    MainCloseDriver::close(ResidencyAssembly& owner) noexcept
    {
        return close(owner.closeAsync());
    }

    lux::cxx::expected<AsyncRenderUploadCloseReport, EMainCloseError>
    MainCloseDriver::close(AsyncRenderUploadCloseSender sender) noexcept
    {
        return drive<AsyncRenderUploadCloseReport>(
            coordinator_, std::move(sender), watchdog_);
    }

    lux::cxx::expected<AsyncRenderUploadCloseReport, EMainCloseError>
    MainCloseDriver::close(AsyncRenderUploadService& owner) noexcept
    {
        return close(owner.closeAsync());
    }

    lux::cxx::expected<void, EMainCloseError>
    MainCloseDriver::close(lux::logging::LogRouterCloseSender sender) noexcept
    {
        auto report = drive<lux::logging::LogRouterStatistics>(
            coordinator_, std::move(sender), watchdog_);
        if (!report)
            return lux::cxx::unexpected(report.error());
        return {};
    }

    lux::cxx::expected<void, EMainCloseError>
    MainCloseDriver::close(lux::logging::LogRouter& owner) noexcept
    {
        return close(owner.closeAsync());
    }

    lux::cxx::expected<void, EMainCloseError>
    MainCloseDriver::close(lux::exec::AsyncScopeCloseSender sender) noexcept
    {
        return driveVoid(coordinator_, std::move(sender), watchdog_);
    }

    lux::cxx::expected<void, EMainCloseError>
    MainCloseDriver::close(lux::exec::AsyncScope& owner) noexcept
    {
        return close(owner.closeAsync());
    }

    lux::cxx::expected<
        lux::extensions::EngineExtensionsCloseReport,
        EMainCloseError>
    MainCloseDriver::close(
        lux::extensions::EngineExtensionsCloseSender sender) noexcept
    {
        return drive<lux::extensions::EngineExtensionsCloseReport>(
            coordinator_, std::move(sender), watchdog_);
    }

    lux::cxx::expected<
        lux::extensions::EngineExtensionsCloseReport,
        EMainCloseError>
    MainCloseDriver::close(
        lux::extensions::EngineExtensions& owner) noexcept
    {
        return close(owner.closeAsync());
    }

    lux::cxx::expected<lux::exec::AsyncCloseReport, EMainCloseError>
    MainCloseDriver::close(lux::exec::AsyncRuntimeCloseSender sender) noexcept
    {
        auto report = drive<lux::exec::AsyncCloseReport>(
            coordinator_, std::move(sender), watchdog_);
        if (!report)
            return lux::cxx::unexpected(report.error());
        const auto joined = runtime_.join();
        if (!joined)
            return lux::cxx::unexpected(
                EMainCloseError::RUNTIME_JOIN_FAILED);
        return std::move(*report);
    }

    lux::cxx::expected<lux::exec::AsyncCloseReport, EMainCloseError>
    MainCloseDriver::close(lux::exec::AsyncRuntime& owner) noexcept
    {
        return close(owner.closeAsync());
    }
}
