/**
 * @file pose_sampling_test.cpp
 * @brief Unit tests for lux::animation::samplePose.
 *
 * No GoogleTest dependency — uses a tiny REQUIRE macro mirroring the rest of
 * the engine's manual-test style (cf. flowforge_end_to_end_test). Returns
 * exit code 0 if every check passes, !=0 otherwise.
 */

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <Eigen/Geometry>

#include <lux/engine/animation/PoseSampling.hpp>
#include <lux/engine/description/AnimationClip.hpp>
#include <lux/engine/description/Skeleton.hpp>

namespace {
    int g_failures = 0;
    int g_checks   = 0;
}

#define REQUIRE(cond, msg)                                              \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        }                                                               \
    } while (0)

static bool approxEq(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}
static bool approxEq(const Eigen::Vector3f& a, const Eigen::Vector3f& b, float eps = 1e-5f) {
    return approxEq(a.x(), b.x(), eps) && approxEq(a.y(), b.y(), eps) && approxEq(a.z(), b.z(), eps);
}

// ---- fixture builders --------------------------------------------------------

static lux::rdesc::Skeleton makeTwoBoneSkeleton()
{
    lux::rdesc::Skeleton s;
    s.bones.resize(2);
    s.bones[0].name = "root";
    s.bones[0].parent_index   = -1;
    s.bones[0].bind_local     = Eigen::Affine3f::Identity();
    s.bones[0].inv_bind_world = Eigen::Affine3f::Identity();
    s.bones[1].name = "child";
    s.bones[1].parent_index   = 0;
    s.bones[1].bind_local     = Eigen::Affine3f::Identity();
    s.bones[1].inv_bind_world = Eigen::Affine3f::Identity();
    return s;
}

/// Single track on bone 0 with 3 translation keys at t = 0, 1, 2.
/// Translations: (0,0,0) → (1,0,0) → (2,0,0). Identity rotation+scale.
static lux::rdesc::AnimationClip makeWalkClip(bool loop)
{
    lux::rdesc::AnimationClip c;
    c.name     = "test_walk";
    c.duration = 2.0f;
    c.loop     = loop;
    c.tracks.resize(1);
    auto& tr = c.tracks[0];
    tr.bone_index   = 0;
    tr.times_t      = {0.0f, 1.0f, 2.0f};
    tr.translations = {{0,0,0}, {1,0,0}, {2,0,0}};
    tr.times_r      = {0.0f};
    tr.rotations    = {Eigen::Quaternionf::Identity()};
    tr.times_s      = {0.0f};
    tr.scales       = {{1,1,1}};
    return c;
}

// ---- tests -------------------------------------------------------------------

static void test_exact_keyframe_time()
{
    auto skel = makeTwoBoneSkeleton();
    auto clip = makeWalkClip(/*loop=*/false);
    lux::rdesc::Pose pose;

    lux::animation::samplePose(clip, skel, /*time=*/1.0f, clip.loop, pose);

    REQUIRE(pose.bone_local.size() == 2, "pose sized to skeleton");
    REQUIRE(approxEq(pose.bone_local[0].translation(), Eigen::Vector3f(1, 0, 0)),
            "exact keyframe at t=1 → translation (1,0,0)");
    REQUIRE(approxEq(pose.bone_local[1].translation(), Eigen::Vector3f(0, 0, 0)),
            "untracked bone 1 stays at bind translation");
}

static void test_midpoint_interpolation()
{
    auto skel = makeTwoBoneSkeleton();
    auto clip = makeWalkClip(/*loop=*/false);
    lux::rdesc::Pose pose;

    lux::animation::samplePose(clip, skel, /*time=*/0.5f, clip.loop, pose);
    REQUIRE(approxEq(pose.bone_local[0].translation(), Eigen::Vector3f(0.5f, 0, 0)),
            "midpoint t=0.5 → translation (0.5,0,0)");

    lux::animation::samplePose(clip, skel, /*time=*/1.25f, clip.loop, pose);
    REQUIRE(approxEq(pose.bone_local[0].translation(), Eigen::Vector3f(1.25f, 0, 0)),
            "midpoint t=1.25 → translation (1.25,0,0)");
}

