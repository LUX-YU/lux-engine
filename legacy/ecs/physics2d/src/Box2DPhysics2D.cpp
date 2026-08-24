// ============================================================================
//  Box2DPhysics2D.cpp — Box2D v3 (C API) lives here and NOWHERE else. The
//  header stays box2d-free; box2d::box2d is a PRIVATE link of the physics
//  target, so nothing downstream sees a b2* symbol. Bodies are held in a map
//  keyed by a monotonic BodyId, so ids stay valid when other bodies are
//  destroyed (an ECS entity↔body mapping needs that stability).
// ============================================================================

#include <lux/engine/ecs/physics2d/systems/Box2DPhysics2D.hpp>

#include <box2d/box2d.h>

#include <unordered_map>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        b2BodyType toB2(Box2DPhysics2D::EBodyType t) noexcept
        {
            switch (t)
            {
                case Box2DPhysics2D::EBodyType::Kinematic: return b2_kinematicBody;
                case Box2DPhysics2D::EBodyType::Dynamic:   return b2_dynamicBody;
                case Box2DPhysics2D::EBodyType::Static:    break;
            }
            return b2_staticBody;
        }
    } // namespace

    struct Box2DPhysics2D::Impl
    {
        b2WorldId                            world = b2_nullWorldId;
        std::unordered_map<BodyId, b2BodyId> bodies;
        BodyId                               next = 0;
        std::vector<ContactPair>             begin_contacts;   // last step's enter pairs

        [[nodiscard]] b2BodyId find(BodyId id) const noexcept
        {
            const auto it = bodies.find(id);
            return it == bodies.end() ? b2_nullBodyId : it->second;
        }
    };

    Box2DPhysics2D::Box2DPhysics2D(Eigen::Vector2f gravity)
        : impl_(std::make_unique<Impl>())
    {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity    = b2Vec2{gravity.x(), gravity.y()};
        impl_->world   = b2CreateWorld(&def);
    }

    Box2DPhysics2D::~Box2DPhysics2D()
    {
        if (impl_ && b2World_IsValid(impl_->world))
            b2DestroyWorld(impl_->world);
    }

    Box2DPhysics2D::Box2DPhysics2D(Box2DPhysics2D&&) noexcept            = default;
    Box2DPhysics2D& Box2DPhysics2D::operator=(Box2DPhysics2D&&) noexcept = default;

    Box2DPhysics2D::BodyId
    Box2DPhysics2D::createBox(
        Eigen::Vector2f center,
        float angle,
        Eigen::Vector2f half,
        EBodyType type,
        bool enabled)
    {
        const BodyId id = impl_->next++;

        b2BodyDef bd = b2DefaultBodyDef();
        bd.type      = toB2(type);
        bd.position  = b2Vec2{center.x(), center.y()};
        bd.rotation  = b2MakeRot(angle);
        bd.isEnabled = enabled;
        // The body carries its OWN BodyId so contact events map back without
        // a reverse lookup (the id fits a pointer by construction).
        bd.userData  = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        b2BodyId body = b2CreateBody(impl_->world, &bd);

        b2Polygon  box = b2MakeBox(half.x(), half.y());
        b2ShapeDef sd  = b2DefaultShapeDef();
        sd.enableContactEvents = true;   // begin-touch pairs feed beginContacts()
        b2CreatePolygonShape(body, &sd, &box);

        impl_->bodies.emplace(id, body);
        return id;
    }

    void Box2DPhysics2D::destroyBody(BodyId id)
    {
        const auto it = impl_->bodies.find(id);
        if (it == impl_->bodies.end())
            return;
        if (b2Body_IsValid(it->second))
            b2DestroyBody(it->second);
        impl_->bodies.erase(it);
    }

    void Box2DPhysics2D::setEnabled(BodyId id, bool enabled)
    {
        const b2BodyId body = impl_->find(id);
        if (!b2Body_IsValid(body))
            return;
        if (enabled)
            b2Body_Enable(body);
        else
            b2Body_Disable(body);
    }

    void Box2DPhysics2D::shiftOrigin(Eigen::Vector2f offset)
    {
        for (const auto& [_, body] : impl_->bodies)
        {
            if (!b2Body_IsValid(body))
                continue;
            const auto position = b2Body_GetPosition(body);
            b2Body_SetTransform(
                body,
                b2Vec2{position.x + offset.x(), position.y + offset.y()},
                b2Body_GetRotation(body));
        }
    }

    void Box2DPhysics2D::setLinearVelocity(BodyId id, Eigen::Vector2f v)
    {
        const b2BodyId b = impl_->find(id);
        if (b2Body_IsValid(b))
            b2Body_SetLinearVelocity(b, b2Vec2{v.x(), v.y()});
    }

    void Box2DPhysics2D::setGravityScale(BodyId id, float s)
    {
        const b2BodyId b = impl_->find(id);
        if (b2Body_IsValid(b))
            b2Body_SetGravityScale(b, s);
    }

    void Box2DPhysics2D::step(float dt, int substeps)
    {
        b2World_Step(impl_->world, dt, substeps);

        // Harvest this step's begin-touch (enter) pairs immediately — the
        // event buffers belong to the step that produced them. Shapes may
        // already be gone (destroyed during the step), hence the validity
        // guards the Box2D docs mandate.
        impl_->begin_contacts.clear();
        const b2ContactEvents events = b2World_GetContactEvents(impl_->world);
        for (int i = 0; i < events.beginCount; ++i)
        {
            const b2ContactBeginTouchEvent& ev = events.beginEvents[i];
            if (!b2Shape_IsValid(ev.shapeIdA) || !b2Shape_IsValid(ev.shapeIdB))
                continue;
            const auto id_of = [](b2ShapeId shape) -> BodyId
            {
                return static_cast<BodyId>(reinterpret_cast<std::uintptr_t>(
                    b2Body_GetUserData(b2Shape_GetBody(shape))));
            };
            impl_->begin_contacts.push_back(
                ContactPair{ id_of(ev.shapeIdA), id_of(ev.shapeIdB) });
        }
    }

    std::span<const Box2DPhysics2D::ContactPair>
    Box2DPhysics2D::beginContacts() const
    {
        return impl_->begin_contacts;
    }

    Eigen::Vector2f Box2DPhysics2D::position(BodyId id) const
    {
        const b2BodyId b = impl_->find(id);
        if (!b2Body_IsValid(b))
            return {0.0f, 0.0f};
        const b2Vec2 p = b2Body_GetPosition(b);
        return {p.x, p.y};
    }

    float Box2DPhysics2D::angle(BodyId id) const
    {
        const b2BodyId b = impl_->find(id);
        if (!b2Body_IsValid(b))
            return 0.0f;
        return b2Rot_GetAngle(b2Body_GetRotation(b));
    }

    Eigen::Vector2f Box2DPhysics2D::linearVelocity(BodyId id) const
    {
        const b2BodyId b = impl_->find(id);
        if (!b2Body_IsValid(b))
            return {0.0f, 0.0f};
        const b2Vec2 v = b2Body_GetLinearVelocity(b);
        return {v.x, v.y};
    }
} // namespace lux::ecs
