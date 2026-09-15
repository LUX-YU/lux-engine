#pragma once
#include <lux/engine/editor/scene/SceneResources.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <atomic>

namespace lux::editor::scene::detail
{
    // A result has its own sender-held lifetime. Publishing ready is not permission to destroy the sender.
    template <class T> struct AssetResult final
    {
        enum class EState : std::uint8_t
        {
            PENDING,
            VALUE,
            ERROR,
            CANCELLED
        };
        std::atomic<EState> state{EState::PENDING};
        std::shared_ptr<const T> value;
        lux::process::asset_loading::AssetLoadFailure failure;
    };

    class ResourceTasks final
    {
        struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            ResourceTasks *owner;

            void set_value() && noexcept
            {
                owner->done.store(true, std::memory_order_release);
            }

            stdexec::empty_env get_env() const noexcept
            {
                return {};
            }
        };

        using CloseOperation = stdexec::connect_result_t<lux::process::TaskScopeCloseSender, Receiver>;
        std::unique_ptr<CloseOperation> close_;
        bool started_{};

      public:
        lux::process::TaskScope scope;
        std::atomic<bool> done{};

        ~ResourceTasks() noexcept
        {
            // Cold construction failure has no asynchronous work. Close synchronously, without polling or waiting.
            if (!started_ && !scope.closed())
            {
                auto close = stdexec::connect(scope.close(), Receiver{this});
                stdexec::start(close);
                if (!done.load(std::memory_order_acquire))
                {
                    std::terminate();
                }
            }
            if (!scope.closed())
            {
                std::terminate();
            }
        }

        SceneResult<void> close() noexcept
        {
            if (close_)
            {
                return {};
            }
            {
                close_.reset(new CloseOperation(stdexec::connect(scope.close(), Receiver{this})));
                stdexec::start(*close_);
                return {};
            }
        }

        template <class T>
        lux::cxx::expected<void, lux::process::ETaskStartError> read(
            lux::process::asset_loading::AssetReadPort port, lux::asset::AssetId id,
            const std::shared_ptr<AssetResult<T>> &result) noexcept
        {
            auto values = stdexec::then(
                lux::process::asset_loading::loadAsset<T>(
                    std::move(port), id, lux::asset::AssetDecodeLimits{16 * 1024 * 1024, 32 * 1024 * 1024, 16}),
                [result](std::shared_ptr<const T> value) noexcept
                {
                    result->value = std::move(value);
                    result->state.store(AssetResult<T>::EState::VALUE, std::memory_order_release);
                });
            auto errors =
                stdexec::upon_error(std::move(values),
                                    [result](lux::process::asset_loading::AssetLoadFailure failure) noexcept
                                    {
                                        result->failure = failure;
                                        result->state.store(AssetResult<T>::EState::ERROR, std::memory_order_release);
                                    });
            auto lifetime = stdexec::upon_stopped(
                std::move(errors), [result]() noexcept
                { result->state.store(AssetResult<T>::EState::CANCELLED, std::memory_order_release); });
            auto admitted = scope.start(std::move(lifetime));
            if (admitted)
            {
                started_ = true;
            }
            return admitted;
        }
    };

} // namespace lux::editor::scene::detail