static void test_past_end_loop_wraps()
{
    auto skel = makeTwoBoneSkeleton();
    auto clip = makeWalkClip(/*loop=*/true);
    lux::rdesc::Pose pose;

    // t=2.5 wraps to 0.5 → translation (0.5,0,0)
    lux::animation::samplePose(clip, skel, /*time=*/2.5f, clip.loop, pose);
    REQUIRE(approxEq(pose.bone_local[0].translation(), Eigen::Vector3f(0.5f, 0, 0)),
            "loop=true wraps t=2.5 → 0.5 → translation (0.5,0,0)");

    // t=-0.5 wraps forward to 1.5
    lux::animation::samplePose(clip, skel, /*time=*/-0.5f, clip.loop, pose);
    REQUIRE(approxEq(pose.bone_local[0].translation(), Eigen::Vector3f(1.5f, 0, 0)),
            "loop=true negative time wraps forward (-0.5 → 1.5)");
}

static void test_past_end_clamp()
{
    auto skel = makeTwoBoneSkeleton();
    auto clip = makeWalkClip(/*loop=*/false);
    lux::rdesc::Pose pose;

    // t=3 clamps to 2 → translation (2,0,0)
    lux::animation::samplePose(clip, skel, /*time=*/3.0f, clip.loop, pose);
    REQUIRE(approxEq(pose.bone_local[0].translation(), Eigen::Vector3f(2, 0, 0)),
            "loop=false clamps t=3 → 2 → last keyframe value");
}

static void test_untracked_bone_stays_at_bind()
{
    auto skel = makeTwoBoneSkeleton();
    // Give bone 1 a non-trivial bind local so we can detect it being preserved.
    skel.bones[1].bind_local =
        Eigen::Affine3f(Eigen::Translation3f(0.0f, 2.0f, 0.0f));

    auto clip = makeWalkClip(/*loop=*/false);
    lux::rdesc::Pose pose;
    lux::animation::samplePose(clip, skel, /*time=*/0.5f, clip.loop, pose);

    REQUIRE(approxEq(pose.bone_local[1].translation(), Eigen::Vector3f(0, 2, 0)),
            "bone 1 not in clip tracks → preserves bind_local translation");
}

static void test_rotation_slerp_midpoint()
{
    auto skel = makeTwoBoneSkeleton();
    lux::rdesc::AnimationClip clip;
    clip.name = "rot";
    clip.duration = 1.0f;
    clip.loop = false;
    clip.tracks.resize(1);
    auto& tr = clip.tracks[0];
    tr.bone_index   = 0;
    tr.times_t      = {0.0f};
    tr.translations = {{0,0,0}};
    tr.times_r      = {0.0f, 1.0f};
    // 0 → 90° about +Z
    tr.rotations    = {
        Eigen::Quaternionf::Identity(),
        Eigen::Quaternionf(Eigen::AngleAxisf(static_cast<float>(M_PI) * 0.5f,
                                             Eigen::Vector3f::UnitZ()))
    };
    tr.times_s      = {0.0f};
    tr.scales       = {{1,1,1}};

    lux::rdesc::Pose pose;
    lux::animation::samplePose(clip, skel, 0.5f, clip.loop, pose);

    // slerp midpoint between identity and 90°Z = 45°Z
    Eigen::Quaternionf q_expected(Eigen::AngleAxisf(static_cast<float>(M_PI) * 0.25f,
                                                    Eigen::Vector3f::UnitZ()));
    Eigen::Quaternionf q_actual(pose.bone_local[0].linear());
    // compare as rotation: dot product should be ~1 (or ~-1 for sign-flipped)
    float dot = std::fabs(q_actual.dot(q_expected));
    REQUIRE(approxEq(dot, 1.0f, 1e-4f), "slerp midpoint → 45° about Z");
}

static void test_empty_skeleton()
{
    lux::rdesc::Skeleton empty;
    lux::rdesc::AnimationClip clip;
    clip.duration = 1.0f;
    clip.loop = false;

    lux::rdesc::Pose pose;
    pose.bone_local.resize(5);   // pre-populated; must be cleared
    lux::animation::samplePose(clip, empty, 0.5f, clip.loop, pose);
    REQUIRE(pose.bone_local.empty(), "empty skeleton → empty pose");
}

int main()
{
    test_exact_keyframe_time();
    test_midpoint_interpolation();
    test_past_end_loop_wraps();
    test_past_end_clamp();
    test_untracked_bone_stays_at_bind();
    test_rotation_slerp_midpoint();
    test_empty_skeleton();

    std::printf("pose_sampling_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
