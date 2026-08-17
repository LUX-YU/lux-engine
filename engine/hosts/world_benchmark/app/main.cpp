#include <lux/engine/hosts/game_application/GameApplication.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/platform/FormatCompat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    constexpr std::array<std::string_view,
        lux::runtime::kFrameTracePhaseCount> kFrameTracePhaseNames{
        "control_reply_pump",
        "frame_reply_pump",
        "pending_submit",
        "frame_slot_wait",
        "frame_open",
        "main_completion",
        "event_drain",
        "texture_streaming",
        "contribution_safe_point",
        "integration_safe_point",
        "schedule_input",
        "schedule_pre_transform",
        "schedule_simulation",
        "schedule_pre_render",
        "schedule_render",
        "schedule_post_render",
        "command_barrier",
        "frame_submit"};
    static_assert(kFrameTracePhaseNames.size() ==
        lux::runtime::kFrameTracePhaseCount);

    struct Options final
    {
        std::filesystem::path pak;
        std::filesystem::path engine_pak;
        std::string scene;
        std::filesystem::path json{"world-benchmark-report.json"};
        std::filesystem::path csv{"world-benchmark-frames.csv"};
        std::uint32_t repeat{5u};
        std::uint32_t warmup_frames{180u};
        std::uint32_t sample_frames{1200u};
        std::uint32_t width{1920u};
        std::uint32_t height{1080u};
        std::uint32_t world_edge_m{102400u};
        std::uint64_t session_token{0u};
        bool interactive{false};
        bool validation{false};
        bool real_time_pacing{false};
        std::optional<std::uint32_t> dump_graph_frame;
        std::optional<std::uint32_t> fixed_route_frame;
        std::filesystem::path semantic_capture_dir;
        bool require_route_semantics{false};
        std::optional<double> required_p99_attribution;
        std::optional<double> required_stability_percent;
    };

    struct RunResult final
    {
        struct SystemFrameSample final
        {
            std::uint32_t sample_frame{0u};
            std::uint32_t route_frame{0u};
            std::uint64_t frame_serial{0u};
            std::uint64_t system_hash{0u};
            std::string_view system_name;
            int phase{lux::ecs::kPhaseSimulation};
            std::uint64_t wall_nanoseconds{0u};
        };
        struct StabilitySnapshot final
        {
            std::uint32_t sample_frame{0u};
            std::uint32_t route_frame{0u};
            std::uint64_t frame_serial{0u};
            lux::game::GameApplicationTelemetry telemetry{};
            bool has_telemetry{false};
        };
        std::vector<double> frame_ms;
        std::vector<std::uint32_t> route_frames;
        std::vector<lux::runtime::FrameTrace> frame_traces;
        std::vector<SystemFrameSample> system_frames;
        std::vector<StabilitySnapshot> stability_snapshots;
        lux::deployment::RuntimeCapacityPlan capacity_plan{};
        lux::game::GameApplicationTelemetry telemetry{};
        bool has_telemetry{false};
        std::string gpu_json{"{\"available\":false,\"reason\":\"not sampled\"}"};
    };

    [[nodiscard]] std::optional<std::string_view> valueAfter(
        std::string_view argument,
        std::string_view prefix) noexcept
    {
        return argument.starts_with(prefix)
            ? std::optional{argument.substr(prefix.size())}
            : std::nullopt;
    }

    [[nodiscard]] bool parseUnsigned(
        std::string_view text,
        std::uint32_t& value) noexcept
    {
        if (text.empty())
            return false;
        std::uint64_t parsed = 0u;
        for (const char c : text)
        {
            if (c < '0' || c > '9')
                return false;
            parsed = parsed * 10u + static_cast<unsigned>(c - '0');
            if (parsed > std::numeric_limits<std::uint32_t>::max())
                return false;
        }
        value = static_cast<std::uint32_t>(parsed);
        return true;
    }

    [[nodiscard]] bool parseDouble(
        std::string_view text,
        double& value) noexcept
    {
        if (text.empty())
            return false;
        const auto parsed = std::from_chars(
            text.data(),
            text.data() + text.size(),
            value);
        return parsed.ec == std::errc{} &&
            parsed.ptr == text.data() + text.size() &&
            std::isfinite(value);
    }

    void usage()
    {
        std::puts(
            "usage: lux_world_benchmark --pak=<game.luxpak> [options]\n"
            "  --scene=<vpath>       explicit boot EntityScene\n"
            "  --engine-pak=<file>   optional separate engine-content Pak\n"
            "  --json=<file>         aggregate JSON report\n"
            "  --csv=<file>          per-frame CSV report\n"
            "  --repeat=N            clean runs (default 5)\n"
            "  --warmup=N            warmup frames (default 180)\n"
            "  --frames=N            measured frames (default 1200)\n"
            "  --soak=N              measured seconds at fixed 60 Hz\n"
            "  --size=WxH            render extent (default 1920x1080)\n"
            "  --world-edge=N        generated World edge in metres (default 102400)\n"
            "  --interactive         show the GLFW window\n"
            "  --fixed-route-frame=N hold one canonical route pose/phase\n"
            "  --dump-graph-frame=N  print the compiled graph after warmup frame N\n"
            "  --semantic-capture-dir=<dir> capture fixed-route screenshots\n"
            "  --require-route-semantics fail on structural/pixel capture gates\n"
            "  --require-p99-attribution=R fail unless every p99 frame reaches R (0..1)\n"
            "  --require-stability-percent=P fail unless lap snapshots remain within P%\n"
            "  --vk-validation       enable Vulkan validation\n");
    }

    [[nodiscard]] std::optional<Options> parse(int argc, char** argv)
    {
        Options result;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument{argv[index]};
            if (argument == "--help" || argument == "-h")
            {
                usage();
                return std::nullopt;
            }
            if (argument == "--interactive")
            {
                result.interactive = true;
                continue;
            }
            if (argument == "--vk-validation")
            {
                result.validation = true;
                continue;
            }
            if (argument == "--require-route-semantics")
            {
                result.require_route_semantics = true;
                continue;
            }
            if (const auto value = valueAfter(argument, "--pak="))
                result.pak = *value;
            else if (const auto value = valueAfter(argument, "--engine-pak="))
                result.engine_pak = *value;
            else if (const auto value = valueAfter(argument, "--scene="))
                result.scene = *value;
            else if (const auto value = valueAfter(
                         argument, "--semantic-capture-dir="))
                result.semantic_capture_dir = *value;
            else if (const auto value = valueAfter(argument, "--json="))
                result.json = *value;
            else if (const auto value = valueAfter(argument, "--csv="))
                result.csv = *value;
            else if (const auto value = valueAfter(argument, "--repeat="))
            {
                if (!parseUnsigned(*value, result.repeat))
                    return std::nullopt;
            }
            else if (const auto value = valueAfter(argument, "--warmup="))
            {
                if (!parseUnsigned(*value, result.warmup_frames))
                    return std::nullopt;
            }
            else if (const auto value = valueAfter(argument, "--frames="))
            {
                if (!parseUnsigned(*value, result.sample_frames))
                    return std::nullopt;
            }
            else if (const auto value = valueAfter(argument, "--soak="))
            {
                std::uint32_t seconds = 0u;
                if (!parseUnsigned(*value, seconds) ||
                    seconds > std::numeric_limits<std::uint32_t>::max() / 60u)
                {
                    return std::nullopt;
                }
                result.sample_frames = seconds * 60u;
                result.real_time_pacing = true;
            }
            else if (const auto value = valueAfter(argument, "--size="))
            {
                unsigned width = 0u;
                unsigned height = 0u;
                const std::string owned{*value};
                if (std::sscanf(
                        owned.c_str(), "%ux%u", &width, &height) != 2)
                {
                    return std::nullopt;
                }
                result.width = width;
                result.height = height;
            }
            else if (const auto value = valueAfter(argument, "--world-edge="))
            {
                if (!parseUnsigned(*value, result.world_edge_m) ||
                    result.world_edge_m < 2048u)
                {
                    return std::nullopt;
                }
            }
            else if (const auto value = valueAfter(
                         argument, "--dump-graph-frame="))
            {
                std::uint32_t frame = 0u;
                if (!parseUnsigned(*value, frame))
                    return std::nullopt;
                result.dump_graph_frame = frame;
            }
            else if (const auto value = valueAfter(
                         argument, "--fixed-route-frame="))
            {
                std::uint32_t frame = 0u;
                if (!parseUnsigned(*value, frame))
                    return std::nullopt;
                result.fixed_route_frame = frame;
            }
            else if (const auto value = valueAfter(
                         argument, "--require-p99-attribution="))
            {
                double threshold = 0.0;
                if (!parseDouble(*value, threshold) ||
                    threshold < 0.0 || threshold > 1.0)
                {
                    return std::nullopt;
                }
                result.required_p99_attribution = threshold;
            }
            else if (const auto value = valueAfter(
                         argument, "--require-stability-percent="))
            {
                double percent = 0.0;
                if (!parseDouble(*value, percent) ||
                    percent < 0.0 || percent > 100.0)
                {
                    return std::nullopt;
                }
                result.required_stability_percent = percent;
            }
            else
            {
                std::fprintf(stderr, "unknown option: %.*s\n",
                    static_cast<int>(argument.size()), argument.data());
                return std::nullopt;
            }
        }
        if (result.pak.empty() || result.repeat == 0u ||
            result.sample_frames == 0u || result.width == 0u ||
            result.height == 0u)
        {
            return std::nullopt;
        }
        if (result.required_stability_percent && result.fixed_route_frame)
            return std::nullopt;
        if (result.require_route_semantics &&
            result.semantic_capture_dir.empty())
        {
            return std::nullopt;
        }
        return result;
    }

    // Must remain byte-for-byte formula-compatible with the benchmark
    // generator. The route is scale-independent, while generated mountains
    // are not bounded by the smoke World's old fixed camera heights.
    [[nodiscard]] double terrainHeight(
        double x,
        double z,
        std::uint32_t world_edge_m) noexcept
    {
        const auto ridge = std::abs(std::sin(x * 0.00063) *
            std::cos(z * 0.00051));
        const auto rolling = std::sin(x * 0.0031 + z * 0.0017) * 34.0 +
            std::cos(z * 0.0023 - x * 0.0011) * 22.0;
        const auto mountain = std::pow(ridge, 3.2) * 540.0;
        const auto terrain_edge = std::max<std::uint32_t>(
            world_edge_m / 1024u, 1u);
        const auto water_cell = std::min<std::uint32_t>(
            8u, (terrain_edge - 1u) / 2u);
        const auto water_center =
            (static_cast<double>(water_cell) + 0.5) * 1024.0;
        const auto lake_basin = -70.0 * std::exp(
            -((x - water_center) * (x - water_center) +
                (z - water_center) * (z - water_center)) /
                8000000.0);
        return rolling + mountain + lake_basin + 30.0;
    }

    [[nodiscard]] lux::game::GameApplicationCameraPose routePose(
        std::uint64_t frame,
        std::uint32_t world_edge_m)
    {
        constexpr double dt = 1.0 / 60.0;
        constexpr std::uint64_t route_frames = 1200u;
        const auto local = frame % route_frames;
        const auto edge = static_cast<double>(world_edge_m);
        const auto terrain_cells = std::max<std::uint32_t>(
            world_edge_m / 1024u, 1u);
        const auto town_x_cell = (terrain_cells - 1u) / 2u;
        const auto town_z_cell = terrain_cells / 2u;
        const auto town_street_x =
            static_cast<double>(town_x_cell) * 1024.0 + 420.0;
        const auto town_street_start_z =
            static_cast<double>(town_z_cell) * 1024.0 + 100.0;
        const auto boundary = std::clamp(
            std::floor((edge * 0.5) / 1024.0) * 1024.0,
            1024.0,
            edge - 1024.0);
        double x = 0.0;
        double y = 180.0;
        double z = 0.0;
        double target_x = edge * 0.5;
        double target_z = edge * 0.5;
        double surface_clearance = 20.0;
        double target_clearance = 8.0;
        if (local < 120u)
        {
            // Lake-side overview: the smoke recipe places its deterministic
            // town in the central Cells and the lake in their near corner.
            // This framing shows recognizable water/building/terrain layers
            // while retaining a long horizon for HLOD coverage.
            x = std::clamp(town_street_x - 260.0, 96.0, edge - 96.0);
            y = 260.0;
            z = std::clamp(
                town_street_start_z - 420.0,
                96.0,
                edge - 96.0);
            surface_clearance = 180.0;
            target_clearance = 8.0;
            target_x = std::clamp(town_street_x, 64.0, edge - 64.0);
            target_z = std::clamp(
                town_street_start_z + 430.0,
                64.0,
                edge - 64.0);
        }
        else if (local < 360u)
        {
            // Ground-level street through the deterministic showcase town.
            // On the 4 km smoke World this is Cell (1,2); the normalized
            // formulation remains useful for the larger benchmark scales.
            x = town_street_x;
            y = 6.0;
            z = town_street_start_z +
                static_cast<double>(local - 120u) * dt * 5.0;
            surface_clearance = 6.0;
            target_clearance = 4.0;
            target_x = x;
            target_z = std::min(edge - 32.0, z + 420.0);
        }
        else if (local < 600u)
        {
            x = edge * 0.35 + static_cast<double>(local - 360u) * dt * 30.0;
            y = 15.0;
            z = edge * 0.35;
            surface_clearance = 15.0;
            target_clearance = 4.0;
            target_x = std::min(edge - 32.0, x + 320.0);
            target_z = std::min(edge - 32.0, z + 100.0);
        }
        else if (local < 840u)
        {
            x = edge * 0.55 + static_cast<double>(local - 600u) * dt * 200.0;
            y = 400.0;
            z = edge * 0.55;
            surface_clearance = 150.0;
            target_clearance = 16.0;
            target_x = std::min(edge - 32.0, x + 720.0);
            target_z = std::min(edge - 32.0, z + 320.0);
        }
        else if (local < 960u)
        {
            const auto point = ((local - 840u) / 30u) % 4u;
            static constexpr double fractions[][3]{
                {0.08, 80.0, 0.08},
                {0.50, 900.0, 0.50},
                {0.88, 120.0, 0.12},
                {0.16, 90.0, 0.16}};
            x = std::clamp(edge * fractions[point][0], 64.0, edge - 64.0);
            y = std::min(fractions[point][1], 80.0 + edge * 0.02);
            z = std::clamp(edge * fractions[point][2], 64.0, edge - 64.0);
            surface_clearance = point == 1u ? 200.0 : 30.0;
            target_clearance = 10.0;
            // The benchmark currently has no Terrain hole or cooked cave mesh.
            // Keep every surface-route teleport above the sampled heightfield;
            // an underground camera would only expose its two-sided underside.
            target_x = edge * 0.5;
            target_z = edge * 0.5;
        }
        else
        {
            // Keep the deliberate boundary oscillation as a streaming stress
            // segment, then dwell for the final half second. The lap-end
            // stability snapshot must describe a reproducible route phase,
            // not whichever asynchronous generation happened to finish
            // during the final four-frame boundary crossing.
            const auto boundary_frame = std::min(local, 1169ull);
            const double sign = ((boundary_frame / 4u) & 1u) == 0u
                ? -1.0
                : 1.0;
            x = boundary + sign * 2.0;
            y = 20.0;
            z = boundary + sign * 2.0;
            surface_clearance = 20.0;
            target_clearance = 4.0;
            target_x = edge * 0.5;
            target_z = edge * 0.5;
        }
        const auto terrain_height = terrainHeight(x, z, world_edge_m);
        y = (std::max)(y, terrain_height + surface_clearance);
        if (std::abs(target_x - x) + std::abs(target_z - z) < 1.0)
        {
            target_x = std::clamp(x + edge * 0.18, 32.0, edge - 32.0);
            target_z = std::clamp(z + edge * 0.09, 32.0, edge - 32.0);
        }
        auto target_height = terrainHeight(
            target_x, target_z, world_edge_m) + target_clearance;
        if (local >= 120u && local < 360u)
            target_height = std::min(target_height, y - 0.75);
        lux::game::GameApplicationCameraPose pose;
        pose.position = {x, y, z};
        // Aim at a scale-aware point on the generated surface. A fixed world
        // direction looked outside the 4 km smoke World from several route
        // segments and made otherwise valid content impossible to identify.
        pose.forward = {
            static_cast<float>(target_x - x),
            static_cast<float>(target_height - y),
            static_cast<float>(target_z - z)};
        return pose;
    }

    [[nodiscard]] lux::navigation::NavigationPathRequest
    navigationProbeRequest(
        std::uint64_t route_frame,
        std::uint32_t world_edge_m)
    {
        const auto pose = routePose(route_frame, world_edge_m);
        constexpr double cell_edge = 1024.0;
        const auto cell_x = std::floor(pose.position.x / cell_edge) *
            cell_edge;
        const auto cell_z = std::floor(pose.position.z / cell_edge) *
            cell_edge;
        const auto x = std::clamp(
            pose.position.x, cell_x + 96.0, cell_x + cell_edge - 96.0);
        const auto z = std::clamp(
            pose.position.z, cell_z + 96.0, cell_z + cell_edge - 96.0);
        const auto destination_x = std::min(
            cell_x + cell_edge - 64.0, x + 32.0);
        const auto destination_z = std::min(
            cell_z + cell_edge - 64.0, z + 16.0);
        lux::navigation::NavigationPathRequest request;
        request.start = {
            x, terrainHeight(x, z, world_edge_m), z};
        request.destination = {
            destination_x,
            terrainHeight(destination_x, destination_z, world_edge_m),
            destination_z};
        request.nearest_horizontal_extent = 8.0f;
        request.nearest_vertical_extent = 128.0f;
        return request;
    }

    [[nodiscard]] std::optional<lux::navigation::NavigationPathResult>
    queryNavigationProbe(
        lux::game::GameApplication& application,
        std::uint64_t route_frame,
        std::uint32_t world_edge_m)
    {
        auto result = application.queryNavigationPath(
            navigationProbeRequest(route_frame, world_edge_m));
        if (result && result->status ==
                lux::navigation::ENavigationPathStatus::PENDING &&
            result->missing_regions.empty())
        {
            return std::nullopt;
        }
        return result;
    }

    void paceFrame(
        bool enabled,
        std::chrono::steady_clock::time_point& deadline)
    {
        if (!enabled)
            return;
        deadline += std::chrono::nanoseconds{1'000'000'000ll / 60ll};
        const auto now = std::chrono::steady_clock::now();
        if (now < deadline)
            std::this_thread::sleep_until(deadline);
        else if (now - deadline > std::chrono::milliseconds{250})
            deadline = now;
    }

    enum class EEnvironmentPhase : std::uint8_t
    {
        NOON,
        DUSK,
        NIGHT,
        DENSE_FOG,
        CLEAR_NIGHT
    };

    [[nodiscard]] EEnvironmentPhase environmentPhase(
        std::uint64_t frame) noexcept
    {
        switch ((frame % 1200u) / 240u)
        {
        case 0u: return EEnvironmentPhase::NOON;
        case 1u: return EEnvironmentPhase::DUSK;
        case 2u: return EEnvironmentPhase::NIGHT;
        case 3u: return EEnvironmentPhase::DENSE_FOG;
        default: return EEnvironmentPhase::CLEAR_NIGHT;
        }
    }

    [[nodiscard]] std::optional<lux::game::GameApplicationVisualPatch>
    visualPatch(
        const lux::game::GameApplicationVisualState& current,
        EEnvironmentPhase phase) noexcept
    {
        if (!current.skybox || !current.directional_light ||
            !current.height_fog)
        {
            return std::nullopt;
        }
        lux::game::GameApplicationVisualPatch result;
        result.skybox = *current.skybox;
        result.directional_light = *current.directional_light;
        result.height_fog = *current.height_fog;
        auto& sky = *result.skybox;
        auto& sun = *result.directional_light;
        auto& fog = *result.height_fog;
        sun.cast_shadow = true;
        sun.cascade_count = 4u;
        sun.shadow_map_size = 2048u;
        fog.enabled = true;
        fog.color = {0.56f, 0.64f, 0.72f};
        fog.density = 0.00011f;
        fog.start_distance = 500.0f;
        fog.reference_height = 80.0f;
        fog.height_falloff = 0.004f;
        fog.maximum_opacity = 0.88f;
        switch (phase)
        {
        case EEnvironmentPhase::NOON:
            sky.intensity = 1.0f;
            sun.direction = {
                0.36514837f, -0.91287094f, 0.18257418f};
            sun.color = {1.0f, 0.94f, 0.82f};
            sun.intensity = 2.0f;
            break;
        case EEnvironmentPhase::DUSK:
            sky.intensity = 0.35f;
            sun.direction = {
                0.97014248f, -0.24253562f, 0.0f};
            sun.color = {1.0f, 0.42f, 0.18f};
            sun.intensity = 3.0f;
            fog.color = {0.48f, 0.30f, 0.30f};
            break;
        case EEnvironmentPhase::NIGHT:
            sky.intensity = 0.06f;
            sun.direction = {
                -0.36514837f, -0.91287094f, -0.18257418f};
            sun.color = {0.28f, 0.38f, 0.70f};
            sun.intensity = 0.18f;
            fog.color = {0.08f, 0.11f, 0.18f};
            fog.density = 0.00018f;
            fog.maximum_opacity = 0.82f;
            break;
        case EEnvironmentPhase::DENSE_FOG:
            sky.intensity = 0.18f;
            sun.direction = {
                0.97014248f, -0.24253562f, 0.0f};
            sun.color = {0.80f, 0.84f, 0.90f};
            sun.intensity = 1.2f;
            fog.color = {0.46f, 0.51f, 0.56f};
            fog.density = 0.0022f;
            fog.start_distance = 30.0f;
            fog.height_falloff = 0.0015f;
            fog.maximum_opacity = 0.97f;
            break;
        case EEnvironmentPhase::CLEAR_NIGHT:
            sky.intensity = 0.04f;
            sun.direction = {
                -0.36514837f, -0.91287094f, -0.18257418f};
            sun.color = {0.24f, 0.34f, 0.66f};
            sun.intensity = 0.12f;
            fog.enabled = false;
            break;
        }
        return result;
    }

    [[nodiscard]] bool updateEnvironment(
        lux::game::GameApplication& application,
        EEnvironmentPhase phase,
        std::optional<EEnvironmentPhase>& applied) noexcept
    {
        if (applied && *applied == phase)
            return true;
        const auto current = application.visualState();
        const auto patch = current ? visualPatch(*current, phase) : std::nullopt;
        if (!patch || !application.patchVisualState(*patch))
        {
            return false;
        }
        applied = phase;
        return true;
    }

    [[nodiscard]] bool updateEnvironment(
        lux::game::GameApplication& application,
        std::uint64_t frame,
        std::optional<EEnvironmentPhase>& applied) noexcept
    {
        return updateEnvironment(
            application,
            environmentPhase(frame),
            applied);
    }

    struct RouteSemanticCheckpoint final
    {
        std::string_view name;
        std::uint32_t route_frame{0u};
        EEnvironmentPhase environment{EEnvironmentPhase::NOON};
    };

    [[nodiscard]] lux::game::GameApplicationCameraPose semanticPose(
        const RouteSemanticCheckpoint& checkpoint,
        std::uint32_t world_edge_m)
    {
        if (!checkpoint.name.starts_with("cell-boundary"))
            return routePose(checkpoint.route_frame, world_edge_m);

        const auto edge = static_cast<double>(world_edge_m);
        const auto boundary = std::clamp(
            std::floor((edge * 0.5) / 1024.0) * 1024.0,
            1024.0,
            edge - 1024.0);
        const auto x = boundary + 2.0;
        const auto z = boundary - 2.0;
        const auto y = terrainHeight(x, z, world_edge_m) + 320.0;
        const auto target_x = std::min(edge - 64.0, boundary + 620.0);
        const auto target_z = std::max(64.0, boundary - 520.0);
        const auto target_y = terrainHeight(
            target_x,
            target_z,
            world_edge_m) + 12.0;
        lux::game::GameApplicationCameraPose pose;
        pose.position = {x, y, z};
        pose.forward = {
            static_cast<float>(target_x - x),
            static_cast<float>(target_y - y),
            static_cast<float>(target_z - z)};
        return pose;
    }

    struct RouteSemanticImageStats final
    {
        double mean_luma{0.0};
        double standard_deviation{0.0};
        double non_black_fraction{0.0};
        double deep_black_fraction{0.0};
        double edge_fraction{0.0};
        std::uint32_t minimum_luma{255u};
        std::uint32_t maximum_luma{0u};
        std::uint32_t coarse_color_count{0u};
    };

    struct RouteSemanticCapture final
    {
        RouteSemanticCheckpoint checkpoint{};
        std::filesystem::path image;
        RouteSemanticImageStats image_stats{};
        lux::game::GameApplicationTelemetry telemetry{};
        bool passed{false};
        double revisit_mean_absolute_error{0.0};
        double revisit_changed_fraction{0.0};
        std::vector<std::string> failures;
    };

    void writeLittleEndian16(std::ostream& output, std::uint16_t value)
    {
        output.put(static_cast<char>(value & 0xffu));
        output.put(static_cast<char>((value >> 8u) & 0xffu));
    }

    void writeLittleEndian32(std::ostream& output, std::uint32_t value)
    {
        output.put(static_cast<char>(value & 0xffu));
        output.put(static_cast<char>((value >> 8u) & 0xffu));
        output.put(static_cast<char>((value >> 16u) & 0xffu));
        output.put(static_cast<char>((value >> 24u) & 0xffu));
    }

    [[nodiscard]] bool writeCaptureBmp(
        const std::filesystem::path& path,
        const lux::game::GameApplicationFrameCapture& capture)
    {
        const auto pixel_bytes = static_cast<std::uint64_t>(
            capture.extent.width) * capture.extent.height * 4u;
        if (capture.extent.width == 0u || capture.extent.height == 0u ||
            pixel_bytes != capture.pixels_bgra8.size() ||
            pixel_bytes > std::numeric_limits<std::uint32_t>::max() - 54u)
        {
            return false;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;

        output.put('B');
        output.put('M');
        writeLittleEndian32(
            output, static_cast<std::uint32_t>(54u + pixel_bytes));
        writeLittleEndian16(output, 0u);
        writeLittleEndian16(output, 0u);
        writeLittleEndian32(output, 54u);
        writeLittleEndian32(output, 40u);
        writeLittleEndian32(output, capture.extent.width);
        // A negative DIB height preserves the GPU readback's top-first rows.
        writeLittleEndian32(
            output,
            static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(capture.extent.height)));
        writeLittleEndian16(output, 1u);
        writeLittleEndian16(output, 32u);
        writeLittleEndian32(output, 0u);
        writeLittleEndian32(
            output, static_cast<std::uint32_t>(pixel_bytes));
        writeLittleEndian32(output, 2835u);
        writeLittleEndian32(output, 2835u);
        writeLittleEndian32(output, 0u);
        writeLittleEndian32(output, 0u);
        for (std::size_t index = 0u;
             index < capture.pixels_bgra8.size(); index += 4u)
        {
            output.put(static_cast<char>(capture.pixels_bgra8[index + 0u]));
            output.put(static_cast<char>(capture.pixels_bgra8[index + 1u]));
            output.put(static_cast<char>(capture.pixels_bgra8[index + 2u]));
            output.put(static_cast<char>(255u));
        }
        return static_cast<bool>(output);
    }

    [[nodiscard]] RouteSemanticImageStats analyzeCapture(
        const lux::game::GameApplicationFrameCapture& capture)
    {
        RouteSemanticImageStats result;
        const auto width = static_cast<std::size_t>(capture.extent.width);
        const auto height = static_cast<std::size_t>(capture.extent.height);
        const auto pixel_count = width * height;
        if (pixel_count == 0u ||
            capture.pixels_bgra8.size() != pixel_count * 4u)
        {
            return result;
        }

        std::array<bool, 4096u> coarse_colors{};
        double sum = 0.0;
        double squared_sum = 0.0;
        std::size_t non_black = 0u;
        std::size_t deep_black = 0u;
        std::size_t edge_count = 0u;
        std::size_t edge_samples = 0u;
        const auto lumaAt = [&](std::size_t pixel)
        {
            const auto offset = pixel * 4u;
            const auto blue = capture.pixels_bgra8[offset + 0u];
            const auto green = capture.pixels_bgra8[offset + 1u];
            const auto red = capture.pixels_bgra8[offset + 2u];
            return static_cast<std::uint32_t>(
                (19u * blue + 183u * green + 54u * red) >> 8u);
        };
        for (std::size_t y = 0u; y < height; ++y)
        {
            for (std::size_t x = 0u; x < width; ++x)
            {
                const auto pixel = y * width + x;
                const auto offset = pixel * 4u;
                const auto blue = capture.pixels_bgra8[offset + 0u];
                const auto green = capture.pixels_bgra8[offset + 1u];
                const auto red = capture.pixels_bgra8[offset + 2u];
                const auto luma = lumaAt(pixel);
                sum += luma;
                squared_sum += static_cast<double>(luma) * luma;
                result.minimum_luma = std::min(result.minimum_luma, luma);
                result.maximum_luma = std::max(result.maximum_luma, luma);
                non_black += luma > 4u ? 1u : 0u;
                deep_black += luma <= 4u ? 1u : 0u;
                coarse_colors[
                    ((static_cast<std::size_t>(red) >> 4u) << 8u) |
                    ((static_cast<std::size_t>(green) >> 4u) << 4u) |
                    (static_cast<std::size_t>(blue) >> 4u)] = true;
                if (x + 1u < width)
                {
                    edge_count += std::abs(
                        static_cast<int>(luma) -
                        static_cast<int>(lumaAt(pixel + 1u))) > 12
                        ? 1u
                        : 0u;
                    ++edge_samples;
                }
                if (y + 1u < height)
                {
                    edge_count += std::abs(
                        static_cast<int>(luma) -
                        static_cast<int>(lumaAt(pixel + width))) > 12
                        ? 1u
                        : 0u;
                    ++edge_samples;
                }
            }
        }
        result.mean_luma = sum / static_cast<double>(pixel_count);
        const auto variance = std::max(
            0.0,
            squared_sum / static_cast<double>(pixel_count) -
                result.mean_luma * result.mean_luma);
        result.standard_deviation = std::sqrt(variance);
        result.non_black_fraction = static_cast<double>(non_black) /
            static_cast<double>(pixel_count);
        result.deep_black_fraction = static_cast<double>(deep_black) /
            static_cast<double>(pixel_count);
        result.edge_fraction = edge_samples == 0u
            ? 0.0
            : static_cast<double>(edge_count) /
                static_cast<double>(edge_samples);
        result.coarse_color_count = static_cast<std::uint32_t>(
            std::ranges::count(coarse_colors, true));
        return result;
    }

    [[nodiscard]] bool routeTelemetryReady(
        const lux::game::GameApplicationTelemetry& telemetry) noexcept
    {
        return telemetry.camera_pose_valid &&
            telemetry.spatial3d_catalog_present &&
            telemetry.spatial3d_interest_available &&
            telemetry.spatial3d_camera_interest &&
            telemetry.spatial3d_tracked_sources != 0u &&
            telemetry.spatial3d_active_sections != 0u &&
            telemetry.spatial3d_resident_sections != 0u &&
            telemetry.spatial3d_published_sections != 0u &&
            telemetry.spatial3d_waiting_sections == 0u &&
            telemetry.spatial3d_staging_sections == 0u &&
            telemetry.spatial3d_failed_sections == 0u &&
            telemetry.async_active_operations == 0u &&
            telemetry.render_cluster_available &&
            telemetry.render_clusters != 0u &&
            telemetry.render_instances != 0u &&
            telemetry.world_render_bounds_valid &&
            telemetry.world_classic_render_clusters != 0u &&
            telemetry.world_hlod_render_clusters != 0u &&
            telemetry.gpu_render_candidates_valid &&
            telemetry.gpu_render_candidates_requested != 0u &&
            telemetry.terrain_available &&
            telemetry.terrain_selected_patches_valid &&
            telemetry.terrain_selected_patches != 0u &&
            telemetry.terrain_drawable_pages != 0u &&
            telemetry.skybox_available && telemetry.skybox_draws != 0u &&
            telemetry.light_render_available &&
            telemetry.render_directional_lights != 0u;
    }

    [[nodiscard]] bool captureRouteSemantics(
        lux::game::GameApplication& application,
        const Options& options,
        lux::common::Size2D extent,
        std::optional<EEnvironmentPhase>& applied_environment,
        std::uint32_t run)
    {
        static constexpr std::array checkpoints{
            RouteSemanticCheckpoint{
                "overview-noon", 60u, EEnvironmentPhase::NOON},
            RouteSemanticCheckpoint{
                "street-dusk", 240u, EEnvironmentPhase::DUSK},
            RouteSemanticCheckpoint{
                "ground-night", 480u, EEnvironmentPhase::NIGHT},
            RouteSemanticCheckpoint{
                "flight-fog", 720u, EEnvironmentPhase::DENSE_FOG},
            RouteSemanticCheckpoint{
                "teleport-fog", 900u, EEnvironmentPhase::DENSE_FOG},
            // The boundary pose intentionally hugs a steep terrain cell.  Keep
            // it in daylight so this gate measures streaming continuity rather
            // than the expected near-black result of an unlit night slope.
            RouteSemanticCheckpoint{
                "cell-boundary-noon", 1169u, EEnvironmentPhase::NOON},
            RouteSemanticCheckpoint{
                "cell-boundary-noon-settled", 1169u, EEnvironmentPhase::NOON},
            RouteSemanticCheckpoint{
                "overview-revisit", 60u, EEnvironmentPhase::NOON}};
        const auto capture_root = options.semantic_capture_dir /
            ("run-" + std::to_string(run));
        std::error_code directory_error;
        std::filesystem::create_directories(
            capture_root, directory_error);
        if (directory_error)
            return false;

        // Keep the capture deterministic without freezing runtime state.
        // HLOD/fine and terrain transitions are time based (currently 0.35 s),
        // so a fixed 60 Hz step is required to exercise their production
        // lifecycle at every camera checkpoint.
        constexpr float fixed_dt = 1.0f / 60.0f;
        std::vector<RouteSemanticCapture> captures;
        captures.reserve(checkpoints.size());
        std::vector<std::uint8_t> overview_reference;
        bool infrastructure_ok = true;
        for (std::size_t index = 0u; index < checkpoints.size(); ++index)
        {
            const auto checkpoint = checkpoints[index];
            RouteSemanticCapture record;
            record.checkpoint = checkpoint;
            if (!application.setMainCameraPose(semanticPose(
                    checkpoint,
                    options.world_edge_m)) ||
                !updateEnvironment(
                    application,
                    checkpoint.environment,
                    applied_environment))
            {
                record.failures.emplace_back(
                    "camera/environment update was rejected");
                captures.push_back(std::move(record));
                infrastructure_ok = false;
                break;
            }
            std::optional<lux::game::GameApplicationTelemetry> telemetry;
            const auto minimum_settle_frames = index == 0u ? 180u : 90u;
            // EntitySection staging and the independent render/terrain
            // preparation owners advance under bounded per-frame budgets.
            // Capture only after both registry publication and derived
            // presentation have settled; fail closed after one full route.
            constexpr std::uint32_t maximum_extra_frames = 1200u;
            for (std::uint32_t frame = 0u;
                 frame < minimum_settle_frames + maximum_extra_frames;
                 ++frame)
            {
                lux::window::LuxWindow::pollEvents();
                if (!application.tick(fixed_dt, extent))
                {
                    record.failures.emplace_back(
                        "engine tick failed while settling checkpoint");
                    infrastructure_ok = false;
                    break;
                }
                if (frame + 1u < minimum_settle_frames)
                    continue;
                telemetry = application.telemetrySnapshot();
                if (telemetry && routeTelemetryReady(*telemetry))
                    break;
            }
            if (!infrastructure_ok)
            {
                captures.push_back(std::move(record));
                break;
            }

            auto capture = application.captureMainView(4u);
            telemetry = application.telemetrySnapshot();
            if (!capture || !telemetry)
            {
                record.failures.emplace_back(
                    "offscreen capture or telemetry snapshot failed");
                captures.push_back(std::move(record));
                infrastructure_ok = false;
                break;
            }
            if (!capture->render_graph_dump.empty())
            {
                const auto graph_filename = lux::format(
                    "{:02}-{}-r{:04}-render-graph.txt",
                    index,
                    checkpoint.name,
                    checkpoint.route_frame);
                std::ofstream graph_file(
                    capture_root / graph_filename,
                    std::ios::binary | std::ios::trunc);
                graph_file.write(
                    capture->render_graph_dump.data(),
                    static_cast<std::streamsize>(
                        capture->render_graph_dump.size()));
            }
            record.telemetry = *telemetry;
            const auto filename = lux::format(
                "{:02}-{}-r{:04}.bmp",
                index,
                checkpoint.name,
                checkpoint.route_frame);
            record.image = capture_root / filename;
            if (!writeCaptureBmp(record.image, *capture))
            {
                record.failures.emplace_back("BMP screenshot write failed");
                captures.push_back(std::move(record));
                infrastructure_ok = false;
                break;
            }
            record.image_stats = analyzeCapture(*capture);
            const auto expect = [&](bool condition, const char* message)
            {
                if (!condition)
                    record.failures.emplace_back(message);
            };
            expect(record.telemetry.validation_error_count == 0u,
                "Vulkan validation reported an error");
            expect(record.telemetry.camera_pose_valid,
                "main camera pose is invalid");
            expect(record.telemetry.spatial3d_catalog_present &&
                    record.telemetry.spatial3d_interest_available &&
                    record.telemetry.spatial3d_camera_interest &&
                    record.telemetry.spatial3d_tracked_sources != 0u,
                "Spatial3D EntityScene selection is not driven by the camera");
            expect(record.telemetry.spatial3d_active_sections != 0u &&
                    record.telemetry.spatial3d_resident_sections != 0u &&
                    record.telemetry.spatial3d_published_sections != 0u &&
                    record.telemetry.spatial3d_waiting_sections == 0u &&
                    record.telemetry.spatial3d_staging_sections == 0u &&
                    record.telemetry.spatial3d_failed_sections == 0u,
                "Spatial3D EntitySections did not settle atomically");
            expect(record.telemetry.render_cluster_available &&
                    record.telemetry.render_clusters != 0u &&
                    record.telemetry.render_instances != 0u,
                "World render clusters are not resident");
            expect(record.telemetry.world_render_bounds_valid,
                "World render bounds are unavailable");
            expect(record.telemetry.world_classic_render_clusters != 0u &&
                    record.telemetry.world_hlod_render_clusters != 0u,
                "Classic/HLOD representations are not both resident");
            expect(record.telemetry.gpu_render_candidates_valid &&
                    record.telemetry.gpu_render_candidates_requested != 0u &&
                    record.telemetry.gpu_render_candidates_overflow == 0u,
                "GPU candidate result is missing or overflowed");
            expect(record.telemetry.terrain_available &&
                    record.telemetry.terrain_selected_patches_valid &&
                    record.telemetry.terrain_selected_patches != 0u &&
                    record.telemetry.terrain_drawable_pages != 0u,
                "Terrain has no selected/drawable patches");
            expect(record.telemetry.skybox_available &&
                    record.telemetry.skybox_draws != 0u &&
                    record.telemetry.skybox_pipeline_bind_failures == 0u,
                "Sky rendering is unavailable or failed");
            expect(record.telemetry.light_render_available &&
                    record.telemetry.render_directional_lights != 0u,
                "Sun light is not resident in the renderer");
            expect(record.image_stats.non_black_fraction >= 0.05,
                "capture is predominantly black");
            if (checkpoint.environment == EEnvironmentPhase::NOON)
            {
                expect(record.image_stats.deep_black_fraction <= 0.01,
                    "daylight capture contains continuity holes");
            }
            expect(record.image_stats.maximum_luma -
                    record.image_stats.minimum_luma >= 12u,
                "capture has insufficient luminance range");
            expect(record.image_stats.standard_deviation >= 2.0,
                "capture is nearly flat");
            const auto minimum_coarse_colors =
                checkpoint.environment == EEnvironmentPhase::DENSE_FOG
                    ? 4u
                    : 8u;
            expect(record.image_stats.coarse_color_count >=
                    minimum_coarse_colors,
                "capture contains too few coarse colours");
            // Dense fog deliberately erases most high-frequency edges.  Its
            // correctness is covered by the non-flat image distribution plus
            // resident terrain/cluster state; applying the clear-air edge
            // threshold here would reject the intended effect.
            if (checkpoint.environment != EEnvironmentPhase::DENSE_FOG)
            {
                expect(record.image_stats.edge_fraction >= 0.001,
                    "capture contains no stable geometric edges");
            }
            if (index == 0u)
            {
                overview_reference = capture->pixels_bgra8;
            }
            if (checkpoint.name == "cell-boundary-noon-settled" &&
                !captures.empty() &&
                captures.back().checkpoint.name == "cell-boundary-noon")
            {
                const auto& transition = captures.back().image_stats;
                expect(std::abs(
                        transition.mean_luma -
                        record.image_stats.mean_luma) <= 5.0 &&
                        std::abs(
                            transition.standard_deviation -
                            record.image_stats.standard_deviation) <= 5.0 &&
                        transition.deep_black_fraction <= 0.01,
                    "terrain transition differs materially from settled output");
            }
            else if (index + 1u == checkpoints.size())
            {
                if (overview_reference.size() != capture->pixels_bgra8.size())
                {
                    expect(false, "overview revisit extent changed");
                }
                else
                {
                    std::uint64_t absolute_error = 0u;
                    std::uint64_t changed_pixels = 0u;
                    const auto pixel_count = overview_reference.size() / 4u;
                    for (std::size_t pixel = 0u; pixel < pixel_count; ++pixel)
                    {
                        bool changed = false;
                        for (std::size_t channel = 0u; channel < 3u; ++channel)
                        {
                            const auto offset = pixel * 4u + channel;
                            const auto error = std::abs(
                                static_cast<int>(overview_reference[offset]) -
                                static_cast<int>(capture->pixels_bgra8[offset]));
                            absolute_error += static_cast<std::uint64_t>(error);
                            changed = changed || error > 4;
                        }
                        changed_pixels += changed ? 1u : 0u;
                    }
                    record.revisit_mean_absolute_error =
                        pixel_count == 0u ? 0.0 :
                        static_cast<double>(absolute_error) /
                            static_cast<double>(pixel_count * 3u);
                    record.revisit_changed_fraction =
                        pixel_count == 0u ? 0.0 :
                        static_cast<double>(changed_pixels) /
                            static_cast<double>(pixel_count);
                    expect(record.revisit_mean_absolute_error <= 1.0 &&
                            record.revisit_changed_fraction <= 0.01,
                        "overview revisit differs from the initial capture");
                }
            }
            record.passed = record.failures.empty();
            std::printf(
                "semantic %.*s: image=%s luma=%.2f+/-%.2f range=%u..%u black=%.5f "
                "colors=%u edges=%.5f clusters=%u instances=%u "
                "terrain=%u drawable=%u transitions=%u levels=%u/%u/%u/%u/%u "
                "water=%u passed=%u\n",
                static_cast<int>(checkpoint.name.size()),
                checkpoint.name.data(),
                record.image.string().c_str(),
                record.image_stats.mean_luma,
                record.image_stats.standard_deviation,
                record.image_stats.minimum_luma,
                record.image_stats.maximum_luma,
                record.image_stats.deep_black_fraction,
                record.image_stats.coarse_color_count,
                record.image_stats.edge_fraction,
                record.telemetry.render_clusters,
                record.telemetry.render_instances,
                record.telemetry.terrain_selected_patches,
                record.telemetry.terrain_drawable_pages,
                record.telemetry.terrain_transition_pages,
                record.telemetry.terrain_drawable_pages_by_level[0],
                record.telemetry.terrain_drawable_pages_by_level[1],
                record.telemetry.terrain_drawable_pages_by_level[2],
                record.telemetry.terrain_drawable_pages_by_level[3],
                record.telemetry.terrain_drawable_pages_by_level[4],
                record.telemetry.water_visible_patches,
                record.passed ? 1u : 0u);
            captures.push_back(std::move(record));
        }

        const auto summary_path = capture_root / "semantic-summary.json";
        std::ofstream summary(summary_path, std::ios::trunc);
        if (!summary)
            return false;
        bool passed = infrastructure_ok && captures.size() == checkpoints.size();
        for (const auto& capture : captures)
            passed &= capture.passed;
        summary << "{\"version\":1,\"passed\":"
            << (passed ? "true" : "false")
            << ",\"required\":"
            << (options.require_route_semantics ? "true" : "false")
            << ",\"captures\":[";
        for (std::size_t index = 0u; index < captures.size(); ++index)
        {
            if (index != 0u)
                summary << ',';
            const auto& capture = captures[index];
            summary << "{\"name\":\"" << capture.checkpoint.name
                << "\",\"route_frame\":"
                << capture.checkpoint.route_frame
                << ",\"image\":\""
                << capture.image.filename().string()
                << "\",\"passed\":"
                << (capture.passed ? "true" : "false")
                << ",\"image_stats\":{\"mean_luma\":"
                << capture.image_stats.mean_luma
                << ",\"standard_deviation\":"
                << capture.image_stats.standard_deviation
                << ",\"minimum_luma\":"
                << capture.image_stats.minimum_luma
                << ",\"maximum_luma\":"
                << capture.image_stats.maximum_luma
                << ",\"non_black_fraction\":"
                << capture.image_stats.non_black_fraction
                << ",\"deep_black_fraction\":"
                << capture.image_stats.deep_black_fraction
                << ",\"edge_fraction\":"
                << capture.image_stats.edge_fraction
                << ",\"coarse_color_count\":"
                << capture.image_stats.coarse_color_count
                << ",\"revisit_mean_absolute_error\":"
                << capture.revisit_mean_absolute_error
                << ",\"revisit_changed_fraction\":"
                << capture.revisit_changed_fraction
                << "},\"spatial3d\":{\"catalog_present\":"
                << (capture.telemetry.spatial3d_catalog_present
                        ? "true" : "false")
                << ",\"interest_available\":"
                << (capture.telemetry.spatial3d_interest_available
                        ? "true" : "false")
                << ",\"camera_interest\":"
                << (capture.telemetry.spatial3d_camera_interest
                        ? "true" : "false")
                << ",\"tracked_sources\":"
                << capture.telemetry.spatial3d_tracked_sources
                << ",\"active_sections\":"
                << capture.telemetry.spatial3d_active_sections
                << ",\"resident_sections\":"
                << capture.telemetry.spatial3d_resident_sections
                << ",\"waiting_sections\":"
                << capture.telemetry.spatial3d_waiting_sections
                << ",\"staging_sections\":"
                << capture.telemetry.spatial3d_staging_sections
                << ",\"published_sections\":"
                << capture.telemetry.spatial3d_published_sections
                << ",\"failed_sections\":"
                << capture.telemetry.spatial3d_failed_sections
                << "},\"render\":{\"clusters\":"
                << capture.telemetry.render_clusters
                << ",\"instances\":"
                << capture.telemetry.render_instances
                << ",\"visible_clusters\":"
                << capture.telemetry.visible_render_clusters
                << ",\"visible_instances\":"
                << capture.telemetry.visible_render_instances
                << ",\"classic_clusters\":"
                << capture.telemetry.world_classic_render_clusters
                << ",\"hlod_clusters\":"
                << capture.telemetry.world_hlod_render_clusters
                << ",\"candidate_requested\":"
                << capture.telemetry.gpu_render_candidates_requested
                << ",\"candidate_overflow\":"
                << capture.telemetry.gpu_render_candidates_overflow
                << ",\"terrain_selected_patches\":"
                << capture.telemetry.terrain_selected_patches
                << ",\"terrain_drawable_pages\":"
                << capture.telemetry.terrain_drawable_pages
                << ",\"terrain_transition_pages\":"
                << capture.telemetry.terrain_transition_pages
                << ",\"terrain_drawable_pages_by_level\":["
                << capture.telemetry.terrain_drawable_pages_by_level[0] << ','
                << capture.telemetry.terrain_drawable_pages_by_level[1] << ','
                << capture.telemetry.terrain_drawable_pages_by_level[2] << ','
                << capture.telemetry.terrain_drawable_pages_by_level[3] << ','
                << capture.telemetry.terrain_drawable_pages_by_level[4] << ']'
                << ",\"water_resident_surfaces\":"
                << capture.telemetry.water_resident_surfaces
                << ",\"water_visible_patches\":"
                << capture.telemetry.water_visible_patches
                << ",\"validation_errors\":"
                << capture.telemetry.validation_error_count
                << "},\"failures\":[";
            for (std::size_t failure = 0u;
                 failure < capture.failures.size(); ++failure)
            {
                if (failure != 0u)
                    summary << ',';
                summary << '"' << capture.failures[failure] << '"';
            }
            summary << "]}";
        }
        summary << "]}\n";
        if (!summary)
            return false;
        if (options.require_route_semantics && !passed)
        {
            std::fprintf(
                stderr,
                "route semantic gate failed; inspect %s\n",
                summary_path.string().c_str());
            return false;
        }
        return infrastructure_ok;
    }

    [[nodiscard]] double percentile(
        const std::vector<double>& sorted,
        double quantile) noexcept
    {
        if (sorted.empty())
            return 0.0;
        const auto index = static_cast<std::size_t>(std::ceil(
            quantile * static_cast<double>(sorted.size()))) - 1u;
        return sorted[std::min(index, sorted.size() - 1u)];
    }

    [[nodiscard]] std::uint64_t percentile(
        const std::vector<std::uint64_t>& sorted,
        double quantile) noexcept
    {
        if (sorted.empty())
            return 0u;
        const auto index = static_cast<std::size_t>(std::ceil(
            quantile * static_cast<double>(sorted.size()))) - 1u;
        return sorted[std::min(index, sorted.size() - 1u)];
    }

    [[nodiscard]] std::string_view schedulePhaseName(int phase) noexcept
    {
        if (phase <= lux::ecs::kPhaseInput) return "input";
        if (phase <= lux::ecs::kPhasePreTransform) return "pre_transform";
        if (phase <= lux::ecs::kPhaseSimulation) return "simulation";
        if (phase <= lux::ecs::kPhasePreRender) return "pre_render";
        if (phase <= lux::ecs::kPhaseRender) return "render";
        return "post_render";
    }

    void writeCsvText(std::ostream& output, std::string_view value)
    {
        output << '"';
        for (const char character : value)
        {
            if (character == '"')
                output << "\"\"";
            else
                output << character;
        }
        output << '"';
    }

    void writeJsonText(std::ostream& output, std::string_view value)
    {
        output << '"';
        for (const unsigned char character : value)
        {
            switch (character)
            {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20u)
                    output << '?';
                else
                    output << static_cast<char>(character);
                break;
            }
        }
        output << '"';
    }

    void writeFrameTrace(
        std::ostream& output,
        const lux::runtime::FrameTrace& trace)
    {
        output << "{\"frame_serial\":" << trace.frame_serial
            << ",\"wall_nanoseconds\":" << trace.wall_nanoseconds
            << ",\"attributed_nanoseconds\":"
            << trace.attributedNanoseconds()
            << ",\"phase_nanoseconds\":{";
        for (std::size_t phase = 0u;
             phase < trace.phase_nanoseconds.size(); ++phase)
        {
            if (phase != 0u)
                output << ',';
            writeJsonText(output, kFrameTracePhaseNames[phase]);
            output << ':' << trace.phase_nanoseconds[phase];
        }
        output << "},\"allocation_count\":" << trace.allocation_count
            << ",\"allocation_bytes\":" << trace.allocation_bytes << '}';
    }

    [[nodiscard]] std::optional<RunResult> runOne(
        const Options& options,
        std::uint32_t run,
        lux::window::LuxWindow& window)
    {
#if !defined(__PLATFORM_WIN32__)
        (void)options;
        (void)run;
        (void)window;
        return std::nullopt;
#else
        lux::game::GameApplication application;
        lux::game::GameApplicationConfig config;
        config.title = "lux World Benchmark";
        config.game_pak_file = options.pak;
        config.engine_pak_file = options.engine_pak;
        config.boot_scene = options.scene;
        config.save_root = options.json.parent_path() /
            "Saves" /
            ("session-" + std::to_string(options.session_token)) /
            ("run-" + std::to_string(run));
        if (config.save_root.empty())
            config.save_root = std::filesystem::path{"Saves"} /
                ("run-" + std::to_string(run));
        config.enable_validation = options.validation;
        config.capacity_request.set(
            lux::deployment::kActiveRenderInstancesCapacity,
            lux::deployment::RuntimeCapacityValue::exact(100'000u));
        config.capacity_request.set(
            lux::deployment::kClassicMeshRecordsCapacity,
            lux::deployment::RuntimeCapacityValue::exact(100'000u));
        const auto required =
            lux::window::LuxWindow::requiredVulkanInstanceExtensions();
        for (const auto* extension : required)
            config.vulkan_instance_extensions.emplace_back(extension);
        const auto extent = lux::common::Size2D{
            options.width,
            options.height};
        const auto native = reinterpret_cast<std::uint64_t>(
            window.win32Handle());
        if (!application.start(std::move(config), native, extent))
        {
            std::fprintf(stderr, "engine start failed before benchmark run\n");
            return std::nullopt;
        }
        const auto closeAndFail = [&application]()
            -> std::optional<RunResult>
        {
            if (!application.close())
            {
                std::fprintf(
                    stderr,
                    "engine close failed while aborting benchmark run\n");
            }
            return std::nullopt;
        };
        const auto loading_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!application.sceneReady())
        {
            lux::window::LuxWindow::pollEvents();
            if (!application.tick(0.0f, extent))
            {
                std::fprintf(
                    stderr,
                    "scene loading frame failed before benchmark run\n");
                return closeAndFail();
            }
            (void)application.pumpIdleFor(std::chrono::milliseconds{1});
            if (std::chrono::steady_clock::now() >= loading_deadline)
            {
                std::fprintf(
                    stderr,
                    "scene did not reach READY before benchmark run\n");
                return closeAndFail();
            }
        }
        if (!application.setMainCameraPose(
                routePose(0u, options.world_edge_m)))
        {
            std::fprintf(stderr, "benchmark has no primary 3D camera\n");
            return closeAndFail();
        }
        const auto benchmark_far = std::clamp(
            static_cast<float>(options.world_edge_m) * 1.5f,
            10'000.0f,
            40'000.0f);
        if (!application.setMainCameraClipRange(1.0f, benchmark_far))
        {
            std::fprintf(stderr, "benchmark camera clip range is invalid\n");
            return closeAndFail();
        }

        // Exercise the real ECS-owned navigation path before collecting
        // frames.  Scene READY covers startup Sections only; spatial content
        // may still be decoding/preparing, so PENDING is retried while its
        // region identity remains observable.  A synthetic empty success is
        // never accepted.
        bool navigation_ready = false;
        const auto navigation_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{30};
        while (!navigation_ready &&
               std::chrono::steady_clock::now() < navigation_deadline)
        {
            if (!application.tick(0.0f, extent))
                return closeAndFail();
            (void)application.pumpIdleFor(std::chrono::milliseconds{1});
            const auto path = queryNavigationProbe(
                application, 0u, options.world_edge_m);
            if (!path)
                continue;
            navigation_ready =
                path->status ==
                    lux::navigation::ENavigationPathStatus::COMPLETE ||
                path->status ==
                    lux::navigation::ENavigationPathStatus::PARTIAL;
        }
        if (!navigation_ready)
        {
            std::fprintf(
                stderr,
                "NavigationRegion3D did not produce a real path before "
                "the route deadline\n");
            return closeAndFail();
        }

        const auto spatial_state = application.telemetrySnapshot();
        if (!spatial_state || !spatial_state->spatial3d_catalog_present ||
            !spatial_state->spatial3d_interest_available ||
            !spatial_state->spatial3d_camera_interest)
        {
            std::fprintf(
                stderr,
                "benchmark scene has no camera-driven Spatial3D EntityScene "
                "selection\n");
            return closeAndFail();
        }

        const auto cooked_visual_state = application.visualState();
        if (!cooked_visual_state || !cooked_visual_state->skybox ||
            !cooked_visual_state->directional_light ||
            !cooked_visual_state->height_fog)
        {
            std::fprintf(
                stderr,
                "benchmark scene has no cooked sky/light/fog components\n");
            return closeAndFail();
        }

        constexpr float fixed_dt = 1.0f / 60.0f;
        std::optional<EEnvironmentPhase> applied_environment;
        if (!options.semantic_capture_dir.empty() &&
            !captureRouteSemantics(
                application,
                options,
                extent,
                applied_environment,
                run))
        {
            return closeAndFail();
        }
        auto interactive_deadline = std::chrono::steady_clock::now();
        for (std::uint32_t frame = 0u;
             frame < options.warmup_frames; ++frame)
        {
            lux::window::LuxWindow::pollEvents();
            // Pace before the tick so completions that arrive during the
            // frame wait are adopted by this frame's safe points. Pacing
            // after tick and then sampling left a 16 ms interval in which
            // completed EntitySection generations remained artificially
            // pending.
            paceFrame(
                options.interactive || options.real_time_pacing,
                interactive_deadline);
            const auto route_frame = options.fixed_route_frame.value_or(frame);
            if (!application.setMainCameraPose(routePose(
                    route_frame, options.world_edge_m)))
            {
                std::fprintf(
                    stderr,
                    "camera update failed during warmup frame %u\n",
                    frame);
                lux::log::error(
                    "world_benchmark",
                    "camera update failed during warmup frame {}",
                    frame
                );
                return closeAndFail();
            }
            if (!updateEnvironment(
                application,
                route_frame,
                applied_environment))
            {
                std::fprintf(
                    stderr,
                    "environment update failed during warmup frame %u\n",
                    frame);
                return closeAndFail();
            }
            // setMainCameraPose patches the ECS Transform. The Spatial3D
            // interest system observes its resolved position in this tick and
            // updates EntitySection demand; no World topology side channel is
            // involved.
            if (!application.tick(fixed_dt, extent))
            {
                std::fprintf(
                    stderr,
                    "engine tick failed during warmup frame %u\n",
                    frame);
                lux::log::error(
                    "world_benchmark",
                    "engine tick failed during warmup frame {}",
                    frame
                );
                return closeAndFail();
            }
            if (route_frame % 120u == 0u)
            {
                const auto path = queryNavigationProbe(
                    application, route_frame, options.world_edge_m);
                if (!path || path->status ==
                        lux::navigation::ENavigationPathStatus::FAILED)
                {
                    std::fprintf(
                        stderr,
                        "navigation route query failed during warmup frame "
                        "%u\n",
                        frame);
                    return closeAndFail();
                }
            }
            if (options.dump_graph_frame == frame)
            {
                const auto graph = application.renderGraphDump();
                if (!graph)
                {
                    std::fprintf(
                        stderr,
                        "render graph dump failed after warmup frame %u\n",
                        frame);
                    return closeAndFail();
                }
                std::fwrite(graph->data(), 1u, graph->size(), stdout);
                std::fputc('\n', stdout);
            }
        }

        RunResult result;
        result.capacity_plan = application.capacityPlan();
        result.frame_ms.reserve(options.sample_frames);
        result.route_frames.reserve(options.sample_frames);
        result.frame_traces.reserve(options.sample_frames);
        result.system_frames.reserve(
            static_cast<std::size_t>(options.sample_frames) * 32u);
        for (std::uint32_t frame = 0u;
             frame < options.sample_frames; ++frame)
        {
            lux::window::LuxWindow::pollEvents();
            const auto route_frame = options.fixed_route_frame.value_or(
                options.warmup_frames + frame);
            const auto started = std::chrono::steady_clock::now();
            paceFrame(
                options.interactive || options.real_time_pacing,
                interactive_deadline);
            if (!application.setMainCameraPose(routePose(
                    route_frame, options.world_edge_m)))
            {
                std::fprintf(
                    stderr,
                    "camera update failed during sample frame %u\n",
                    frame);
                lux::log::error(
                    "world_benchmark",
                    "camera update failed during sample frame {}",
                    frame
                );
                return closeAndFail();
            }
            if (!updateEnvironment(
                application,
                route_frame,
                applied_environment))
            {
                std::fprintf(
                    stderr,
                    "environment update failed during sample frame %u\n",
                    frame);
                return closeAndFail();
            }
            // The route itself is the streaming stimulus: Transform3D ->
            // ResolvedTransform3D -> SpatialInterest3D -> Section demand.
            if (!application.tick(fixed_dt, extent))
            {
                std::fprintf(
                    stderr,
                    "engine tick failed during sample frame %u\n",
                    frame);
                lux::log::error(
                    "world_benchmark",
                    "engine tick failed during sample frame {}",
                    frame
                );
                return closeAndFail();
            }
            if (route_frame % 120u == 0u)
            {
                const auto path = queryNavigationProbe(
                    application, route_frame, options.world_edge_m);
                if (!path || path->status ==
                        lux::navigation::ENavigationPathStatus::FAILED)
                {
                    std::fprintf(
                        stderr,
                        "navigation route query failed during sample frame "
                        "%u\n",
                        frame);
                    return closeAndFail();
                }
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            result.frame_ms.push_back(elapsed);
            result.route_frames.push_back(static_cast<std::uint32_t>(
                route_frame % 1200u));
            if (const auto trace = application.latestFrameTrace())
                result.frame_traces.push_back(*trace);
            else
                result.frame_traces.emplace_back();
            for (const auto& system :
                 application.latestScheduleSystemFrameTrace())
            {
                result.system_frames.push_back({
                    frame,
                    static_cast<std::uint32_t>(route_frame % 1200u),
                    system.frame_serial,
                    system.system.hash,
                    system.system.name,
                    system.phase,
                    system.wall_nanoseconds});
            }
            if (route_frame % 1200u == 1199u)
            {
                RunResult::StabilitySnapshot snapshot;
                snapshot.sample_frame = frame;
                snapshot.route_frame = static_cast<std::uint32_t>(
                    route_frame % 1200u);
                snapshot.frame_serial = result.frame_traces.back().frame_serial;
                if (const auto telemetry = application.telemetrySnapshot())
                {
                    snapshot.telemetry = *telemetry;
                    snapshot.has_telemetry = true;
                }
                result.stability_snapshots.push_back(std::move(snapshot));
            }
        }
        if (const auto telemetry = application.telemetrySnapshot())
        {
            result.telemetry = *telemetry;
            result.has_telemetry = true;
        }
        if (const auto gpu = application.gpuTimingDump())
            result.gpu_json = *gpu;
        if (options.validation && result.has_telemetry &&
            result.telemetry.validation_error_count != 0u)
        {
            std::fprintf(
                stderr,
                "Vulkan validation reported %u error(s)\n",
                result.telemetry.validation_error_count);
            return closeAndFail();
        }
        if (!application.close())
        {
            std::fprintf(stderr, "engine close failed after benchmark run\n");
            return std::nullopt;
        }
        return result;
#endif
    }

    void writeCapacityPlan(
        std::ostream& output,
        const lux::deployment::RuntimeCapacityPlan& plan)
    {
        const auto writeDomain = [&output](
            const lux::deployment::RuntimeCapacityDomainPlan& domain)
        {
            output << "{\"id\":\"" << domain.domain.name()
                << "\",\"requested\":" << domain.requested
                << ",\"device_limit\":" << domain.device_limit
                << ",\"protocol_limit\":" << domain.protocol_limit
                << ",\"effective\":" << domain.effective
                << ",\"estimated_bytes\":" << domain.estimated_bytes
                << ",\"reason\":"
                << static_cast<std::uint32_t>(domain.reason) << '}';
        };
        output << "{\"device\":{\"vram_budget_bytes\":"
            << plan.device.vram_budget_bytes
            << ",\"vram_usage_bytes\":" << plan.device.vram_usage_bytes
            << ",\"max_storage_buffer_range\":"
            << plan.device.max_storage_buffer_range
            << ",\"buffer_device_address\":"
            << (plan.device.buffer_device_address ? "true" : "false")
            << ",\"shader_int64\":"
            << (plan.device.shader_int64 ? "true" : "false")
            << "},\"domains\":[";
        for (std::size_t index = 0u; index < plan.domains.size(); ++index)
        {
            if (index != 0u)
                output << ',';
            writeDomain(plan.domains[index]);
        }
        output << "]}";
    }

    using StabilityMetrics = std::map<std::string, std::uint64_t>;

    struct StabilityFailure final
    {
        std::size_t lap{0u};
        std::string metric;
        std::uint64_t reference{0u};
        std::uint64_t actual{0u};
    };

    struct StabilityAssessment final
    {
        bool sufficient_samples{false};
        bool passed{false};
        std::vector<StabilityFailure> failures;
    };

    struct AcceptanceFailure final
    {
        std::string gate;
        std::size_t run{0u};
        double required{0.0};
        double actual{0.0};
        std::uint64_t failure_count{0u};
        bool sufficient_samples{true};
    };

    void addTelemetryStabilityMetrics(
        StabilityMetrics& result,
        const lux::game::GameApplicationTelemetry& value)
    {
        result["handles.render_clusters"] = value.render_clusters;
        result["handles.render_instances"] = value.render_instances;
        result["handles.actor_instances"] = value.actor_render_instances;
        result["handles.directional_lights"] =
            value.render_directional_lights;
        result["handles.point_lights"] = value.render_point_lights;
        result["handles.spot_lights"] = value.render_spot_lights;
        result["handles.terrain_pages"] = value.terrain_resident_pages;
        result["handles.water_surfaces"] = value.water_resident_surfaces;
        result["navigation.active_regions"] =
            value.navigation_active_regions;
        result["navigation.staging_regions"] =
            value.navigation_staging_regions;
        result["navigation.retiring_regions"] =
            value.navigation_retiring_regions;
        result["navigation.owner_bytes"] =
            value.navigation_owner_bytes;
        result["spatial.active_sections"] =
            value.spatial3d_active_sections;
        result["spatial.resident_sections"] =
            value.spatial3d_resident_sections;
        result["spatial.published_sections"] =
            value.spatial3d_published_sections;
        result["texture.target_bytes"] = value.texture_target_bytes;
        result["texture.actual_bytes"] = value.texture_actual_bytes;
    }

    [[nodiscard]] StabilityMetrics stabilityMetrics(
        const RunResult::StabilitySnapshot& snapshot)
    {
        StabilityMetrics result;
        if (snapshot.has_telemetry)
            addTelemetryStabilityMetrics(result, snapshot.telemetry);
        return result;
    }

    [[nodiscard]] std::uint64_t medianOfFive(
        std::array<std::uint64_t, 5u> values) noexcept
    {
        std::ranges::sort(values);
        return values[2u];
    }

    [[nodiscard]] bool withinPercent(
        std::uint64_t reference,
        std::uint64_t actual,
        double percent) noexcept
    {
        if (reference == 0u)
            return actual == 0u;
        const auto delta = actual > reference
            ? actual - reference
            : reference - actual;
        return static_cast<long double>(delta) * 100.0L <=
            static_cast<long double>(reference) * percent;
    }

    [[nodiscard]] StabilityAssessment assessStability(
        const RunResult& run,
        double percent)
    {
        StabilityAssessment result;
        if (run.stability_snapshots.size() < 6u)
            return result;
        for (const auto& snapshot : run.stability_snapshots)
        {
            if (!snapshot.has_telemetry)
                return result;
        }
        result.sufficient_samples = true;
        std::array<StabilityMetrics, 5u> references;
        for (std::size_t index = 0u; index < references.size(); ++index)
            references[index] = stabilityMetrics(run.stability_snapshots[index]);
        StabilityMetrics median;
        for (const auto& [name, ignored] : references[0u])
        {
            (void)ignored;
            std::array<std::uint64_t, 5u> values{};
            bool complete = true;
            for (std::size_t index = 0u; index < references.size(); ++index)
            {
                const auto found = references[index].find(name);
                if (found == references[index].end())
                {
                    complete = false;
                    break;
                }
                values[index] = found->second;
            }
            if (complete)
                median.emplace(name, medianOfFive(values));
        }
        for (std::size_t lap = references.size();
             lap < run.stability_snapshots.size(); ++lap)
        {
            const auto metrics = stabilityMetrics(run.stability_snapshots[lap]);
            for (const auto& [name, reference] : median)
            {
                if (name.ends_with("high_water_bytes") ||
                    name.ends_with("allocation_high_water"))
                {
                    continue;
                }
                const auto found = metrics.find(name);
                const auto actual = found == metrics.end()
                    ? std::numeric_limits<std::uint64_t>::max()
                    : found->second;
                if (!withinPercent(reference, actual, percent))
                {
                    result.failures.push_back({
                        lap,
                        name,
                        reference,
                        actual});
                }
            }
        }
        // A high-water value can settle above the five-lap median while still
        // remaining inside the tolerance band. What is not acceptable is an
        // unbounded trend hidden by that band. Three consecutive post-reference
        // increases are reported as a distinct stability failure.
        auto previous = stabilityMetrics(run.stability_snapshots[4u]);
        std::map<std::string, std::uint32_t> growth_streak;
        for (std::size_t lap = references.size();
             lap < run.stability_snapshots.size(); ++lap)
        {
            const auto metrics = stabilityMetrics(run.stability_snapshots[lap]);
            for (const auto& [name, actual] : metrics)
            {
                if (!name.ends_with("high_water_bytes") &&
                    !name.ends_with("allocation_high_water"))
                {
                    continue;
                }
                const auto prior = previous.find(name);
                auto& streak = growth_streak[name];
                if (prior != previous.end() && actual > prior->second)
                    ++streak;
                else
                    streak = 0u;
                if (streak == 3u)
                {
                    result.failures.push_back({
                        lap,
                        name + ".continuous_growth",
                        prior == previous.end() ? 0u : prior->second,
                        actual});
                }
            }
            previous = std::move(metrics);
        }
        result.passed = result.failures.empty();
        return result;
    }

    void writeStabilityAssessment(
        std::ostream& output,
        const RunResult& run,
        const StabilityAssessment& assessment,
        double percent)
    {
        output << "{\"required_percent\":" << percent
            << ",\"lap_count\":" << run.stability_snapshots.size()
            << ",\"reference_laps\":5,\"sufficient_samples\":"
            << (assessment.sufficient_samples ? "true" : "false")
            << ",\"passed\":" << (assessment.passed ? "true" : "false")
            << ",\"failures\":[";
        for (std::size_t index = 0u;
             index < assessment.failures.size(); ++index)
        {
            if (index != 0u)
                output << ',';
            const auto& failure = assessment.failures[index];
            output << "{\"lap\":" << failure.lap
                << ",\"metric\":\"" << failure.metric
                << "\",\"reference\":" << failure.reference
                << ",\"actual\":" << failure.actual << '}';
        }
        output << "]}";
    }

    void writeTelemetry(
        std::ostream& output,
        const lux::game::GameApplicationTelemetry& value)
    {
        output << "{\"frame\":{\"opened\":" << value.frame_opened
            << ",\"submitted\":" << value.frame_submitted
            << ",\"slot_wait_ns\":"
            << value.frame_slot_wait_nanoseconds
            << ",\"slot_wait_max_ns\":"
            << value.frame_slot_wait_max_nanoseconds
            << ",\"validation_errors\":"
            << value.validation_error_count
            << ",\"camera_pose_valid\":"
            << (value.camera_pose_valid ? "true" : "false")
            << ",\"camera_position\":[" << value.camera_position_x << ','
            << value.camera_position_y << ',' << value.camera_position_z
            << "],\"camera_forward\":[" << value.camera_forward_x << ','
            << value.camera_forward_y << ',' << value.camera_forward_z
            << "]},\"spatial3d\":{\"catalog_present\":"
            << (value.spatial3d_catalog_present ? "true" : "false")
            << ",\"interest_available\":"
            << (value.spatial3d_interest_available ? "true" : "false")
            << ",\"camera_interest\":"
            << (value.spatial3d_camera_interest ? "true" : "false")
            << ",\"tracked_sources\":"
            << value.spatial3d_tracked_sources
            << ",\"active_sections\":"
            << value.spatial3d_active_sections
            << ",\"resident_sections\":"
            << value.spatial3d_resident_sections
            << ",\"waiting_sections\":"
            << value.spatial3d_waiting_sections
            << ",\"staging_sections\":"
            << value.spatial3d_staging_sections
            << ",\"published_sections\":"
            << value.spatial3d_published_sections
            << ",\"failed_sections\":"
            << value.spatial3d_failed_sections
            << "},\"async\":{\"accepted\":" << value.async_accepted
            << ",\"rejected\":" << value.async_rejected
            << ",\"active_operations\":"
            << value.async_active_operations
            << ",\"queue_high_water\":"
            << value.async_queue_high_water
            << ",\"byte_high_water\":"
            << value.async_byte_high_water
            << ",\"main_queue_high_water\":"
            << value.async_main_queue_high_water
            << ",\"coordinator_handler_ns\":"
            << value.async_coordinator_handler_nanoseconds
            << ",\"coordinator_handler_max_ns\":"
            << value.async_coordinator_handler_max_nanoseconds
            << ",\"blocking_io_running\":"
            << value.async_blocking_io_running
            << ",\"background_cpu_running\":"
            << value.async_background_cpu_running
            << "},\"upload\":{\"submitted_packets\":"
            << value.upload_submitted_packets
            << ",\"shared_bytes\":" << value.upload_shared_bytes
            << ",\"copied_bytes\":" << value.upload_copied_bytes
            << ",\"pending_backpressure\":"
            << value.upload_pending_backpressure
            << ",\"active_replies\":" << value.upload_active_replies
            << ",\"accepted_inflight\":"
            << value.upload_accepted_inflight
            << ",\"retry_attempts\":" << value.upload_retry_attempts
            << ",\"retry_high_water\":"
            << value.upload_retry_high_water
            << ",\"queue_high_water\":"
            << value.upload_queue_high_water
            << ",\"payload_high_water\":"
            << value.upload_payload_high_water
            << "},\"physics3d\":{\"dynamic_bodies\":"
            << value.physics_dynamic_bodies
            << ",\"characters\":" << value.physics_characters
            << ",\"static_heightfield_bodies\":"
            << value.physics_static_heightfield_bodies
            << ",\"capacity_bytes\":"
            << value.physics_capacity_bytes
            << ",\"allocation_count\":"
            << value.physics_allocation_count
            << "},\"navigation3d\":{\"generation\":"
            << value.navigation_generation
            << ",\"waiting_regions\":"
            << value.navigation_waiting_regions
            << ",\"staging_regions\":"
            << value.navigation_staging_regions
            << ",\"ready_regions\":"
            << value.navigation_ready_regions
            << ",\"active_regions\":"
            << value.navigation_active_regions
            << ",\"retiring_regions\":"
            << value.navigation_retiring_regions
            << ",\"requests_emitted\":"
            << value.navigation_requests_emitted
            << ",\"queue_backpressure\":"
            << value.navigation_queue_backpressure
            << ",\"stale_completions\":"
            << value.navigation_stale_completions
            << ",\"failed_regions\":"
            << value.navigation_failed_regions
            << ",\"staging_work_items\":"
            << value.navigation_staging_work_items
            << ",\"retirement_work_items\":"
            << value.navigation_retirement_work_items
            << ",\"staging_bytes\":"
            << value.navigation_staging_bytes
            << ",\"retired_bytes\":"
            << value.navigation_retired_bytes
            << ",\"close_hides\":"
            << value.navigation_close_hides
            << ",\"owner_bytes\":"
            << value.navigation_owner_bytes
            << ",\"maximum_staging_work_items_per_tick\":"
            << value.navigation_maximum_staging_work_items_per_tick
            << ",\"maximum_retirement_work_items_per_tick\":"
            << value.navigation_maximum_retirement_work_items_per_tick
            << ",\"maximum_close_hides_per_tick\":"
            << value.navigation_maximum_close_hides_per_tick
            << ",\"queries_submitted\":"
            << value.navigation_queries_submitted
            << ",\"queries_completed\":"
            << value.navigation_queries_completed
            << ",\"queries_failed\":"
            << value.navigation_queries_failed
            << ",\"complete_paths\":"
            << value.navigation_queries_complete_paths
            << ",\"partial_paths\":"
            << value.navigation_queries_partial_paths
            << ",\"pending_paths\":"
            << value.navigation_queries_pending_paths
            << ",\"last_path_points\":"
            << value.navigation_last_path_points
            << ",\"last_missing_regions\":"
            << value.navigation_last_missing_regions
            << "},\"ecs\":{\"root_3d_transforms\":"
            << value.root_3d_transforms
            << ",\"point_lights\":" << value.point_lights
            << ",\"spot_lights\":" << value.spot_lights
            << ",\"directional_lights\":" << value.directional_lights
            << ",\"water_surfaces\":" << value.water_surfaces
            << ",\"rigid_bodies_3d\":" << value.rigid_bodies_3d
            << ",\"character_controllers_3d\":"
            << value.character_controllers_3d
            << ",\"sky_revision\":" << value.sky_revision
            << ",\"directional_light_revision\":"
            << value.directional_light_revision
            << ",\"height_fog_revision\":"
            << value.height_fog_revision
            << "},\"render_cluster\":{\"available\":"
            << (value.render_cluster_available ? "true" : "false")
            << ",\"clusters\":" << value.render_clusters
            << ",\"instances\":" << value.render_instances
            << ",\"visible_clusters\":"
            << value.visible_render_clusters
            << ",\"visible_instances\":"
            << value.visible_render_instances
            << ",\"world_classic_clusters\":"
            << value.world_classic_render_clusters
            << ",\"world_hlod_clusters\":"
            << value.world_hlod_render_clusters
            << ",\"world_classic_instances\":"
            << value.world_classic_render_instances
            << ",\"world_hlod_instances\":"
            << value.world_hlod_render_instances
            << ",\"world_bounds_valid\":"
            << (value.world_render_bounds_valid ? "true" : "false")
            << ",\"world_min\":[" << value.world_render_min_x << ','
            << value.world_render_min_y << ',' << value.world_render_min_z
            << "],\"world_max\":[" << value.world_render_max_x << ','
            << value.world_render_max_y << ',' << value.world_render_max_z
            << ']'
            << ",\"gpu_candidates\":" << value.gpu_render_candidates
            << ",\"gpu_candidates_requested\":"
            << value.gpu_render_candidates_requested
            << ",\"gpu_candidates_overflow\":"
            << value.gpu_render_candidates_overflow
            << ",\"gpu_candidate_groups\":"
            << value.gpu_render_candidate_groups
            << ",\"gpu_candidates_valid\":"
            << (value.gpu_render_candidates_valid ? "true" : "false")
            << ",\"cull_visible_flags\":"
            << value.cull_visible_flag_instances
            << ",\"cull_gbuffer_pass\":"
            << value.cull_gbuffer_pass_instances
            << ",\"cull_geometry\":"
            << value.cull_geometry_instances
            << ",\"cull_lod\":" << value.cull_lod_instances
            << ",\"cull_mdc\":" << value.cull_mdc_instances
            << ",\"cull_frustum\":"
            << value.cull_frustum_instances
            << ",\"non_white_instances\":"
            << value.non_white_render_instances
            << ",\"instance_rgba8_xor\":"
            << value.render_instance_rgba8_xor
            << ",\"wanted_mip_textures\":"
            << value.wanted_mip_textures
            << ",\"minimum_wanted_mip\":" << value.minimum_wanted_mip
            << ",\"workgroup_aggregation_fallbacks\":"
            << value.workgroup_aggregation_fallbacks
            << ",\"wanted_mip_feedback_valid\":"
            << (value.wanted_mip_feedback_valid ? "true" : "false")
            << ",\"texture_full_bytes\":" << value.texture_full_bytes
            << ",\"texture_target_bytes\":" << value.texture_target_bytes
            << ",\"texture_actual_bytes\":" << value.texture_actual_bytes
            << ",\"cpu_capacity_bytes\":"
            << value.render_cluster_cpu_capacity_bytes
            << ",\"cpu_allocation_count\":"
            << value.render_cluster_cpu_allocation_count
            << "},\"actor_render\":{\"available\":"
            << (value.mesh_stack_available ? "true" : "false")
            << ",\"instances\":" << value.actor_render_instances
            << ",\"transitioning_instances\":"
            << value.actor_transitioning_instances
            << ",\"resource_bound_instances\":"
            << value.actor_resource_bound_instances
            << ",\"transparent_hard_cuts\":"
            << value.transparent_actor_hard_cuts
            << ",\"vbo_segments\":" << value.mesh_vbo_segments
            << ",\"ibo_segments\":" << value.mesh_ibo_segments
            << ",\"vbo_growths\":" << value.mesh_vbo_growths
            << ",\"ibo_growths\":" << value.mesh_ibo_growths
            << ",\"vbo_used_bytes\":" << value.mesh_vbo_used_bytes
            << ",\"vbo_free_bytes\":" << value.mesh_vbo_free_bytes
            << ",\"vbo_largest_free_block\":"
            << value.mesh_vbo_largest_free_block
            << ",\"vbo_fragmentation\":"
            << value.mesh_vbo_fragmentation
            << ",\"ibo_used_bytes\":" << value.mesh_ibo_used_bytes
            << ",\"ibo_free_bytes\":" << value.mesh_ibo_free_bytes
            << ",\"ibo_largest_free_block\":"
            << value.mesh_ibo_largest_free_block
            << ",\"ibo_fragmentation\":"
            << value.mesh_ibo_fragmentation
            << "},\"light_render\":{\"available\":"
            << (value.light_render_available ? "true" : "false")
            << ",\"directional\":" << value.render_directional_lights
            << ",\"point\":" << value.render_point_lights
            << ",\"spot\":" << value.render_spot_lights
            << ",\"area\":" << value.render_area_lights
            << ",\"transitioning\":" << value.transitioning_lights
            << "},\"skybox\":{\"available\":"
            << (value.skybox_available ? "true" : "false")
            << ",\"active_mode\":" << value.skybox_active_mode
            << ",\"bindless_index\":" << value.skybox_bindless_index
            << ",\"pass_visits\":" << value.skybox_pass_visits
            << ",\"draws\":" << value.skybox_draws
            << ",\"inactive_pass_visits\":"
            << value.skybox_inactive_pass_visits
            << ",\"pipeline_bind_failures\":"
            << value.skybox_pipeline_bind_failures
            << ",\"intensity\":" << value.skybox_intensity
            << "},\"terrain\":{\"available\":"
            << (value.terrain_available ? "true" : "false")
            << ",\"resident_pages\":" << value.terrain_resident_pages
            << ",\"full_resolution_pages\":"
            << value.terrain_full_resolution_pages
            << ",\"fallback_pages\":" << value.terrain_fallback_pages
            << ",\"selected_patches\":"
            << value.terrain_selected_patches
            << ",\"selected_patches_valid\":"
            << (value.terrain_selected_patches_valid ? "true" : "false")
            << ",\"fine_pages\":" << value.terrain_fine_pages
            << ",\"hlod_pages\":" << value.terrain_hlod_pages
            << ",\"drawable_pages\":" << value.terrain_drawable_pages
            << ",\"drawable_pages_by_level\":["
            << value.terrain_drawable_pages_by_level[0] << ','
            << value.terrain_drawable_pages_by_level[1] << ','
            << value.terrain_drawable_pages_by_level[2] << ','
            << value.terrain_drawable_pages_by_level[3] << ','
            << value.terrain_drawable_pages_by_level[4] << ']'
            << ",\"transition_pages\":" << value.terrain_transition_pages
            << ",\"view_surface_valid\":"
            << (value.terrain_view_surface_valid ? "true" : "false")
            << ",\"view_surface_level\":"
            << value.terrain_view_surface_level
            << ",\"render_view_page\":["
            << value.terrain_view_page_x << ','
            << value.terrain_view_page_y << ','
            << value.terrain_view_page_z << ']'
            << ",\"render_view_local\":["
            << value.terrain_view_local_x << ','
            << value.terrain_view_local_y << ','
            << value.terrain_view_local_z << ']'
            << ",\"view_surface_height\":"
            << value.terrain_view_surface_height
            << ",\"view_surface_clearance\":"
            << value.terrain_view_surface_clearance
            << ",\"cpu_bytes\":" << value.terrain_cpu_bytes
            << ",\"gpu_bytes\":" << value.terrain_gpu_bytes
            << "},\"water\":{\"available\":"
            << (value.water_available ? "true" : "false")
            << ",\"resident_surfaces\":"
            << value.water_resident_surfaces
            << ",\"visible_patches\":" << value.water_visible_patches
            << ",\"transitioning_surfaces\":"
            << value.water_transitioning_surfaces
            << ",\"transparent_hard_cuts\":"
            << value.transparent_hard_cuts
            << ",\"cpu_bytes\":" << value.water_cpu_bytes
            << ",\"gpu_capacity_bytes\":"
            << value.water_gpu_capacity_bytes << "}}";
    }
} // namespace

int main(int argc, char** argv)
{
    auto options = parse(argc, argv);
    if (!options)
    {
        if (argc > 1 && (std::string_view{argv[1]} == "--help" ||
                        std::string_view{argv[1]} == "-h"))
            return 0;
        usage();
        return 2;
    }
    if (!std::filesystem::exists(options->pak))
    {
        std::fprintf(stderr, "benchmark pak does not exist: %s\n",
            options->pak.string().c_str());
        return 2;
    }
    if (!options->engine_pak.empty() &&
        !std::filesystem::exists(options->engine_pak))
    {
        std::fprintf(stderr, "engine pak does not exist: %s\n",
            options->engine_pak.string().c_str());
        return 2;
    }
    options->session_token = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    lux::log::setOutput([](const lux::log::LogRecord& record)
    { lux::log::writeRecordToStderr(record); });
    lux::window::GlfwRuntime glfw;
    if (!glfw.valid())
        return 1;
    lux::window::LuxWindow window(
        static_cast<int>(options->width),
        static_cast<int>(options->height),
        "lux World Benchmark");
    if (!window.isInitialized())
        return 1;
    if (!options->interactive)
        window.hide(true);

    if (!options->json.parent_path().empty())
        std::filesystem::create_directories(options->json.parent_path());
    if (!options->csv.parent_path().empty())
        std::filesystem::create_directories(options->csv.parent_path());

    std::vector<RunResult> runs;
    runs.reserve(options->repeat);
    for (std::uint32_t run = 0u; run < options->repeat; ++run)
    {
        std::printf("benchmark run %u/%u\n", run + 1u, options->repeat);
        auto result = runOne(*options, run, window);
        if (!result)
            return 1;
        runs.push_back(std::move(*result));
    }

    std::ofstream json(options->json, std::ios::trunc);
    std::ofstream csv(options->csv, std::ios::trunc);
    const auto systems_csv_path = options->csv.parent_path() /
        "systems.csv";
    std::ofstream systems_csv(systems_csv_path, std::ios::trunc);
    if (!json || !csv || !systems_csv)
        return 1;
    csv << "run,frame,route_frame,frame_serial,cpu_frame_ms,trace_wall_ns,"
           "trace_attributed_ns,control_reply_pump_ns,frame_reply_pump_ns,"
           "pending_submit_ns,frame_slot_wait_ns,frame_open_ns,"
           "main_completion_ns,event_drain_ns,texture_streaming_ns,"
           "contribution_safe_point_ns,integration_safe_point_ns,"
           "schedule_input_ns,schedule_pre_transform_ns,"
           "schedule_simulation_ns,schedule_pre_render_ns,"
           "schedule_render_ns,schedule_post_render_ns,command_barrier_ns,"
           "frame_submit_ns,allocation_count,allocation_bytes\n";
    systems_csv << "run,frame,route_frame,frame_serial,system_hash,"
                   "system_name,phase,wall_ns\n";
    json << "{\"version\":5,\"fixed_dt_hz\":60,\"world_edge_m\":"
        << options->world_edge_m << ",\"extent\":["
        << options->width << ',' << options->height
        << "],\"warmup_frames\":" << options->warmup_frames
        << ",\"sample_frames\":" << options->sample_frames
        << ",\"real_time_pacing\":"
        << (options->real_time_pacing ? "true" : "false")
        << ",\"route_frames\":1200,\"systems_csv\":";
    writeJsonText(json, systems_csv_path.filename().string());
    json << ",\"required_p99_attribution\":";
    if (options->required_p99_attribution)
        json << *options->required_p99_attribution;
    else
        json << "null";
    json << ",\"required_stability_percent\":";
    if (options->required_stability_percent)
        json << *options->required_stability_percent;
    else
        json << "null";
    json << ",\"frame_trace_phase_names\":["
           "\"control_reply_pump\",\"frame_reply_pump\","
           "\"pending_submit\",\"frame_slot_wait\",\"frame_open\","
           "\"main_completion\",\"event_drain\",\"texture_streaming\","
           "\"contribution_safe_point\","
           "\"integration_safe_point\",\"schedule_input\","
           "\"schedule_pre_transform\",\"schedule_simulation\","
           "\"schedule_pre_render\",\"schedule_render\","
           "\"schedule_post_render\",\"command_barrier\","
           "\"frame_submit\"],\"environment_phases\":["
           "\"noon\",\"dusk\",\"night\",\"dense_fog\","
           "\"clear_night\"],\"runs\":[";
    bool acceptance_failed = false;
    std::vector<AcceptanceFailure> acceptance_failures;
    for (std::size_t index = 0u; index < runs.size(); ++index)
    {
        auto sorted = runs[index].frame_ms;
        std::ranges::sort(sorted);
        if (index != 0u)
            json << ',';
        const auto p99 = percentile(sorted, 0.99);
        double p99_attribution_min = 1.0;
        std::uint32_t p99_trace_count = 0u;
        std::uint32_t p99_failed_frames = 0u;
        std::vector<std::size_t> p99_frames;
        for (std::size_t frame = 0u;
             frame < runs[index].frame_ms.size() &&
             frame < runs[index].frame_traces.size(); ++frame)
        {
            if (runs[index].frame_ms[frame] < p99)
                continue;
            p99_frames.push_back(frame);
            const auto& trace = runs[index].frame_traces[frame];
            const auto ratio = trace.frame_serial == 0u ||
                    trace.wall_nanoseconds == 0u
                ? 0.0
                : std::min(
                    1.0,
                    static_cast<double>(trace.attributedNanoseconds()) /
                        static_cast<double>(trace.wall_nanoseconds));
            p99_attribution_min = std::min(p99_attribution_min, ratio);
            if (options->required_p99_attribution &&
                ratio < *options->required_p99_attribution)
            {
                ++p99_failed_frames;
            }
            ++p99_trace_count;
        }
        std::ranges::sort(
            p99_frames,
            [&](std::size_t left, std::size_t right)
            {
                const auto& left_trace = runs[index].frame_traces[left];
                const auto& right_trace = runs[index].frame_traces[right];
                if (left_trace.wall_nanoseconds != right_trace.wall_nanoseconds)
                {
                    return left_trace.wall_nanoseconds >
                        right_trace.wall_nanoseconds;
                }
                return runs[index].frame_ms[left] >
                    runs[index].frame_ms[right];
            });
        const auto p99_attribution_passed =
            !options->required_p99_attribution ||
            (p99_trace_count != 0u && p99_attribution_min >=
                *options->required_p99_attribution);
        if (!p99_attribution_passed)
        {
            acceptance_failed = true;
            acceptance_failures.push_back({
                "p99_attribution",
                index,
                options->required_p99_attribution.value_or(0.0),
                p99_trace_count == 0u ? 0.0 : p99_attribution_min,
                p99_failed_frames,
                p99_trace_count != 0u});
        }
        const auto stability_percent =
            options->required_stability_percent.value_or(5.0);
        const auto stability = assessStability(runs[index], stability_percent);
        if (options->required_stability_percent && !stability.passed)
        {
            acceptance_failed = true;
            acceptance_failures.push_back({
                "stability_percent",
                index,
                *options->required_stability_percent,
                0.0,
                stability.failures.size(),
                stability.sufficient_samples});
        }
        json << "{\"run\":" << index
            << ",\"frame_p50_ms\":" << percentile(sorted, 0.50)
            << ",\"frame_p95_ms\":" << percentile(sorted, 0.95)
            << ",\"frame_p99_ms\":" << p99
            << ",\"frame_peak_ms\":"
            << (sorted.empty() ? 0.0 : sorted.back())
            << ",\"p99_trace_count\":" << p99_trace_count
            << ",\"p99_attribution_min\":"
            << (p99_trace_count == 0u ? 0.0 : p99_attribution_min)
            << ",\"p99_attribution_passed\":"
            << (p99_attribution_passed ? "true" : "false")
            << ",\"p99_frames\":[";
        for (std::size_t rank = 0u; rank < p99_frames.size(); ++rank)
        {
            if (rank != 0u)
                json << ',';
            const auto frame = p99_frames[rank];
            const auto& trace = runs[index].frame_traces[frame];
            const auto attributed = std::min(
                trace.attributedNanoseconds(),
                trace.wall_nanoseconds);
            const auto ratio = trace.frame_serial == 0u ||
                    trace.wall_nanoseconds == 0u
                ? 0.0
                : static_cast<double>(attributed) /
                    static_cast<double>(trace.wall_nanoseconds);
            std::vector<std::pair<std::size_t, std::uint64_t>> contributions;
            contributions.reserve(trace.phase_nanoseconds.size());
            for (std::size_t phase = 0u;
                 phase < trace.phase_nanoseconds.size(); ++phase)
            {
                if (trace.phase_nanoseconds[phase] != 0u)
                {
                    contributions.emplace_back(
                        phase,
                        trace.phase_nanoseconds[phase]);
                }
            }
            std::ranges::sort(
                contributions,
                [](const auto& left, const auto& right)
                {
                    if (left.second != right.second)
                        return left.second > right.second;
                    return left.first < right.first;
                });
            json << "{\"rank\":" << rank
                << ",\"frame\":" << frame
                << ",\"route_frame\":"
                << runs[index].route_frames[frame]
                << ",\"serial\":" << trace.frame_serial
                << ",\"cpu_frame_ms\":" << runs[index].frame_ms[frame]
                << ",\"trace_wall_ns\":" << trace.wall_nanoseconds
                << ",\"attribution_ratio\":" << ratio
                << ",\"unattributed_ns\":"
                << (trace.wall_nanoseconds - attributed)
                << ",\"contributions\":[";
            for (std::size_t contribution = 0u;
                 contribution < contributions.size(); ++contribution)
            {
                if (contribution != 0u)
                    json << ',';
                const auto [phase, nanoseconds] =
                    contributions[contribution];
                json << "{\"phase\":\""
                    << kFrameTracePhaseNames[phase]
                    << "\",\"nanoseconds\":" << nanoseconds << '}';
            }
            json << "]}";
        }
        json << ']';

        std::map<std::string, std::vector<std::uint64_t>> system_samples;
        for (const auto& sample : runs[index].system_frames)
            system_samples[std::string{sample.system_name}].push_back(
                sample.wall_nanoseconds);
        std::vector<std::pair<std::string, std::uint64_t>> system_p99;
        system_p99.reserve(system_samples.size());
        for (auto& [name, samples] : system_samples)
        {
            std::ranges::sort(samples);
            system_p99.emplace_back(name, percentile(samples, 0.99));
        }
        std::ranges::sort(
            system_p99,
            [](const auto& left, const auto& right)
            {
                if (left.second != right.second)
                    return left.second > right.second;
                return left.first < right.first;
            });

        json << ",\"p99_contributions\":{\"systems\":[";
        for (std::size_t rank = 0u; rank < system_p99.size(); ++rank)
        {
            if (rank != 0u)
                json << ',';
            json << "{\"system\":";
            writeJsonText(json, system_p99[rank].first);
            json << ",\"p99_nanoseconds\":" << system_p99[rank].second
                << '}';
        }
        json << "],\"domains\":[]},\"capacity_plan\":";
        writeCapacityPlan(json, runs[index].capacity_plan);
        json << ",\"telemetry\":";
        if (runs[index].has_telemetry)
            writeTelemetry(json, runs[index].telemetry);
        else
            json << "null";
        json << ",\"gpu\":" << runs[index].gpu_json
            << ",\"stability_enforced\":"
            << (options->required_stability_percent ? "true" : "false")
            << ",\"stability\":";
        writeStabilityAssessment(
            json,
            runs[index],
            stability,
            stability_percent);
        json << ",\"stability_snapshots\":[";
        for (std::size_t lap = 0u;
             lap < runs[index].stability_snapshots.size(); ++lap)
        {
            if (lap != 0u)
                json << ',';
            const auto& snapshot = runs[index].stability_snapshots[lap];
            json << "{\"lap\":" << lap
                << ",\"sample_frame\":" << snapshot.sample_frame
                << ",\"route_frame\":" << snapshot.route_frame
                << ",\"frame_serial\":" << snapshot.frame_serial;
            json << ",\"telemetry\":";
            if (snapshot.has_telemetry)
                writeTelemetry(json, snapshot.telemetry);
            else
                json << "null";
            json << '}';
        }
        json << "],\"frame_traces\":[";
        for (std::size_t frame = 0u;
             frame < runs[index].frame_traces.size(); ++frame)
        {
            if (frame != 0u)
                json << ',';
            writeFrameTrace(json, runs[index].frame_traces[frame]);
        }
        json << "]}";
        for (std::size_t frame = 0u;
             frame < runs[index].frame_ms.size(); ++frame)
        {
            const lux::runtime::FrameTrace empty_trace{};
            const auto& trace = frame < runs[index].frame_traces.size()
                ? runs[index].frame_traces[frame]
                : empty_trace;
            const auto route_frame = frame < runs[index].route_frames.size()
                ? runs[index].route_frames[frame]
                : 0u;
            csv << index << ',' << frame << ',' << route_frame << ','
                << trace.frame_serial << ','
                << runs[index].frame_ms[frame] << ','
                << trace.wall_nanoseconds << ','
                << trace.attributedNanoseconds();
            for (const auto phase : trace.phase_nanoseconds)
                csv << ',' << phase;
            csv << ',' << trace.allocation_count
                << ',' << trace.allocation_bytes << '\n';
        }
        for (const auto& system : runs[index].system_frames)
        {
            systems_csv << index << ',' << system.sample_frame << ','
                << system.route_frame << ',' << system.frame_serial << ','
                << system.system_hash << ',';
            writeCsvText(systems_csv, system.system_name);
            systems_csv << ',' << schedulePhaseName(system.phase) << ','
                << system.wall_nanoseconds << '\n';
        }
    }
    json << "],\"acceptance\":{\"passed\":"
        << (acceptance_failed ? "false" : "true")
        << ",\"failures\":[";
    for (std::size_t index = 0u;
         index < acceptance_failures.size(); ++index)
    {
        if (index != 0u)
            json << ',';
        const auto& failure = acceptance_failures[index];
        json << "{\"gate\":\"" << failure.gate
            << "\",\"run\":" << failure.run
            << ",\"required\":" << failure.required
            << ",\"actual\":" << failure.actual
            << ",\"failure_count\":" << failure.failure_count
            << ",\"sufficient_samples\":"
            << (failure.sufficient_samples ? "true" : "false") << '}';
    }
    json << "]}}\n";
    json.flush();
    csv.flush();
    systems_csv.flush();
    if (!json || !csv || !systems_csv)
        return 1;
    std::printf("wrote %s, %s and %s\n",
        options->json.string().c_str(),
        options->csv.string().c_str(),
        systems_csv_path.string().c_str());
    if (acceptance_failed)
    {
        std::fprintf(
            stderr,
            "benchmark acceptance gate failed; inspect the JSON report\n");
        return 3;
    }
    return 0;
}
