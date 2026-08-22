// ============================================================================
//  scene_view_integration_test.cpp
//
//  Integration test: Scene rendering with ImGui UI controls.
//    - Two offscreen views of one scene (main viewport + secondary panel)
//    - Deferred rendering with shadows, point clouds, grid, skybox, tonemap
//    - ImGui panels to control mesh transform, lights, point cloud gen/remove
//    - Uses UIRenderServer + UIRenderFrameSession (ImGui + content scene)
//
//  Requires a real GPU (Vulkan).  Opens a window; press ESC or close to exit.
// ============================================================================

// ── UI / ImGui ──────────────────────────────────────────────────────────
#include <lux/engine/ui/UISystem.hpp>
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/SceneViewElement.hpp>
#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/UIRenderFrameSession.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>

// ── Render comm ─────────────────────────────────────────────────────────
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/render/testing/DirectRenderUploadClient.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>

// ── Feature operations ──────────────────────────────────────────────────
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HzbOperation.ops.hpp>   // kHzbFeatureFactory / HzbCommTag
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>   // LightFeature / LightProxy / LightDescriptor
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp> // kMaterialFeatureFactory
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp> // kMeshStackFeatureFactory
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/features/shadow/ShadowQualityParams.hpp>
#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp>   // FeatureParamsProxy
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/PointCloudOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>

// ── Resource descriptions ───────────────────────────────────────────────
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>

// Eigen geometry — Vector3f::cross() (buildViewMatrix)
#include <Eigen/Geometry>

// ── Graph material (W5a: rdesc::Material retired; materials are graph materials) ──
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include "graph_test_helpers.hpp"   // lux::mgtest::makeColorGraph + compileGraphPass
#include <span>
#include <string>

// ── Asset loading ───────────────────────────────────────────────────────
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/texture/TextureSerDeser.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

// ── Window ──────────────────────────────────────────────────────────────
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <GLFW/glfw3.h>

// ── Generated paths ─────────────────────────────────────────────────────
#include <test_time_asset_path.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace lux::render;
namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
//  Constants
// ═══════════════════════════════════════════════════════════════════════════

static constexpr uint32_t kWidth      = 1280;
static constexpr uint32_t kHeight     = 720;
static constexpr float    kPi         = 3.14159265f;
static constexpr float    kCubeHalf   = 0.4f;
static constexpr float    kFloorHalf  = 15.0f;
static constexpr float    kCamRadius  = 15.0f;
static constexpr float    kCamHeight  = 10.0f;
static constexpr float    kCamSpeed   = 0.3f;
static constexpr uint32_t kPCPointsPerChunk = 4096;

// ═══════════════════════════════════════════════════════════════════════════
//  Shared mutable state — written by ControlPanel, read by main loop
// ═══════════════════════════════════════════════════════════════════════════

struct SceneState
{
    // Mesh
    float cube_pos[3]     = {0.f, 2.5f, 0.f};
    float cube_rot_deg[3] = {0.f, 0.f, 0.f};

    // Point lights
    float pl1_pos[3]   = {5.f, 3.f, 0.f};
    float pl1_color[3] = {1.f, 0.8f, 0.5f};
    float pl1_intensity = 2.0f;

    float pl2_pos[3]   = {-5.f, 3.f, 0.f};
    float pl2_color[3] = {0.5f, 0.8f, 1.f};
    float pl2_intensity = 2.0f;

    // Point cloud actions (set by button clicks, cleared after handling)
    bool     gen_chunk      = false;
    bool     remove_last    = false;
    bool     clear_all      = false;
    uint32_t chunk_count    = 0;
    uint64_t total_points   = 0;

    // Directional shadow mode (global for this test scene)
    bool directional_csm_enabled   = true; // default ON
    bool directional_csm_supported = false; // set after ShadowMap op IDs are known

    // Shadow quality controls
    bool shadow_quality_supported = false;
    int shadow_quality_preset = 2; // 0:Low 1:Medium 2:High 3:Ultra
    uint32_t shadow_atlas_page_resolution = 2048;
    uint32_t shadow_atlas_page_count = 2;
    uint32_t shadow_max_slices = 64;
    float shadow_max_distance = 60.0f;
    bool shadow_quality_dirty = false;
};

struct ShadowQualityPresetConfig
{
    const char* name;
    uint32_t atlas_page_resolution;
    uint32_t atlas_page_count;
    uint32_t max_shadow_slices;
    float max_distance;
};

static constexpr std::array<ShadowQualityPresetConfig, 4> kShadowQualityPresets = {{
    {"Low", 512u, 1u, 16u, 20.0f},
    {"Medium", 1024u, 2u, 32u, 40.0f},
    {"High", 2048u, 2u, 64u, 60.0f},
    {"Ultra", 4096u, 4u, 128u, 100.0f},
}};

static void applyShadowPreset(SceneState& state, int preset_index)
{
    const int clamped = std::clamp(preset_index, 0, static_cast<int>(kShadowQualityPresets.size()) - 1);
    const auto& preset = kShadowQualityPresets[static_cast<size_t>(clamped)];
    state.shadow_quality_preset = clamped;
    state.shadow_atlas_page_resolution = preset.atlas_page_resolution;
    state.shadow_atlas_page_count = preset.atlas_page_count;
    state.shadow_max_slices = preset.max_shadow_slices;
    state.shadow_max_distance = preset.max_distance;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Geometry helpers  (from deferred_stress_test)
// ═══════════════════════════════════════════════════════════════════════════

static void buildFloorPlane(std::vector<lux::rdesc::Vertex> &verts,
                            std::vector<uint32_t> &indices,
                            float half_extent)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;

    V3 n(0, 1, 0), t(1, 0, 0), bt(0, 0, 1);
    float uv_scale = half_extent / 2.0f;
    V3 corners[4] = {
        {-half_extent, 0, -half_extent}, { half_extent, 0, -half_extent},
        { half_extent, 0,  half_extent}, {-half_extent, 0,  half_extent}
    };
    V2 uvs[4] = {{0, 0}, {uv_scale, 0}, {uv_scale, uv_scale}, {0, uv_scale}};

    auto base = static_cast<uint32_t>(verts.size());
    for (int i = 0; i < 4; ++i)
        verts.push_back({corners[i], n, t, uvs[i], bt});
    indices.insert(indices.end(), {base, base+2, base+1, base, base+3, base+2});
}

