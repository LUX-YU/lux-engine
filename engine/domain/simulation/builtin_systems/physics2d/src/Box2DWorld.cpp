#include <lux/engine/physics2d/Box2DWorld.hpp>

#include <box2d/box2d.h>

#include <cmath>
#include <vector>

namespace lux::physics2d::detail
{
    struct Box2DWorld::Impl final
    {
        b2WorldId world;
        std::vector<b2BodyId> bodies;
        std::vector<BodyId> free_bodies;

        Impl(b2WorldId prepared_world, std::size_t body_capacity) noexcept
            : world(prepared_world), bodies(body_capacity, b2_nullBodyId)
        {
            free_bodies.reserve(body_capacity);
            for (std::size_t index{body_capacity}; index > 0U; --index)
            {
                free_bodies.push_back(static_cast<BodyId>(index - 1U));
            }
        }

        ~Impl() noexcept
        {
            b2DestroyWorld(world);
        }

        [[nodiscard]] b2BodyId body(BodyId id) const noexcept
        {
            if (id >= bodies.size() || !b2Body_IsValid(bodies[id]))
            {
                return b2_nullBodyId;
            }
            return bodies[id];
        }
    };

    std::optional<Box2DWorld> Box2DWorld::create(double gravity_x, double gravity_y, std::size_t body_capacity) noexcept
    {
        auto definition = b2DefaultWorldDef();
        definition.gravity = {static_cast<float>(gravity_x), static_cast<float>(gravity_y)};
        const auto world = b2CreateWorld(&definition);
        if (!b2World_IsValid(world))
        {
            return std::nullopt;
        }
        return Box2DWorld(std::make_unique<Impl>(world, body_capacity));
    }

    Box2DWorld::Box2DWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    Box2DWorld::Box2DWorld(Box2DWorld&&) noexcept = default;

    Box2DWorld& Box2DWorld::operator=(Box2DWorld&&) noexcept = default;

    Box2DWorld::~Box2DWorld() noexcept = default;

    std::optional<Box2DWorld::BodyId> Box2DWorld::createBox(
        Eigen::Vector2f center,
        float angle,
        Eigen::Vector2f half_extents,
        bool dynamic
    ) noexcept
    {
        const bool is_invalid_shape = !center.allFinite() || !half_extents.allFinite() || half_extents.x() <= 0.0F ||
                                      half_extents.y() <= 0.0F || !std::isfinite(angle);
        const bool has_capacity = !impl_->free_bodies.empty();
        if (is_invalid_shape || !has_capacity)
        {
            return std::nullopt;
        }
        auto body_definition = b2DefaultBodyDef();
        body_definition.type = dynamic ? b2_dynamicBody : b2_staticBody;
        body_definition.position = {center.x(), center.y()};
        body_definition.rotation = b2MakeRot(angle);
        const auto body = b2CreateBody(impl_->world, &body_definition);
        if (!b2Body_IsValid(body))
        {
            return std::nullopt;
        }

        const auto polygon = b2MakeBox(half_extents.x(), half_extents.y());
        auto shape_definition = b2DefaultShapeDef();
        const auto shape = b2CreatePolygonShape(body, &shape_definition, &polygon);
        if (!b2Shape_IsValid(shape))
        {
            b2DestroyBody(body);
            return std::nullopt;
        }

        const auto id = impl_->free_bodies.back();
        impl_->free_bodies.pop_back();
        impl_->bodies[id] = body;
        return id;
    }

    void Box2DWorld::destroyBody(BodyId body) noexcept
    {
        const auto value = impl_->body(body);
        if (!b2Body_IsValid(value))
        {
            return;
        }
        b2DestroyBody(value);
        impl_->bodies[body] = b2_nullBodyId;
        impl_->free_bodies.push_back(body);
    }

    void Box2DWorld::setTransform(BodyId body, Eigen::Vector2f center, float angle) noexcept
    {
        const auto value = impl_->body(body);
        const bool is_valid_transform = center.allFinite() && std::isfinite(angle);
        if (b2Body_IsValid(value) && is_valid_transform)
        {
            b2Body_SetTransform(value, {center.x(), center.y()}, b2MakeRot(angle));
        }
    }

    void Box2DWorld::setLinearVelocity(BodyId body, Eigen::Vector2f velocity) noexcept
    {
        const auto value = impl_->body(body);
        if (b2Body_IsValid(value) && velocity.allFinite())
        {
            b2Body_SetLinearVelocity(value, {velocity.x(), velocity.y()});
        }
    }

    void Box2DWorld::setGravityScale(BodyId body, float scale) noexcept
    {
        const auto value = impl_->body(body);
        if (b2Body_IsValid(value) && std::isfinite(scale))
        {
            b2Body_SetGravityScale(value, scale);
        }
    }

    void Box2DWorld::step(float seconds) noexcept
    {
        if (std::isfinite(seconds) && seconds > 0.0F)
        {
            b2World_Step(impl_->world, seconds, 4);
        }
    }

    Eigen::Vector2f Box2DWorld::position(BodyId body) const noexcept
    {
        const auto value = impl_->body(body);
        if (!b2Body_IsValid(value))
        {
            return Eigen::Vector2f::Zero();
        }
        const auto position = b2Body_GetPosition(value);
        return {position.x, position.y};
    }

    float Box2DWorld::angle(BodyId body) const noexcept
    {
        const auto value = impl_->body(body);
        return b2Body_IsValid(value) ? b2Rot_GetAngle(b2Body_GetRotation(value)) : 0.0F;
    }

    Eigen::Vector2f Box2DWorld::linearVelocity(BodyId body) const noexcept
    {
        const auto value = impl_->body(body);
        if (!b2Body_IsValid(value))
        {
            return Eigen::Vector2f::Zero();
        }
        const auto velocity = b2Body_GetLinearVelocity(value);
        return {velocity.x, velocity.y};
    }

    bool Box2DWorld::overlapsBox(Eigen::Vector2f center, Eigen::Vector2f half_extents) const noexcept
    {
        const bool is_invalid_query =
            !center.allFinite() || !half_extents.allFinite() || half_extents.x() <= 0.0F || half_extents.y() <= 0.0F;
        if (is_invalid_query)
        {
            return false;
        }
        bool found{};
        const b2AABB bounds{
            {center.x() - half_extents.x(), center.y() - half_extents.y()},
            {center.x() + half_extents.x(), center.y() + half_extents.y()}
        };
        auto filter = b2DefaultQueryFilter();
        b2World_OverlapAABB(
            impl_->world,
            bounds,
            filter,
            [](b2ShapeId, void* context) noexcept
            {
                *static_cast<bool*>(context) = true;
                return false;
            },
            &found
        );
        return found;
    }
} // namespace lux::physics2d::detail
