/**
 * @file skinning_matrix_test.cpp
 * @brief Unit tests for lux::animation::buildSkinningMatrices.
 */

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <Eigen/Geometry>

#include <lux/engine/animation/PoseSampling.hpp>
#include <lux/engine/description/AnimationClip.hpp>
#include <lux/engine/description/Skeleton.hpp>

namespace
{
    int g_failures = 0;
    int g_checks = 0;
}

#define REQUIRE(cond, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);                                        \
        }                                                                                                              \
    } while (0)

static bool
approxEq(const Eigen::Matrix4f& a, const Eigen::Matrix4f& b, float eps = 1e-4f)
{
    return (a - b).cwiseAbs().maxCoeff() <= eps;
}

static void
test_identity_pose_gives_identity_matrices()
{
    lux::rdesc::Skeleton skel;
    skel.bones.resize(3);
    for (int i = 0; i < 3; ++i)
    {
        skel.bones[i].parent_index = (i == 0 ? -1 : i - 1);
        skel.bones[i].bind_local = Eigen::Affine3f::Identity();
        skel.bones[i].inv_bind_world = Eigen::Affine3f::Identity();
    }

    lux::rdesc::Pose pose;
    pose.bone_local.assign(3, Eigen::Affine3f::Identity());

    std::vector<Eigen::Matrix4f> mats;
    lux::animation::buildSkinningMatrices(pose, skel, mats);

    REQUIRE(mats.size() == 3, "matrices sized to skeleton");
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(approxEq(mats[i], Eigen::Matrix4f::Identity()), "identity-pose + identity-invbind → identity matrix");
    }
}

static void
test_two_bone_chain_root_translate_child_rotate()
{
    // Skeleton: root (bind=identity) → child (bind=identity).
    // inv_bind_world identities so we test composition cleanly.
    lux::rdesc::Skeleton skel;
    skel.bones.resize(2);
    skel.bones[0].parent_index = -1;
    skel.bones[0].bind_local = Eigen::Affine3f::Identity();
    skel.bones[0].inv_bind_world = Eigen::Affine3f::Identity();
    skel.bones[1].parent_index = 0;
    skel.bones[1].bind_local = Eigen::Affine3f::Identity();
    skel.bones[1].inv_bind_world = Eigen::Affine3f::Identity();

    // Pose: root translates by (1,0,0). Child rotates 90° about Z.
    lux::rdesc::Pose pose;
    pose.bone_local.resize(2);
    pose.bone_local[0] = Eigen::Affine3f(Eigen::Translation3f(1.0f, 0.0f, 0.0f));
    pose.bone_local[1] = Eigen::Affine3f(Eigen::AngleAxisf(static_cast<float>(M_PI) * 0.5f, Eigen::Vector3f::UnitZ()));

    std::vector<Eigen::Matrix4f> mats;
    lux::animation::buildSkinningMatrices(pose, skel, mats);

    // Expected:
    //   skin[0] = pose[0] * inv_bind[0] = T(1,0,0)
    //   skin[1] = pose[0] * pose[1] * inv_bind[1] = T(1,0,0) * R_z(90°)
    Eigen::Matrix4f expect0 = Eigen::Affine3f(Eigen::Translation3f(1, 0, 0)).matrix();
    Eigen::Matrix4f expect1 =
        (Eigen::Affine3f(Eigen::Translation3f(1, 0, 0)) *
         Eigen::Affine3f(Eigen::AngleAxisf(static_cast<float>(M_PI) * 0.5f, Eigen::Vector3f::UnitZ())))
            .matrix();

    REQUIRE(approxEq(mats[0], expect0), "root: T(1,0,0)");
    REQUIRE(approxEq(mats[1], expect1), "child world: T(1,0,0) * R_z(90°)");
}

static void
test_inv_bind_world_application()
{
    // Single bone whose bind is at world (5,0,0). inv_bind = T(-5,0,0).
    // Pose = bind → skinning matrix should be identity (no deformation).
    lux::rdesc::Skeleton skel;
    skel.bones.resize(1);
    skel.bones[0].parent_index = -1;
    skel.bones[0].bind_local = Eigen::Affine3f(Eigen::Translation3f(5, 0, 0));
    skel.bones[0].inv_bind_world = Eigen::Affine3f(Eigen::Translation3f(-5, 0, 0));

    lux::rdesc::Pose pose;
    pose.bone_local.resize(1);
    pose.bone_local[0] = skel.bones[0].bind_local; // pose == bind

    std::vector<Eigen::Matrix4f> mats;
    lux::animation::buildSkinningMatrices(pose, skel, mats);

    REQUIRE(approxEq(mats[0], Eigen::Matrix4f::Identity()), "pose==bind → skin matrix is identity (vertex undeformed)");
}

