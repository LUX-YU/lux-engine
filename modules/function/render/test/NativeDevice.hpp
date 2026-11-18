#pragma once

#include "Support.hpp"
#include <lux/engine/render/device/NativeExecution.hpp>

namespace foundation_test
{
    class TestDevice final
    {
    public:
        TestDevice(
            const DeviceOptions& options,
            const nri::CallbackInterface& diagnostic,
            bool validation,
            std::source_location location = std::source_location::current()
        )
            : native_(take(NativeExecution::create(), location)),
              session_(take(DeviceSession::create(options, diagnostic, validation, native_->scheduler()), location))
        {
        }

        void reset() noexcept
        {
            session_.reset();
            native_.reset();
        }

        [[nodiscard]] DeviceSession* operator->() const noexcept { return session_.get(); }
        [[nodiscard]] DeviceSession& operator*() const noexcept { return *session_; }

    private:
        std::unique_ptr<NativeExecution> native_;
        std::unique_ptr<DeviceSession> session_;
    };
} // namespace foundation_test
