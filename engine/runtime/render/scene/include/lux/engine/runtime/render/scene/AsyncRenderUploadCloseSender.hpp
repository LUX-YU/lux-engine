#pragma once
/**
 * @file AsyncRenderUploadCloseSender.hpp
 * @brief Opt-in sender adapter for the coordinator-owned upload bridge.
 */

#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <stdexec/execution.hpp>

#include <type_traits>
#include <utility>

namespace lux::runtime
{
    class AsyncRenderUploadCloseSender final
    {
    public:
        using Completion = lux::cxx::move_only_function<void(
            AsyncRenderUploadCloseReport)>;
        using Subscribe = void (*)(void*, Completion) noexcept;
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(AsyncRenderUploadCloseReport)>;

        AsyncRenderUploadCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            std::shared_ptr<void> owner;
            Subscribe subscribe{nullptr};
            Receiver receiver;

            void start() & noexcept
            {
                if (!owner || subscribe == nullptr)
                {
                    stdexec::set_value(
                        std::move(receiver),
                        AsyncRenderUploadCloseReport{});
                    return;
                }
                subscribe(
                    owner.get(),
                    [this](AsyncRenderUploadCloseReport report)
                        mutable noexcept
                    {
                        stdexec::set_value(
                            std::move(receiver), std::move(report));
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {
                std::move(owner_),
                std::exchange(subscribe_, nullptr),
                std::forward<Receiver>(receiver)};
        }

    private:
        friend class AsyncRenderUploadService;
        explicit AsyncRenderUploadCloseSender(
            std::shared_ptr<void> owner,
            Subscribe subscribe) noexcept
            : owner_(std::move(owner)), subscribe_(subscribe)
        {}

        std::shared_ptr<void> owner_;
        Subscribe subscribe_{nullptr};
    };
}