static void
test_pose_skeleton_size_mismatch_safe()
{
    // Wrong-sized pose → out_matrices should be identities (no NaN crash).
    lux::rdesc::Skeleton skel;
    skel.bones.resize(2);
    for (auto& b : skel.bones)
    {
        b.parent_index = -1;
        b.bind_local = Eigen::Affine3f::Identity();
        b.inv_bind_world = Eigen::Affine3f::Identity();
    }
    skel.bones[1].parent_index = 0;

    lux::rdesc::Pose pose;
    pose.bone_local.resize(5); // wrong size

    std::vector<Eigen::Matrix4f> mats;
    lux::animation::buildSkinningMatrices(pose, skel, mats);

    REQUIRE(mats.size() == 2, "matrices sized to skeleton even on mismatch");
    REQUIRE(approxEq(mats[0], Eigen::Matrix4f::Identity()), "mismatch → identity fallback (no NaN)");
    REQUIRE(approxEq(mats[1], Eigen::Matrix4f::Identity()), "mismatch → identity fallback (no NaN)");
}

static void
test_global_transform_applies_to_root_and_propagates()
{
    // skeleton.global_transform is the armature prefix. With T(10,0,0),
    // a two-bone chain (all binds/inv-binds identity, identity pose) should give
    // root skin = T, and child skin = T (propagated through the parent world).
    lux::rdesc::Skeleton skel;
    skel.bones.resize(2);
    skel.bones[0].parent_index = -1;
    skel.bones[0].bind_local = Eigen::Affine3f::Identity();
    skel.bones[0].inv_bind_world = Eigen::Affine3f::Identity();
    skel.bones[1].parent_index = 0;
    skel.bones[1].bind_local = Eigen::Affine3f::Identity();
    skel.bones[1].inv_bind_world = Eigen::Affine3f::Identity();
    skel.global_transform = Eigen::Affine3f(Eigen::Translation3f(10, 0, 0));

    lux::rdesc::Pose pose;
    pose.bone_local.assign(2, Eigen::Affine3f::Identity());

    std::vector<Eigen::Matrix4f> mats;
    lux::animation::buildSkinningMatrices(pose, skel, mats);

    const Eigen::Matrix4f expect = Eigen::Affine3f(Eigen::Translation3f(10, 0, 0)).matrix();
    REQUIRE(approxEq(mats[0], expect), "global_transform applies to root bone");
    REQUIRE(approxEq(mats[1], expect), "global_transform propagates to child via parent");
}

static void
test_global_transform_identity_is_noop()
{
    // Default/identity global_transform must reproduce legacy behavior exactly:
    // pose==bind with mirrored inv_bind → identity skin (vertex undeformed).
    lux::rdesc::Skeleton skel;
    skel.bones.resize(1);
    skel.bones[0].parent_index = -1;
    skel.bones[0].bind_local = Eigen::Affine3f(Eigen::Translation3f(5, 0, 0));
    skel.bones[0].inv_bind_world = Eigen::Affine3f(Eigen::Translation3f(-5, 0, 0));
    skel.global_transform = Eigen::Affine3f::Identity();

    lux::rdesc::Pose pose;
    pose.bone_local.resize(1);
    pose.bone_local[0] = skel.bones[0].bind_local; // pose == bind

    std::vector<Eigen::Matrix4f> mats;
    lux::animation::buildSkinningMatrices(pose, skel, mats);

    REQUIRE(
        approxEq(mats[0], Eigen::Matrix4f::Identity()),
        "identity global_transform → no regression (pose==bind yields identity)"
    );
}

int
main()
{
    test_identity_pose_gives_identity_matrices();
    test_two_bone_chain_root_translate_child_rotate();
    test_inv_bind_world_application();
    test_pose_skeleton_size_mismatch_safe();
    test_global_transform_applies_to_root_and_propagates();
    test_global_transform_identity_is_noop();

    std::printf("skinning_matrix_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
