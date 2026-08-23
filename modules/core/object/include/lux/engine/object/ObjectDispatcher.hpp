#pragma once

#include <cstddef>
#include <memory>
#include <thread>

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/core/visibility.h>

namespace lux::object
{
    enum class EPostStatus
    {
        POSTED,
        CLOSED
    };

    class LUX_CORE_PUBLIC ObjectDispatcher final
    {
      public:
        ObjectDispatcher();
        ~ObjectDispatcher();

        ObjectDispatcher(const ObjectDispatcher&) = delete;
        ObjectDispatcher& operator=(const ObjectDispatcher&) = delete;

        [[nodiscard]] EPostStatus post(lux::cxx::move_only_function<void()> message);
        [[nodiscard]] std::size_t dispatchPending();
        void close() noexcept;

        [[nodiscard]] std::thread::id ownerThread() const noexcept;
        [[nodiscard]] bool isOwnerThread() const noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
