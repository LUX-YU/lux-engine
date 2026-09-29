#pragma once

#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/scene/visibility.h>
#include <memory>

namespace lux::scene
{
    namespace detail
    {
        struct InstanceLifetime;
    }

    // Completion survives removal of the runtime slot. Requesting retirement is not completion.
    class LUX_ENGINE_SCENE_PUBLIC InstanceRetirement final
    {
    public:
        InstanceRetirement() noexcept = default;
        [[nodiscard]] bool complete() const noexcept;
        [[nodiscard]] SceneInstanceId id() const noexcept;

    private:
        friend class SceneRuntime;
        friend class SceneInstanceLease;
        explicit InstanceRetirement(std::shared_ptr<detail::InstanceLifetime>) noexcept;
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
}
