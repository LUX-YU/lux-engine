#pragma once
/**
 * @file ResidencyCallbacks.hpp
 * @brief Lightweight, host-injected boundary between ECS residency glue and
 *        the engine-level residency owner.
 *
 * This header contains only the callback protocol and its move-only RAII
 * ticket. It deliberately knows neither ResidencySubsystem nor any
 * scene-runtime implementation type, so public composition headers do not
 * pull the full ECS resolver implementation into every host translation unit.
 */

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class EResourceDomain : std::uint8_t;
    struct ResourceFailure;

    struct ResidencyCallbacks
    {
        /// Move-only type-erased subscription owner. Destruction is the only
        /// cancellation protocol exposed across the engine/ECS boundary.
        class Ticket final
        {
        public:
            Ticket() noexcept = default;
            explicit Ticket(
                lux::cxx::move_only_function<void()> release) noexcept
                : release_(std::move(release))
            {}
            ~Ticket() noexcept { reset(); }

            Ticket(const Ticket&) = delete;
            Ticket& operator=(const Ticket&) = delete;
            Ticket(Ticket&&) noexcept = default;
            Ticket& operator=(Ticket&& other) noexcept
            {
                if (this == &other)
                    return *this;
                reset();
                release_ = std::move(other.release_);
                return *this;
            }

            void reset() noexcept
            {
                auto release = std::move(release_);
                if (release)
                    release();
            }

            [[nodiscard]] bool active() const noexcept
            {
                return static_cast<bool>(release_);
            }

        private:
            lux::cxx::move_only_function<void()> release_{};
        };

        /// Exactly-once delivery: non-zero bits means READY; zero bits requires
        /// a non-null terminal failure.
        using DeliverFn = std::function<
            void(std::uint64_t bits, const ResourceFailure* failure)>;

        std::function<void(
            const lux::asset::asset_id_t&,
            EResourceDomain)> request;
        std::function<Ticket(
            const lux::asset::asset_id_t&,
            DeliverFn)> await;
        std::function<Ticket(std::function<void(
            const std::vector<lux::asset::asset_id_t>&)>)> watch_invalidation;
    };
} // namespace lux::ecs
