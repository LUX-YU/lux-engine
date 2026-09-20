#include <lux/engine/math/MeshBVH.hpp>
#include <lux/engine/math/Picking.hpp>

#include <Eigen/Geometry>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    const std::array positions{
        Eigen::Vector3f{-1.0F, -1.0F, 1.0F},
        Eigen::Vector3f{1.0F, -1.0F, 1.0F},
        Eigen::Vector3f{0.0F, 1.0F, 1.0F}
    };
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    lux::math::MeshBVH mesh;
    mesh.build(positions.data(), 3, indices.data(), 3);

    const lux::math::Ray ray{};
    Eigen::Matrix4f world = Eigen::Matrix4f::Identity();
    world(0, 0) = 3.0F;
    world(1, 1) = 4.0F;
    world(2, 2) = 2.0F;
    const auto hit = mesh.intersect(ray, world);
    if (!hit || std::abs(hit->t - 2.0F) > 1.0e-5F)
    {
        std::fprintf(stderr, "scaled-distance: expected=2 actual=%g\n", hit ? hit->t : -1.0F);
        return 1;
    }

    Eigen::Matrix4d distant = world.cast<double>();
    distant(0, 3) = 1.0e12;
    const lux::math::Ray3d precise{Eigen::Vector3d{1.0e12 + 0.25, 0.0, 0.0},
                                  Eigen::Vector3d::UnitZ()};
    const auto distant_hit = mesh.intersect(precise, distant);
    assert(distant_hit && std::abs(distant_hit->t - 2.0) < 1.0e-6);
    assert(distant_hit->normal.isApprox(Eigen::Vector3d::UnitZ()));

    distant(2, 2) = 0.0;
    assert(!mesh.intersect(precise, distant));

    lux::math::Ray3d projected;
    const Eigen::Matrix4d identity = Eigen::Matrix4d::Identity();
    lux::math::screenToRay(25.0, 75.0, 100.0, 100.0, identity, projected);
    assert(projected.origin.isApprox(Eigen::Vector3d{-0.5, 0.5, 0.0}));
    assert(projected.direction.isApprox(Eigen::Vector3d::UnitZ()));

    mesh.build(nullptr, 0, nullptr, 0);
    assert(!mesh.isBuilt());
    assert(mesh.triangleCount() == 0);
    assert(!mesh.intersectLocal(ray));
    std::puts("PASS spatial math: scaled distance, double origin, singular transform, unprojection, empty rebuild");
    return 0;
}