static void buildCube(std::vector<lux::rdesc::Vertex> &verts,
                      std::vector<uint32_t> &indices,
                      float half)
{
    using V3 = Eigen::Vector3f;
    using V2 = Eigen::Vector2f;

    struct FaceInfo { V3 n, t, bt; V3 corners[4]; };
    FaceInfo faces[6] = {
        {{ 0, 0, 1},{ 1, 0, 0},{0, 1, 0}, {{-half,-half, half},{ half,-half, half},{ half, half, half},{-half, half, half}}},
        {{ 0, 0,-1},{-1, 0, 0},{0, 1, 0}, {{ half,-half,-half},{-half,-half,-half},{-half, half,-half},{ half, half,-half}}},
        {{ 1, 0, 0},{ 0, 0,-1},{0, 1, 0}, {{ half,-half, half},{ half,-half,-half},{ half, half,-half},{ half, half, half}}},
        {{-1, 0, 0},{ 0, 0, 1},{0, 1, 0}, {{-half,-half,-half},{-half,-half, half},{-half, half, half},{-half, half,-half}}},
        {{ 0, 1, 0},{ 1, 0, 0},{0, 0, 1}, {{-half, half, half},{ half, half, half},{ half, half,-half},{-half, half,-half}}},
        {{ 0,-1, 0},{ 1, 0, 0},{0, 0,-1}, {{-half,-half,-half},{ half,-half,-half},{ half,-half, half},{-half,-half, half}}},
    };
    V2 uvs[4] = {{0,0},{1,0},{1,1},{0,1}};

    for (auto &f : faces) {
        auto base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < 4; ++i)
            verts.push_back({f.corners[i], f.n, f.t, uvs[i], f.bt});
        indices.insert(indices.end(), {base, base+1, base+2, base, base+2, base+3});
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Matrix helpers
// ═══════════════════════════════════════════════════════════════════════════

static void setTranslation(float m[16], float x, float y, float z)
{
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.f;
    m[12] = x; m[13] = y; m[14] = z;
}

/// Build column-major 4×4: Rz * Ry * Rx * T  (euler XYZ in radians + translation)
static void setTransformEuler(float m[16], float x, float y, float z,
                              float rx, float ry, float rz)
{
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    // Column-major rotation = Rz * Ry * Rx
    m[0]  =  cz * cy;
    m[1]  =  sz * cy;
    m[2]  = -sy;
    m[3]  =  0.f;

    m[4]  =  cz * sy * sx - sz * cx;
    m[5]  =  sz * sy * sx + cz * cx;
    m[6]  =  cy * sx;
    m[7]  =  0.f;

    m[8]  =  cz * sy * cx + sz * sx;
    m[9]  =  sz * sy * cx - cz * sx;
    m[10] =  cy * cx;
    m[11] =  0.f;

    m[12] =  x;
    m[13] =  y;
    m[14] =  z;
    m[15] =  1.f;
}

static Eigen::Matrix4f buildViewMatrix(const Eigen::Vector3f &eye,
                                       const Eigen::Vector3f &target,
                                       const Eigen::Vector3f &up)
{
    Eigen::Vector3f f = (target - eye).normalized();
    Eigen::Vector3f s = f.cross(up).normalized();
    Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f V = Eigen::Matrix4f::Identity();
    V(0,0) = s.x(); V(0,1) = s.y(); V(0,2) = s.z(); V(0,3) = -s.dot(eye);
    V(1,0) = u.x(); V(1,1) = u.y(); V(1,2) = u.z(); V(1,3) = -u.dot(eye);
    V(2,0) = -f.x(); V(2,1) = -f.y(); V(2,2) = -f.z(); V(2,3) = f.dot(eye);
    return V;
}

static Eigen::Matrix4f buildProjMatrix(float fov_rad, float aspect,
                                       float near_z, float far_z)
{
    float tanHalf = std::tan(fov_rad * 0.5f);
    Eigen::Matrix4f P = Eigen::Matrix4f::Zero();
    P(0,0) =  1.f / (aspect * tanHalf);
    P(1,1) = -1.f / tanHalf;          // Vulkan Y-down
    P(2,2) = -far_z / (far_z - near_z);
    P(2,3) = -(far_z * near_z) / (far_z - near_z);
    P(3,2) = -1.f;
    return P;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Random point cloud chunk generation
// ═══════════════════════════════════════════════════════════════════════════

static std::vector<PointCloudPoint> generateRandomChunk(uint32_t chunk_id,
                                                       std::mt19937 &rng)
{
    std::uniform_real_distribution<float> pos(-8.f, 8.f);
    std::uniform_real_distribution<float> height(0.1f, 6.f);
    std::uniform_real_distribution<float> col(0.f, 1.f);

    std::vector<PointCloudPoint> pts;
    pts.reserve(kPCPointsPerChunk);
    // Cluster around a random center
    float cx = pos(rng), cz = pos(rng);
    std::normal_distribution<float> spread(0.f, 1.5f);
    for (uint32_t i = 0; i < kPCPointsPerChunk; ++i)
    {
        float px = cx + spread(rng);
        float py = height(rng);
        float pz = cz + spread(rng);
        float r = 0.2f + 0.8f * (py / 6.f);
        float g = 0.6f + 0.4f * col(rng);
        float b = 0.3f;
        pts.push_back(PointCloudPoint::make(px, py, pz, r, g, b));
    }
    return pts;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Panels
// ═══════════════════════════════════════════════════════════════════════════

/// Main 3D viewport — displays the orbiting camera view
class MainViewportPanel : public lux::ui::Panel
{
public:
    MainViewportPanel()
        : Panel("Main Viewport", {800, 600})
    {}

    lux::ui::SceneViewElement& sceneView() { return scene_view_; }

protected:
    void paint() override
    {
        scene_view_.draw();
    }

private:
    lux::ui::SceneViewElement scene_view_{"MainSceneView"};
};

/// Secondary 3D viewport — displays a different camera angle
class SecondaryViewPanel : public lux::ui::Panel
{
public:
    SecondaryViewPanel()
        : Panel("Secondary View", {400, 300})
    {}

    lux::ui::SceneViewElement& sceneView() { return scene_view_; }

protected:
    void paint() override
    {
        scene_view_.draw();
    }

private:
    lux::ui::SceneViewElement scene_view_{"SecondarySceneView"};
};

/// Control panel — ImGui widgets for mesh, lights, point clouds
class ControlPanel : public lux::ui::Panel
{
public:
    explicit ControlPanel(SceneState &state)
        : Panel("Scene Controls", {350, 600})
        , state_(state)
    {}

protected:
    void paint() override
    {
        const ImGuiIO &io = ImGui::GetIO();
        ImGui::Text("%.1f FPS (%.3f ms/frame)",
            static_cast<double>(io.Framerate),
            1000.0 / static_cast<double>(io.Framerate));
        ImGui::Separator();

        // ── Mesh transform ──────────────────────────────────────────
        if (ImGui::CollapsingHeader("Mesh Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat3("Position##mesh", state_.cube_pos, -10.f, 10.f);
            ImGui::SliderFloat3("Rotation (deg)##mesh", state_.cube_rot_deg, -180.f, 180.f);
            if (ImGui::Button("Reset Mesh"))
            {
                state_.cube_pos[0] = 0.f; state_.cube_pos[1] = 0.5f; state_.cube_pos[2] = 0.f;
                state_.cube_rot_deg[0] = state_.cube_rot_deg[1] = state_.cube_rot_deg[2] = 0.f;
            }
        }

        // ── Point Light 1 ───────────────────────────────────────────
        if (ImGui::CollapsingHeader("Point Light 1", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat3("Position##pl1", state_.pl1_pos, -20.f, 20.f);
            ImGui::ColorEdit3("Color##pl1", state_.pl1_color);
            ImGui::SliderFloat("Intensity##pl1", &state_.pl1_intensity, 0.f, 10.f);
        }

        // ── Point Light 2 ───────────────────────────────────────────
        if (ImGui::CollapsingHeader("Point Light 2", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat3("Position##pl2", state_.pl2_pos, -20.f, 20.f);
            ImGui::ColorEdit3("Color##pl2", state_.pl2_color);
            ImGui::SliderFloat("Intensity##pl2", &state_.pl2_intensity, 0.f, 10.f);
        }

        // ── Point Cloud ─────────────────────────────────────────────
        if (ImGui::CollapsingHeader("Point Cloud", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Chunks: %u   Points: %llu",
                state_.chunk_count,
                static_cast<unsigned long long>(state_.total_points));
            if (ImGui::Button("Generate Chunk"))
                state_.gen_chunk = true;
            ImGui::SameLine();
            if (ImGui::Button("Remove Last") && state_.chunk_count > 0)
                state_.remove_last = true;
            if (ImGui::Button("Clear All") && state_.chunk_count > 0)
                state_.clear_all = true;
        }

        // ── Animated rainbow image ─────────────────────────────────
        if (ImGui::CollapsingHeader("Rainbow Image", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (io.Fonts && io.Fonts->TexID)
            {
                const float t = static_cast<float>(ImGui::GetTime());
                auto rainbow = [t](float phase) {
                    return 0.5f + 0.5f * std::sin(1.6f * t + phase);
                };

                const ImVec4 tint_col{
                    rainbow(0.0f),
                    rainbow(2.0943951f),
                    rainbow(4.1887902f),
                    1.0f
                };

                const ImVec2 uv = io.Fonts->TexUvWhitePixel;
                ImGui::Image(
                    io.Fonts->TexID,
                    ImVec2(240.0f, 90.0f),
                    uv,
                    uv,
                    tint_col,
                    ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::Text("RGB: %.2f  %.2f  %.2f", tint_col.x, tint_col.y, tint_col.z);
            }
            else
            {
                ImGui::TextDisabled("Font texture is unavailable.");
            }
        }

        // ── Shadow ─────────────────────────────────────────────────
        if (ImGui::CollapsingHeader("Shadow", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (state_.shadow_quality_supported)
            {
                static constexpr const char* kPresetNames[] = {"Low", "Medium", "High", "Ultra"};
                int preset_index = std::clamp(state_.shadow_quality_preset, 0, static_cast<int>(kShadowQualityPresets.size()) - 1);
                if (ImGui::Combo(
                        "Quality Preset##shadow",
                        &preset_index,
                        kPresetNames,
                        IM_ARRAYSIZE(kPresetNames)))
                {
                    applyShadowPreset(state_, preset_index);
                    state_.shadow_quality_dirty = true;
                }

                if (ImGui::SliderFloat(
                        "Shadow Max Distance##shadow",
                        &state_.shadow_max_distance,
                        0.0f,
                        200.0f,
                        "%.1f"))
                {
                    state_.shadow_quality_dirty = true;
                }

                if (ImGui::CollapsingHeader("Advanced##shadow"))
                {
                    static constexpr uint32_t kAtlasResValues[] = {512u, 1024u, 2048u, 4096u, 8192u};
                    static constexpr const char* kAtlasResLabels[] = {"512", "1024", "2048", "4096", "8192"};

                    int atlas_res_index = 0;
                    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kAtlasResValues)); ++i)
                    {
                        if (state_.shadow_atlas_page_resolution == kAtlasResValues[i])
                        {
                            atlas_res_index = i;
                            break;
                        }
                    }
                    if (ImGui::Combo(
                            "Atlas Page Resolution##shadow",
                            &atlas_res_index,
                            kAtlasResLabels,
                            IM_ARRAYSIZE(kAtlasResLabels)))
                    {
                        state_.shadow_atlas_page_resolution = kAtlasResValues[atlas_res_index];
                        state_.shadow_quality_dirty = true;
                    }

                    int atlas_page_count = static_cast<int>(state_.shadow_atlas_page_count);
                    if (ImGui::SliderInt("Atlas Page Count##shadow", &atlas_page_count, 1, 8))
                    {
                        state_.shadow_atlas_page_count = static_cast<uint32_t>(std::max(atlas_page_count, 1));
                        state_.shadow_quality_dirty = true;
                    }

                    int max_shadow_slices = static_cast<int>(state_.shadow_max_slices);
                    if (ImGui::SliderInt("Max Shadow Slices##shadow", &max_shadow_slices, 1, 512))
                    {
                        state_.shadow_max_slices = static_cast<uint32_t>(std::max(max_shadow_slices, 1));
                        state_.shadow_quality_dirty = true;
                    }

                    if (ImGui::SliderFloat(
                            "Shadow Max Distance (Advanced)##shadow",
                            &state_.shadow_max_distance,
                            0.0f,
                            200.0f,
                            "%.1f"))
                    {
                        state_.shadow_quality_dirty = true;
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("Shadow quality update unsupported (missing op).");
            }

            if (!state_.directional_csm_supported)
            {
                ImGui::BeginDisabled();
                ImGui::Checkbox("Directional CSM##shadow", &state_.directional_csm_enabled);
                ImGui::EndDisabled();
                ImGui::TextDisabled("Directional CSM toggle unsupported (missing op).");
            }
            else
            {
                ImGui::Checkbox("Directional CSM##shadow", &state_.directional_csm_enabled);
                ImGui::Text("Mode: %s",
                    state_.directional_csm_enabled ? "CSM ON" : "CSM OFF");
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Press ESC to exit, V to swap view update order, 5 to toggle CSM.");
    }

private:
    SceneState &state_;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Frame pump helpers
// ═══════════════════════════════════════════════════════════════════════════

/// Begin recording a frame — issue session commands after this.
static void openFrame(lux::ui::UIRenderFrameSession &session,
                      lux::ui::UISystem &ui)
{
    lux::window::LuxWindow::pollEvents();
    ui.newFrame();
    while (!session.beginFrame())
    {
        // Drain any pending submission so beginFrame can succeed
        session.trySubmitFrame();
        session.pumpReplies();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lux::window::LuxWindow::pollEvents();
    }
}

/// End recording — submit ImGui draw data + frame, pump replies.
static void closeFrame(lux::ui::UIRenderFrameSession &session)
{
    auto *dd = ImGui::GetDrawData();
    session.submitImGuiDrawData(RenderSceneId{}, dd);
    session.trySubmitFrame();
    session.pumpReplies();
}

/// One empty frame (no user commands).
static void pumpFrame(lux::ui::UIRenderFrameSession &session,
                      lux::ui::UISystem &ui)
{
    openFrame(session, ui);
    closeFrame(session);
}

/// Pump frames until a pending request becomes ready.
template<typename T>
static T waitReady(lux::ui::UIRenderFrameSession &session,
                   RenderRequest<T> req,
                   lux::ui::UISystem &ui)
{
    while (!req.isReady())
        pumpFrame(session, ui);
    return req.tryResult()->get();
}

template<typename T>
static T waitReady(
    RenderControlSession& control,
    RenderRequest<T> request,
    lux::ui::UISystem&
)
{
    while (!request.isReady())
        if (!control.waitAndPumpReplies())
            break;
    return request.tryResult()->get();
}

template<typename T>
static T waitReady(
    RenderUploadSession& upload,
    RenderRequest<T> request,
    lux::ui::UISystem&
)
{
    while (!request.isReady())
        if (!upload.waitAndPumpReplies())
            break;
    return request.tryResult()->get();
}

template<typename T>
static T waitReady(
    RenderUploadSession& upload,
    UploadSubmitResult<T> submitted,
    lux::ui::UISystem& ui
)
{
    if (!submitted)
        return {};
    return waitReady(upload, std::move(submitted.value()), ui);
}

// ═══════════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════════

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== Scene View Integration Test ===\n");
    std::printf("Two offscreen views + ImGui UI controls.\n");
    std::printf("Press ESC or close window to exit.\n\n");

    fs::path asset_dir(asset_path);

    // ── 1. Window + UISystem ────────────────────────────────────────
    lux::window::GlfwRuntime glfw;
    if (!glfw.valid()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }

    lux::window::LuxWindow window(kWidth, kHeight,
        "Scene View Integration Test");

    SceneState state;
    MainViewportPanel   main_vp;
    SecondaryViewPanel  sec_vp;
    ControlPanel        ctrl_panel(state);

    lux::ui::UISystem ui(window);
    auto main_registration = ui.registerPanel(main_vp);
    auto secondary_registration = ui.registerPanel(sec_vp);
    auto control_registration = ui.registerPanel(ctrl_panel);
    if (!main_registration || !secondary_registration || !control_registration)
        return 1;

    // ── 2. Communication channel + render thread ────────────────────
    auto channel = RenderFrameChannel<>::create();
    auto control_channel = RenderControlChannel<>::create();
    auto upload_channel = RenderUploadChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();

    lux::ui::ImGuiOperationIds imgui_ops{};
    std::atomic<bool> server_running{false};

    std::thread render_thread([&] {
        auto server = std::make_unique<lux::ui::UIRenderServer>(
            channel, control_channel, upload_channel, sync);

        lux::render::ServerConfig cfg;
        cfg.enable_validation = true;
        for (auto *ext : lux::ui::UISystem::requiredVulkanExtensions())
            cfg.instance_extensions.emplace_back(ext);

        lux::ui::ImGuiCommConfig imgui_cfg{};
        imgui_cfg.color_format = lux::render::ETextureFormatHint::SBGRA8;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(
            &imgui_cfg.font_pixels, &imgui_cfg.font_width, &imgui_cfg.font_height);

        if (auto r = server->init(std::move(cfg), imgui_cfg); !r)
        {
            std::fprintf(stderr, "UIRenderServer::init() failed\n");
            return;
        }
        if (auto r = server->attachToWindow(window); !r)
        {
            std::fprintf(stderr, "UIRenderServer::attachToWindow() failed\n");
            return;
        }

        imgui_ops = server->imguiOps();
        server_running.store(true, std::memory_order_release);

        while (server->tick()) {}
        server_running.store(false, std::memory_order_release);
    });

    while (!server_running.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto session = std::make_unique<lux::ui::UIRenderFrameSession>(channel, sync, imgui_ops);
    RenderControlSession control(control_channel, sync);
    RenderUploadSession upload(upload_channel, sync);
    lux::render::testing::DirectRenderUploadClient upload_client{upload};


    std::printf("  Server running. Session created.\n");

    // ── 3. Create content scene ─────────────────────────────────────
    openFrame(*session, ui);
    auto scene_req = control.createScene("IntegrationScene");
    closeFrame(*session);
    auto scene_reply = waitReady(control, std::move(scene_req), ui);
    auto scene_id = scene_reply.scene_id;
    std::printf("  Scene created (id=%u)\n", scene_id.index);

    openFrame(*session, ui);
    auto active_req = control.setActiveScene(scene_id, true);
    closeFrame(*session);
    waitReady(control, std::move(active_req), ui);

    // ── 4. Create two offscreen views + SAMPLED targets ─────────────
    Eigen::Vector3f eye1(kCamRadius, kCamHeight, 0.f);
    Eigen::Vector3f target(0.f, 1.f, 0.f);
    Eigen::Vector3f up(0.f, 1.f, 0.f);
    Eigen::Matrix4f V1 = buildViewMatrix(eye1, target, up);
    Eigen::Matrix4f P1 = buildProjMatrix(60.f * kPi / 180.f, 800.f / 600.f, 0.1f, 200.f);

    // RT 一等化:视图 + 显式 SAMPLED 目标 + setLayer(原 addUIView 命令面)。
    openFrame(*session, ui);
    auto v1_req = control.addView(scene_id, {800, 600}, "MainView");
    auto t1_req = control.createOffscreenRenderTarget(
        {800, 600}, lux::render::kTargetFlagSampled);
    closeFrame(*session);
    auto view1 = waitReady(control, std::move(v1_req), ui).view;
    auto target1 = waitReady(control, std::move(t1_req), ui).target;
    openFrame(*session, ui);
    control.setLayer(target1, 0, scene_id, view1);
    closeFrame(*session);
    std::printf("  View 1 (main, rotating camera) created: handle=%u\n", view1.index);
    (void)V1; (void)P1;   // camera matrices are pushed per-frame via ViewCameraProxy below

    // Secondary view — top-down angled
    Eigen::Vector3f eye2(0.f, 20.f, 10.f);
    Eigen::Matrix4f V2 = buildViewMatrix(eye2, target, up);
    Eigen::Matrix4f P2 = buildProjMatrix(60.f * kPi / 180.f, 400.f / 300.f, 0.1f, 200.f);

    openFrame(*session, ui);
    auto v2_req = control.addView(scene_id, {400, 300}, "SecondaryView");
    auto t2_req = control.createOffscreenRenderTarget(
        {400, 300}, lux::render::kTargetFlagSampled);
    closeFrame(*session);
    auto view2 = waitReady(control, std::move(v2_req), ui).view;
    auto target2 = waitReady(control, std::move(t2_req), ui).target;
    openFrame(*session, ui);
    control.setLayer(target2, 0, scene_id, view2);
    closeFrame(*session);
    std::printf("  View 2 (secondary, fixed camera) created: handle=%u\n", view2.index);
    std::printf("  Camera update order default: rotating(view1=%u) -> fixed(view2=%u)\n",
        view1.index, view2.index);
    (void)V2; (void)P2;

    // Wire sentinel texture IDs to SceneViewElements
    main_vp.sceneView().setTextureID(
        lux::ui::encodeRenderTargetSentinel(target1));
    sec_vp.sceneView().setTextureID(
        lux::ui::encodeRenderTargetSentinel(target2));

    // Resize callbacks — defer actual resizeTarget to in-frame processing
    struct PendingResize { uint32_t w{0}, h{0}; bool pending{false}; };
    PendingResize pending_resize_v1, pending_resize_v2;
    main_vp.sceneView().setResizeCallback([&](uint32_t w, uint32_t h) {
        pending_resize_v1 = {w, h, true};
    });
    sec_vp.sceneView().setResizeCallback([&](uint32_t w, uint32_t h) {
        pending_resize_v2 = {w, h, true};
    });

    // ── 5. Register feature types (batched in one frame) ───────────
    openFrame(*session, ui);
    auto view_cam_req = control.registerFeatureType(kViewCameraFeatureFactory);
    auto gbuf_req   = control.registerFeatureType(kDeferredGBufferFeatureFactory);
    auto lit_req    = control.registerFeatureType(kDeferredLightingFeatureFactory);
    auto light_req  = control.registerFeatureType(kLightFeatureFactory);
    auto material_req = control.registerFeatureType(kMaterialFeatureFactory);
    auto mesh_stack_req = control.registerFeatureType(kMeshStackFeatureFactory);
    auto shmap_req  = control.registerFeatureType(kShadowMapFeatureFactory);
    auto mshsw_req  = control.registerFeatureType(kMeshShadowFeatureFactory);
    auto pc_req     = control.registerFeatureType(kPCFeatureSimpleFactory);
    auto grid_req   = control.registerFeatureType(kGrid3DFeatureFactory);
    auto sky_req    = control.registerFeatureType(kSkyboxFeatureFactory);
    auto tm_req0    = control.registerFeatureType(kTonemapFeatureFactory);
    auto hzb_req    = control.registerFeatureType(kHzbFeatureFactory);
    closeFrame(*session);

    auto view_cam_reg = waitReady(control, std::move(view_cam_req), ui);
    auto gbuf_type  = waitReady(control, std::move(gbuf_req), ui);
    auto lit_type   = waitReady(control, std::move(lit_req), ui);
    auto light_reg  = waitReady(control, std::move(light_req), ui);
    auto material_reg = waitReady(control, std::move(material_req), ui);
    auto mesh_stack_reg = waitReady(control, std::move(mesh_stack_req), ui);
    auto shmap_type = waitReady(control, std::move(shmap_req), ui);
    auto mshsw_type = waitReady(control, std::move(mshsw_req), ui);
    auto pc_type    = waitReady(control, std::move(pc_req), ui);
    auto grid_type  = waitReady(control, std::move(grid_req), ui);
    auto sky_type   = waitReady(control, std::move(sky_req), ui);
    auto tm_type    = waitReady(control, std::move(tm_req0), ui);
    auto hzb_type   = waitReady(control, std::move(hzb_req), ui);

    ViewCameraOperationIds view_cam_ops = ViewCameraOperationIds::fromOps(view_cam_reg.ops, view_cam_reg.op_count);
    const TypeId shadow_params_op = shmap_type.op_count > 0 ? shmap_type.ops[0] : kInvalidTypeId;
    auto skybox_ops = SkyboxOperationIds::fromOps(sky_type.ops, sky_type.op_count);
    auto pc_ops     = PointCloudOperationIds::fromOps(pc_type.ops, pc_type.op_count);
    LightOperationIds light_ops = LightOperationIds::fromOps(light_reg.ops, light_reg.op_count);
    MeshStackOperationIds mesh_stack_ops = MeshStackOperationIds::fromOps(mesh_stack_reg.ops, mesh_stack_reg.op_count);
    MaterialOperationIds material_ops = MaterialOperationIds::fromOps(material_reg.ops, material_reg.op_count);

    std::printf("  Feature types registered: 8/8\n");
    applyShadowPreset(state, 2); // High preset by default.
    state.shadow_quality_supported = (shadow_params_op != kInvalidTypeId);
    state.shadow_quality_dirty = false;
    state.directional_csm_supported = (shadow_params_op != kInvalidTypeId);
    state.directional_csm_enabled   = true;
    std::printf("  Directional CSM toggle: %s (default: ON)\n",
        state.directional_csm_supported ? "available" : "unsupported");
    std::printf("  Shadow quality update: %s (default preset: %s, maxDist=%.1f)\n",
        state.shadow_quality_supported ? "available" : "unsupported",
        kShadowQualityPresets[static_cast<size_t>(state.shadow_quality_preset)].name,
        state.shadow_max_distance);

    // ── 6. Add features (batched in one frame) ─────────────────────
    ShadowMapCommConfig smc{};
    smc.atlas_page_resolution  = state.shadow_atlas_page_resolution;
    smc.atlas_page_count       = state.shadow_atlas_page_count;
    smc.max_shadow_slices      = state.shadow_max_slices;
    smc.enable_directional_csm = state.directional_csm_enabled ? 1u : 0u;
    smc.non_directional_shadow_max_distance = state.shadow_max_distance;

    MeshShadowCommConfig msmc{};
    msmc.comm_config_version = kMeshShadowCommConfigVersion;
    msmc.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;

    DeferredGBufferCommConfig gbcc{};
    gbcc.comm_config_version = kDeferredGBufferCommConfigVersion;
    gbcc.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
    // HZB occlusion culling ON. This test is the engine's only TWO-VIEW-ONE-SCENE
    // configuration with DIFFERENT extents (800x600 + 400x300), which is exactly
    // what per-view HZB has to get right: each view must cull against its OWN
    // pyramid at its OWN size. With a scene-wide pyramid both views culled
    // against whichever one was recorded last, at the first view's size.
    gbcc.extension_flags |= EGpuDrivenMeshExt::HZB;

    DeferredLightingCommConfig dlcc{};
    dlcc.read_mode       = lux::render::ELightingReadMode::SAMPLED;
    dlcc.enable_clustered = 1;
    dlcc.cluster_x = 16;
    dlcc.cluster_y = 9;
    dlcc.cluster_z = 24;
    dlcc.max_cluster_indices = 1'048'576;

    PCSimpleCommConfig pcsc{};
    pcsc.initial_point_size = 3.0f;
    pcsc.max_global_points  = 4'000'000;
    pcsc.max_octree_nodes   = 256;

    Grid3DCommConfig gcc{};

    SkyboxCommConfig skcc{};

    TonemapCommConfig tcc{};
    tcc.tone_map_op     = lux::render::ETonemapOperator::ACES_FILMIC;
    tcc.exposure        = 1.0f;
    tcc.gamma           = 2.2f;

    // Add features one-by-one with frame yields (matches deferred_stress_test pattern)
    // StandardViewCamera MUST attach before every camera consumer (DeferredGBuffer /
    // DeferredLighting / MeshShadow), which read the per-scene ViewCameraResource at
    // their own attach — owner-first ordering, so add it first.
    lux::render::ViewCameraCommTag view_cam_cfg{};
    openFrame(*session, ui);
    auto view_cam_freq = control.addFeature(scene_id, view_cam_reg.feature_type_id, view_cam_cfg);
    closeFrame(*session);
    auto view_cam_feat = waitReady(control, std::move(view_cam_freq), ui);

    // LightFeature MUST attach before ShadowMapFeature: ShadowMapFeature caches a raw
    // LightResources* at attach time, so the light data store must exist first.
    lux::render::LightCommTag light_cfg{};
    openFrame(*session, ui);
    auto light_freq = control.addFeature(scene_id, light_reg.feature_type_id, light_cfg);
    closeFrame(*session);
    auto light_feat = waitReady(control, std::move(light_freq), ui);

    // StandardMaterialFeature owns the global material stack (builtin shading models +
    // MaterialResources). StandardMeshStack's addMeshInstance reads the material slot,
    // and the material consumers bind MaterialResources, so it MUST attach BEFORE
    // StandardMeshStack (and before every material consumer).
    lux::render::MaterialCommTag material_cfg{};
    openFrame(*session, ui);
    auto material_freq = control.addFeature(scene_id, material_reg.feature_type_id, material_cfg);
    closeFrame(*session);
    auto material_feat = waitReady(control, std::move(material_freq), ui);

    // StandardMeshStack owns the scene mesh resources (InstanceResources /
    // VertexPoolRegistry); ShadowMap/MeshShadow/DeferredGBuffer CONSUME them at
    // their own attach, so it MUST be added BEFORE those mesh consumers.
    lux::render::MeshStackCommTag mesh_stack_cfg{};
    openFrame(*session, ui);
    auto mesh_stack_freq = control.addFeature(scene_id, mesh_stack_reg.feature_type_id, mesh_stack_cfg);
    closeFrame(*session);
    auto mesh_stack_feat = waitReady(control, std::move(mesh_stack_freq), ui);

    openFrame(*session, ui);
    auto shadow_freq = control.addFeature(scene_id, shmap_type.feature_type_id, smc);
    closeFrame(*session);
    auto shadow_feat = waitReady(control, std::move(shadow_freq), ui);

    openFrame(*session, ui);
    auto mesh_shadow_freq = control.addFeature(scene_id, mshsw_type.feature_type_id, msmc);
    closeFrame(*session);
    auto mesh_shadow_feat = waitReady(control, std::move(mesh_shadow_freq), ui);

    // HZB before the GBuffer: the GPU-driven cull find<>s HzbResources at attach.
    openFrame(*session, ui);
    auto hzb_freq = control.addFeature(scene_id, hzb_type.feature_type_id,
                                        lux::render::HzbCommTag{});
    closeFrame(*session);
    auto hzb_feat = waitReady(control, std::move(hzb_freq), ui);

    openFrame(*session, ui);
    auto gbuf_freq = control.addFeature(scene_id, gbuf_type.feature_type_id, gbcc);
    closeFrame(*session);
    auto gbuf_feat = waitReady(control, std::move(gbuf_freq), ui);

    openFrame(*session, ui);
    auto lit_freq = control.addFeature(scene_id, lit_type.feature_type_id, dlcc);
    closeFrame(*session);
    auto lit_feat = waitReady(control, std::move(lit_freq), ui);

    openFrame(*session, ui);
    auto pc_freq = control.addFeature(scene_id, pc_type.feature_type_id, pcsc);
    closeFrame(*session);
    auto pc_feat = waitReady(control, std::move(pc_freq), ui);

    openFrame(*session, ui);
    auto grid_freq = control.addFeature(scene_id, grid_type.feature_type_id, gcc);
    closeFrame(*session);
    auto grid_feat = waitReady(control, std::move(grid_freq), ui);

    openFrame(*session, ui);
    auto sky_freq = control.addFeature(scene_id, sky_type.feature_type_id, skcc);
    closeFrame(*session);
    auto sky_feat = waitReady(control, std::move(sky_freq), ui);

    // Tonemap added after skybox texture upload (below)
    FeatureAddedReply tm_feat{};

    // Upload skybox texture
    {
        auto tex_mgr = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog());
        lux::asset::TextureSerDeser tex_ser(tex_mgr);
        auto tex_res = tex_ser.fromLuxAsset(asset_dir / "textures" / "blue_nebulae_1.luxasset");
        if (tex_res)
        {
            auto *tex_asset = tex_res.value().first->as<lux::asset::TextureAsset>();
            auto &sky_tex   = *tex_asset->data();

            // Fail-fast guard so an unsupported asset format aborts the test
            // rather than rendering a black skybox via the rdesc createTexture2D
            // overload's synchronous known-bad path.
            {
                EPixelFormat probe{};
                if (!lux::render::toPixelFormat(sky_tex.pixelFormat(), probe))
                    throw std::runtime_error("Unsupported skybox texture pixel format");
            }

            auto sky_tex_submit = [&]() -> UploadSubmitResult<Texture2DCreatedReply>
            {
                constexpr uint32_t kMaxUploadMips = 16u;
                const uint32_t mip_count = std::min<uint32_t>(sky_tex.mipCount(), kMaxUploadMips);
                if (mip_count > 1u)
                {
                    std::vector<OwnedTextureMipLevel> mip_levels;
                    mip_levels.reserve(mip_count);
                    for (uint32_t i = 0; i < mip_count; ++i)
                    {
                        const auto& range = sky_tex.mipRange(i);
                        if (range.size == 0)
                            break;
                        if (range.offset + range.size > sky_tex.size())
                            throw std::runtime_error("Skybox mip range exceeds texture payload size");

                        auto pixels = sky_tex.pixels().subspan(
                            static_cast<std::size_t>(range.offset),
                            static_cast<std::size_t>(range.size));
                        if (pixels.size() != range.size)
                            throw std::runtime_error("Skybox mip range has no owner");
                        mip_levels.push_back(OwnedTextureMipLevel{
                            std::move(pixels), range.width, range.height});
                    }

                    if (mip_levels.size() > 1u)
                    {
                        return upload.tryCreateTexture2DMips(
                            std::move(mip_levels),
                            sky_tex.channel(),
                            sky_tex.pixelFormat(),
                            /*generate_mips=*/false);
                    }
                }

                return upload.tryCreateTexture2D(
                    sky_tex.pixels(),
                    sky_tex.width(),
                    sky_tex.height(),
                    sky_tex.channel(),
                    sky_tex.pixelFormat(),
                    /*generate_mips=*/!lux::rdesc::isCompressedFormat(sky_tex.pixelFormat()));
            }();
            if (!sky_tex_submit)
            {
                std::fprintf(stderr, "  Skybox upload admission failed\n");
                return 1;
            }
            auto sky_tex_reply = waitReady(
                upload,
                std::move(*sky_tex_submit),
                ui
            );

            SkyboxProxy skybox_proxy(*session, skybox_ops);
            openFrame(*session, ui);
            {
                SkyboxSetEquirectPayload wire{};
                wire.scene_id = scene_id;
                wire.feature  = sky_feat.feature;
                wire.texture  = sky_tex_reply.handle;
                skybox_proxy.setEquirect(wire);
            }
            closeFrame(*session);
            pumpFrame(*session, ui);   // let skybox upload complete
            std::printf("  Skybox texture uploaded\n");
        }
        else
        {
            std::fprintf(stderr, "  Warning: skybox texture not found, skipping\n");
        }
    }

    // Now add tonemap (after skybox texture is uploaded)
    openFrame(*session, ui);
    auto tm_freq = control.addFeature(scene_id, tm_type.feature_type_id, tcc);
    closeFrame(*session);
    tm_feat = waitReady(control, std::move(tm_freq), ui);

    std::printf("  Features added: 8/8\n");

    // ── 7. Upload meshes + materials (batched) ────────────────────
    std::vector<lux::rdesc::Vertex> cube_verts, floor_verts;
    std::vector<uint32_t> cube_idx, floor_idx;
    buildCube(cube_verts, cube_idx, kCubeHalf);
    buildFloorPlane(floor_verts, floor_idx, kFloorHalf);

    auto cube_mesh = lux::rdesc::Mesh{
        std::move(cube_verts), std::move(cube_idx),
        lux::math::AABB(Eigen::Vector3f(-kCubeHalf, -kCubeHalf, -kCubeHalf),
                        Eigen::Vector3f( kCubeHalf,  kCubeHalf,  kCubeHalf))};
    auto floor_mesh = lux::rdesc::Mesh{
        std::move(floor_verts), std::move(floor_idx),
        lux::math::AABB(Eigen::Vector3f(-kFloorHalf, -0.01f, -kFloorHalf),
                        Eigen::Vector3f( kFloorHalf,  0.01f,  kFloorHalf))};

    // Cube + floor GRAPH materials (deferred scene -> compile each graph's GBuffer
    // frag; uploadGraphMaterial gives each its own PSO, R1). compileShader borrows the
    // SPIR-V until closeFrame, so the words live in outer scope here.
    std::vector<uint32_t>  cube_spv, floor_spv;
    std::vector<std::byte> cube_info, floor_info;
    {
        auto cube_cb = lux::mgtest::compileGraphPass(
                lux::mgtest::makeColorGraph(0.9f, 0.45f, 0.15f,
                    lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness, 0.05f, 0.55f),
                lux::rdesc::EMaterialPass::GBuffer);
        auto floor_cb = lux::mgtest::compileGraphPass(
                lux::mgtest::makeColorGraph(0.6f, 0.6f, 0.6f,
                    lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness, 0.0f, 0.9f),
                lux::rdesc::EMaterialPass::GBuffer);
        if (!cube_cb || !floor_cb)
        {
            std::fprintf(stderr, "graph compile failed: %s\n",
                (!cube_cb ? cube_cb.error() : floor_cb.error()).c_str());
            return 1;
        }
        cube_spv   = std::move(cube_cb->spirv);
        cube_info  = std::move(cube_cb->info_bytes);
        floor_spv  = std::move(floor_cb->spirv);
        floor_info = std::move(floor_cb->info_bytes);
    }
    auto spvBytes = [](const std::vector<uint32_t>& v) {
        return std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(v.data()), v.size() * sizeof(uint32_t)};
    };

    openFrame(*session, ui);
    auto mesh_req = lux::render::uploadMesh(
        lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops), cube_mesh);
    auto fmesh_req = lux::render::uploadMesh(
        lux::render::MeshStackUploadClient(upload_client.client(), mesh_stack_ops), floor_mesh);
    auto cube_sh_req  = control.compileShader(spvBytes(cube_spv),
        std::span<const std::byte>{cube_info.data(), cube_info.size()});
    auto floor_sh_req = control.compileShader(spvBytes(floor_spv),
        std::span<const std::byte>{floor_info.data(), floor_info.size()});
    closeFrame(*session);

    auto mesh_h   = waitReady(upload, std::move(mesh_req), ui);
    auto fmesh_h  = waitReady(upload, std::move(fmesh_req), ui);
    auto cube_sh  = waitReady(control, std::move(cube_sh_req), ui);
    auto floor_sh = waitReady(control, std::move(floor_sh_req), ui);

    lux::render::GraphMaterialData cube_gd{}, floor_gd{};
    openFrame(*session, ui);
    auto mat_req = uploadGraphMaterial(
        MaterialUploadClient(upload_client.client(), material_ops),
        cube_gd, cube_sh.shader, lux::render::ShaderHandle{});
    auto fmat_req = uploadGraphMaterial(
        MaterialUploadClient(upload_client.client(), material_ops),
        floor_gd, floor_sh.shader, lux::render::ShaderHandle{});
    closeFrame(*session);

    auto mat_h   = waitReady(upload, std::move(mat_req), ui);
    auto fmat_h  = waitReady(upload, std::move(fmat_req), ui);

    std::printf("  Meshes + graph materials uploaded\n");

    // ── 8. Add mesh instances (batched) ────────────────────────────
    // Cube — lifted above grids to avoid Z-fighting
    constexpr float kModelLift = 2.0f;
    float cube_xform[16];
    setTranslation(cube_xform, state.cube_pos[0], state.cube_pos[1], state.cube_pos[2]);
    // Floor — also lifted
    float floor_xform[16];
    setTranslation(floor_xform, 0.f, kModelLift, 0.f);

    // Add mesh instances (scene-level) then make visible in BOTH views
    openFrame(*session, ui);
    auto cube_inst_req  = addTransientMeshInstance(MeshStackProxy(*session, mesh_stack_ops), scene_id, mesh_h.handle, mat_h.handle, cube_xform);
    auto floor_inst_req = addTransientMeshInstance(MeshStackProxy(*session, mesh_stack_ops), scene_id, fmesh_h.handle, fmat_h.handle, floor_xform);
    closeFrame(*session);

    auto cube_inst = waitReady(*session, std::move(cube_inst_req), ui);
    RenderObjectHandle cube_object = cube_inst.object;
    auto floor_inst = waitReady(*session, std::move(floor_inst_req), ui);
    RenderObjectHandle floor_object = floor_inst.object;

    // Register visibility in both views
    openFrame(*session, ui);
    MeshStackProxy(*session, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene_id, .view = view1, .object = cube_object});
    MeshStackProxy(*session, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene_id, .view = view1, .object = floor_object});
    MeshStackProxy(*session, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene_id, .view = view2, .object = cube_object});
    MeshStackProxy(*session, mesh_stack_ops).makeInstanceVisibleForView({.scene_id = scene_id, .view = view2, .object = floor_object});
    closeFrame(*session);

    std::printf("  Mesh instances created (cube object idx=%u gen=%u)\n",
        cube_object.index, cube_object.gen);

    // ── 9. Create lights (batched) ─────────────────────────────────
    // Directional light with cascaded shadows
    DirectionalLightDesc dl{};
    {
        float dx = -0.5f, dy = -1.f, dz = -0.3f;
        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        dl.direction = Eigen::Vector3f(dx/len, dy/len, dz/len);
    }
    dl.color              = Eigen::Vector3f(1.f, 0.95f, 0.8f);
    dl.intensity          = 1.5f;
    dl.flags              = 1u; // LIGHT_FLAG_CAST_SHADOW
    dl.shadow_map_size    = 2048;
    dl.shadow_bias        = 0.002f;
    dl.shadow_normal_bias = 0.05f;
    dl.cascade_count      = 4;
    dl.cascade_splits     = {10.f, 30.f, 70.f, 150.f};

    PointLightDesc pl1{};
    pl1.position  = Eigen::Vector3f(state.pl1_pos[0], state.pl1_pos[1], state.pl1_pos[2]);
    pl1.color     = Eigen::Vector3f(state.pl1_color[0], state.pl1_color[1], state.pl1_color[2]);
    pl1.intensity = state.pl1_intensity;
    pl1.range     = 15.f;
    pl1.flags             = 1u; // LF_CAST_SHADOW
    pl1.shadow_map_size   = 2048;
    pl1.shadow_bias       = 0.002f;
    pl1.shadow_normal_bias = 0.01f;

    PointLightDesc pl2{};
    pl2.position  = Eigen::Vector3f(state.pl2_pos[0], state.pl2_pos[1], state.pl2_pos[2]);
    pl2.color     = Eigen::Vector3f(state.pl2_color[0], state.pl2_color[1], state.pl2_color[2]);
    pl2.intensity = state.pl2_intensity;
    pl2.range     = 15.f;
    pl2.flags             = 1u; // LF_CAST_SHADOW
    pl2.shadow_map_size   = 2048;
    pl2.shadow_bias       = 0.002f;
    pl2.shadow_normal_bias = 0.01f;

    openFrame(*session, ui);
    auto dir_light_req = lightCreate(LightProxy(*session, light_ops), scene_id, LightDescriptor{dl});
    auto pl1_req2      = lightCreate(LightProxy(*session, light_ops), scene_id, LightDescriptor{pl1});
    auto pl2_req2      = lightCreate(LightProxy(*session, light_ops), scene_id, LightDescriptor{pl2});
    closeFrame(*session);

    auto dir_light = waitReady(*session, std::move(dir_light_req), ui);
    auto pl1_reply = waitReady(*session, std::move(pl1_req2), ui);
    auto pl2_reply = waitReady(*session, std::move(pl2_req2), ui);

    std::printf("  Lights created: 1 directional + 2 point\n");

    // ── 10. Point cloud proxy ───────────────────────────────────────
    PointCloudUploadClient pc_upload(upload_client.client(), pc_ops);
    PointCloudControlClient pc_control(control, pc_ops);
    std::mt19937 rng(42);
    std::vector<uint32_t> active_chunks;
    std::vector<PointCloudPoint> pc_buf; // keep alive until submitFrame

    std::printf("\n  === Rendering — ESC exit | V swap views | 5 toggle CSM ===\n\n");

    // ── 11. Main loop ───────────────────────────────────────────────
    auto start_time = std::chrono::steady_clock::now();
    auto fps_timer  = std::chrono::steady_clock::now();
    uint64_t frame_count = 0;
    double fps_accum = 0.0;
    auto prev_time = start_time;
    bool rotate_view_first = true;
    bool key_v_was_down = false;
    bool key_5_was_down = false;
    bool last_applied_csm_enabled = state.directional_csm_enabled;
    uint32_t last_applied_shadow_res = smc.atlas_page_resolution;
    uint32_t last_applied_shadow_pages = smc.atlas_page_count;
    uint32_t last_applied_shadow_slices = smc.max_shadow_slices;
    float last_applied_shadow_max_distance = smc.non_directional_shadow_max_distance;

    while (!window.shouldClose())
    {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - start_time).count();
        double dt = std::chrono::duration<double>(now - prev_time).count();
        prev_time = now;
        dt = std::min(dt, 0.1);

        // ── Poll + ImGui frame ──────────────────────────────────────
        lux::window::LuxWindow::pollEvents();
        ui.newFrame(); // paints all panels (reads SceneState via ControlPanel)
        auto *dd = ImGui::GetDrawData();

        // Drain replies + flush any pending submission before starting a new frame
        session->pumpReplies();
        session->trySubmitFrame(); // blocking — wait until server consumes

        if (!session->beginFrame())
            continue;

        // ── Submit ImGui draw data ──────────────────────────────────
        session->submitImGuiDrawData(RenderSceneId{}, dd);

        // ── Process deferred resizes(M2c:直达目标图像池)──────────
        if (pending_resize_v1.pending) {
            control.resizeTarget(target1,
                {pending_resize_v1.w, pending_resize_v1.h});
            pending_resize_v1.pending = false;
        }
        if (pending_resize_v2.pending) {
            control.resizeTarget(target2,
                {pending_resize_v2.w, pending_resize_v2.h});
            pending_resize_v2.pending = false;
        }

        // ── Update cube transform from ImGui values ─────────────────
        {
            // Auto-rotate Y so camera orbit / rendering can be visually confirmed
            state.cube_rot_deg[1] = std::fmod(elapsed * 30.f, 360.f);

            float rx = state.cube_rot_deg[0] * kPi / 180.f;
            float ry = state.cube_rot_deg[1] * kPi / 180.f;
            float rz = state.cube_rot_deg[2] * kPi / 180.f;
            float xform[16];
            setTransformEuler(xform,
                state.cube_pos[0], state.cube_pos[1], state.cube_pos[2],
                rx, ry, rz);
            updateTransientMeshTransform(MeshStackProxy(*session, mesh_stack_ops), scene_id, cube_object, xform);
        }

        // ── Update point lights ─────────────────────────────────────
        {
            PointLightDesc p{};
            p.position  = Eigen::Vector3f(state.pl1_pos[0], state.pl1_pos[1], state.pl1_pos[2]);
            p.color     = Eigen::Vector3f(state.pl1_color[0], state.pl1_color[1], state.pl1_color[2]);
            p.intensity = state.pl1_intensity;
            p.range     = 15.f;
            p.flags             = 1u;
            p.shadow_map_size   = 2048;
            p.shadow_bias       = 0.002f;
            p.shadow_normal_bias = 0.01f;
            lightUpdate(LightProxy(*session, light_ops), scene_id, pl1_reply.handle, LightDescriptor{p});
        }
        {
            PointLightDesc p{};
            p.position  = Eigen::Vector3f(state.pl2_pos[0], state.pl2_pos[1], state.pl2_pos[2]);
            p.color     = Eigen::Vector3f(state.pl2_color[0], state.pl2_color[1], state.pl2_color[2]);
            p.intensity = state.pl2_intensity;
            p.range     = 15.f;
            p.flags             = 1u;
            p.shadow_map_size   = 2048;
            p.shadow_bias       = 0.002f;
            p.shadow_normal_bias = 0.01f;
            lightUpdate(LightProxy(*session, light_ops), scene_id, pl2_reply.handle, LightDescriptor{p});
        }

        // ── Update cameras ──────────────────────────────────────────
        const bool key_v_down = (glfwGetKey(window.handle(), GLFW_KEY_V) == GLFW_PRESS);
        if (key_v_down && !key_v_was_down)
        {
            rotate_view_first = !rotate_view_first;
            std::printf("  Camera update order switched: %s\n",
                rotate_view_first
                    ? "rotating(view1) -> fixed(view2)"
                    : "fixed(view2) -> rotating(view1)");
        }
        key_v_was_down = key_v_down;

        // Toggle directional CSM with keyboard (edge-triggered).
        const bool key_5_down = (glfwGetKey(window.handle(), GLFW_KEY_5) == GLFW_PRESS);
        if (state.directional_csm_supported && key_5_down && !key_5_was_down)
        {
            state.directional_csm_enabled = !state.directional_csm_enabled;
        }
        key_5_was_down = key_5_down;

        // Push directional CSM changes once (from UI or keyboard).
        if (state.directional_csm_supported &&
            shadow_params_op != kInvalidTypeId &&
            state.directional_csm_enabled != last_applied_csm_enabled)
        {
            // Full reflected snapshot (param seam): carry the current quality knobs
            // alongside the toggled CSM flag so the feature applies one whole state.
            ShadowQualityParams sp{};
            sp.atlas_page_resolution = state.shadow_atlas_page_resolution;
            sp.atlas_page_count      = state.shadow_atlas_page_count;
            sp.max_shadow_slices     = state.shadow_max_slices;
            sp.non_directional_shadow_max_distance = state.shadow_max_distance;
            sp.enable_directional_csm = state.directional_csm_enabled ? 1u : 0u;
            FeatureParamsProxy(*session).setParams(
                scene_id, shadow_feat.feature, shadow_params_op, &sp, sizeof(sp));

            last_applied_csm_enabled = state.directional_csm_enabled;
            std::printf("  Directional shadow mode -> %s\n",
                state.directional_csm_enabled ? "CSM ON" : "CSM OFF");
        }

        if (state.shadow_quality_supported &&
            shadow_params_op != kInvalidTypeId &&
            state.shadow_quality_dirty)
        {
            const bool quality_changed =
                state.shadow_atlas_page_resolution != last_applied_shadow_res ||
                state.shadow_atlas_page_count != last_applied_shadow_pages ||
                state.shadow_max_slices != last_applied_shadow_slices ||
                std::abs(state.shadow_max_distance - last_applied_shadow_max_distance) > 1e-4f;
            if (quality_changed)
            {
                ShadowQualityParams sp{};
                sp.atlas_page_resolution = state.shadow_atlas_page_resolution;
                sp.atlas_page_count      = state.shadow_atlas_page_count;
                sp.max_shadow_slices     = state.shadow_max_slices;
                sp.non_directional_shadow_max_distance = state.shadow_max_distance;
                sp.enable_directional_csm = state.directional_csm_enabled ? 1u : 0u;
                FeatureParamsProxy(*session).setParams(
                    scene_id, shadow_feat.feature, shadow_params_op, &sp, sizeof(sp));

                last_applied_shadow_res = state.shadow_atlas_page_resolution;
                last_applied_shadow_pages = state.shadow_atlas_page_count;
                last_applied_shadow_slices = state.shadow_max_slices;
                last_applied_shadow_max_distance = state.shadow_max_distance;

                std::printf(
                    "  Shadow quality -> res=%u pages=%u slices=%u maxDist=%.1f\n",
                    state.shadow_atlas_page_resolution,
                    state.shadow_atlas_page_count,
                    state.shadow_max_slices,
                    state.shadow_max_distance);
            }
            state.shadow_quality_dirty = false;
        }

        float cam_angle = elapsed * kCamSpeed;
        Eigen::Vector3f eye_rot(
            kCamRadius * std::cos(cam_angle),
            kCamHeight,
            kCamRadius * std::sin(cam_angle));
        Eigen::Matrix4f V_rot = buildViewMatrix(eye_rot, target, up);
        auto cs = main_vp.sceneView().contentSize();
        float aspect = (cs.x > 0 && cs.y > 0) ? cs.x / cs.y : 800.f / 600.f;
        Eigen::Matrix4f P_rot = buildProjMatrix(60.f * kPi / 180.f, aspect, 0.1f, 200.f);

        Eigen::Vector3f eye_fixed(0.f, 20.f, 10.f);
        Eigen::Matrix4f V_fixed = buildViewMatrix(eye_fixed, target, up);
        auto cs2 = sec_vp.sceneView().contentSize();
        float aspect2 = (cs2.x > 0 && cs2.y > 0) ? cs2.x / cs2.y : 400.f / 300.f;
        Eigen::Matrix4f P_fixed = buildProjMatrix(60.f * kPi / 180.f, aspect2, 0.1f, 200.f);

        if (rotate_view_first)
        {
            viewCameraUpdateTransient(ViewCameraProxy(*session, view_cam_ops), scene_id, view1, V_rot.data(), P_rot.data(), eye_rot.data());
            viewCameraUpdateTransient(ViewCameraProxy(*session, view_cam_ops), scene_id, view2, V_fixed.data(), P_fixed.data(), eye_fixed.data());
        }
        else
        {
            viewCameraUpdateTransient(ViewCameraProxy(*session, view_cam_ops), scene_id, view2, V_fixed.data(), P_fixed.data(), eye_fixed.data());
            viewCameraUpdateTransient(ViewCameraProxy(*session, view_cam_ops), scene_id, view1, V_rot.data(), P_rot.data(), eye_rot.data());
        }

        // ── Point cloud actions ─────────────────────────────────────
        pc_buf.clear();
        if (state.gen_chunk)
        {
            state.gen_chunk = false;
            uint32_t cid = state.chunk_count;
            pc_buf = generateRandomChunk(cid, rng);
            {
                        auto pc_span = std::span<const PointCloudPoint>(pc_buf.data(), pc_buf.size());
                        lux::render::UploadPointCloudChunkPayload up{};
                        up.scene_id    = scene_id;
                        up.chunk_id    = cid;
                        up.point_count = static_cast<uint32_t>(pc_span.size());
                        (void)pc_upload.uploadChunk(up, std::as_bytes(pc_span),
                                             alignof(lux::render::PointCloudPoint));
                    }
            active_chunks.push_back(cid);
            state.chunk_count++;
            state.total_points += pc_buf.size();
        }
        if (state.remove_last && !active_chunks.empty())
        {
            state.remove_last = false;
            uint32_t cid = active_chunks.back();
            pc_control.removeChunk({.scene_id = scene_id, .chunk_id = cid});
            active_chunks.pop_back();
            state.chunk_count--;
            state.total_points = static_cast<uint64_t>(state.chunk_count) * kPCPointsPerChunk;
        }
        if (state.clear_all)
        {
            state.clear_all = false;
            for (auto cid : active_chunks)
                pc_control.removeChunk({.scene_id = scene_id, .chunk_id = cid});
            active_chunks.clear();
            state.chunk_count = 0;
            state.total_points = 0;
        }

        // ── Submit frame (blocking to stay in sync with server) ─────
        session->trySubmitFrame();
        session->pumpReplies();

        // ── FPS counter ─────────────────────────────────────────────
        ++frame_count;
        fps_accum += dt;
        double fps_elapsed = std::chrono::duration<double>(now - fps_timer).count();
        if (fps_elapsed >= 2.0)
        {
            double avg_fps = frame_count / fps_accum;
            std::printf("  FPS: %d  cam_angle=%.2f  rotating_view=%u fixed_view=%u order=%s eye=(%.1f, %.1f, %.1f)\n",
                static_cast<int>(avg_fps),
                cam_angle,
                view1.index, view2.index,
                rotate_view_first ? "V1->V2" : "V2->V1",
                eye_rot.x(), eye_rot.y(), eye_rot.z());
            frame_count = 0;
            fps_accum   = 0.0;
            fps_timer   = now;
        }

        // ESC to exit
        if (glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS)
            break;
    }

    // ── 12. Shutdown ────────────────────────────────────────────────
    sync->requestStop();
    render_thread.join();
    session.reset();

    std::printf("\n=== Scene View Integration Test Complete ===\n");
    return 0;
}
