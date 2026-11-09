#pragma once

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lux::physics2d::detail
{
    class Box2DWorld final
    {
    public:
        using BodyId = std::uint32_t;

        // Caller supplies validated configuration. Nullopt means the backend's finite world table is full.
        [[nodiscard]] static std::optional<Box2DWorld> create(
            double gravity_x,
            double gravity_y,
            std::size_t body_capacity
        ) noexcept;
        ~Box2DWorld() noexcept;

        Box2DWorld(const Box2DWorld&) = delete;
        Box2DWorld& operator=(const Box2DWorld&) = delete;
        Box2DWorld(Box2DWorld&&) noexcept;
        Box2DWorld& operator=(Box2DWorld&&) noexcept;

        [[nodiscard]] std::optional<BodyId> createBox(
            Eigen::Vector2f center,
            float angle,
            Eigen::Vector2f half_extents,
            bool dynamic
        ) noexcept;
        void destroyBody(BodyId body) noexcept;
        void setTransform(BodyId body, Eigen::Vector2f center, float angle) noexcept;
        void setLinearVelocity(BodyId body, Eigen::Vector2f velocity) noexcept;
        void setGravityScale(BodyId body, float scale) noexcept;
        void step(float seconds) noexcept;

        [[nodiscard]] Eigen::Vector2f position(BodyId body) const noexcept;
        [[nodiscard]] float angle(BodyId body) const noexcept;
        [[nodiscard]] Eigen::Vector2f linearVelocity(BodyId body) const noexcept;
        [[nodiscard]] bool overlapsBox(Eigen::Vector2f center, Eigen::Vector2f half_extents) const noexcept;

    private:
        struct Impl;
        explicit Box2DWorld(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::physics2d::detail
