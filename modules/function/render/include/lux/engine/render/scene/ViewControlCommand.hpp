#pragma once
/*
 * ViewControlCommand.hpp - Phase 0 Infrastructure
 * 
 * Control commands for view lifecycle management.
 * Transmitted from game thread to render thread via SPSC queue.
 */

#include <lux/engine/render/core/RenderTypes.hpp>

#include <variant>
#include <cstdint>

namespace lux::render
{

//==============================================================================
// View Lifecycle Commands
//==============================================================================

/**
 * @brief Create a new view
 * 
 * Sent from game thread to request render thread to allocate backend resources
 * for a new view (RenderGraph nodes, render targets, etc.)
 */
struct CreateViewCmd
{
    uint32_t    view_handle;          ///< Pre-allocated handle (game thread already called ViewRegistry::allocate())
    common::Size2D    initial_extent;       ///< Initial resolution
    const char* debug_name;           ///< Human-readable name (must outlive command transmission)
};

/**
 * @brief Destroy an existing view
 * 
 * Render thread will release all backend resources (render targets, graph nodes)
 * and acknowledge completion. Game thread can then call ViewRegistry::deallocate().
 */
struct DestroyViewCmd
{
    uint32_t view_handle;       ///< View to destroy
};

/**
 * @brief Resize view's render targets
 * 
 * Triggered by window resize or dynamic resolution scaling.
 * Render thread will recreate render targets with new extent.
 */
struct ResizeViewCmd
{
    uint32_t view_handle;       ///< View to resize
    common::Size2D new_extent;          ///< New resolution
};

/**
 * @brief Pause view rendering
 * 
 * View will stop receiving ViewFramePackets and will not be scheduled.
 * Backend resources remain allocated.
 */
struct PauseViewCmd
{
    uint32_t view_handle;       ///< View to pause
};

/**
 * @brief Resume view rendering
 * 
 * View will start receiving and processing ViewFramePackets again.
 */
struct ResumeViewCmd
{
    uint32_t view_handle;       ///< View to resume
};

/**
 * @brief Update view quality preset
 * 
 * Changes LOD bias, shadow quality, etc. without recreating resources.
 */
struct UpdateViewQualityCmd
{
    uint32_t view_handle;       ///< View to update
    uint8_t quality_preset;       ///< New quality level (0=default, 1-4=low to ultra)
};

/**
 * @brief Enable or disable a specific feature for one view
 *
 * Identified by feature_id (RenderFeature::featureId()).
 * Only affects the per-view feature set; the global feature instance is unchanged.
 */
struct SetViewFeatureEnabledCmd
{
    uint32_t       view_handle;    ///< View to modify
    uint32_t         feature_id;     ///< Feature ID assigned during registration
    bool             enabled;        ///< New per-view enabled state
};

//==============================================================================
// Command Variant
//==============================================================================

/**
 * @brief Type-safe union of all view control commands
 * 
 * Transmitted via SPSC queue (lux::cxx::SpscLockFreeRingQueue<ViewControlCommand>)
 * from game thread to render thread.
 * 
 * Usage:
 * @code
 * // Game thread sends command
 * ViewControlCommand cmd = CreateViewCmd{handle, extent, "Main View"};
 * control_queue.emplace(std::move(cmd));
 * 
 * // Render thread processes command
 * if (ViewControlCommand cmd; control_queue.try_dequeue(cmd)) {
 *     std::visit([](auto&& command) {
 *         using T = std::decay_t<decltype(command)>;
 *         if constexpr (std::is_same_v<T, CreateViewCmd>) {
 *             // Handle create...
 *         }
 *         // ... other command types
 *     }, cmd);
 * }
 * @endcode
 */
using ViewControlCommand = std::variant<
    CreateViewCmd,
    DestroyViewCmd,
    ResizeViewCmd,
    PauseViewCmd,
    ResumeViewCmd,
    UpdateViewQualityCmd,
    SetViewFeatureEnabledCmd
>;

//==============================================================================
// Command Type Query
//==============================================================================

/**
 * @brief Get human-readable command type name (for debugging)
 */
inline const char* getCommandTypeName(const ViewControlCommand& cmd)
{
    return std::visit([](auto&& arg) -> const char* {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, CreateViewCmd>)
            return "CreateView";
        else if constexpr (std::is_same_v<T, DestroyViewCmd>)
            return "DestroyView";
        else if constexpr (std::is_same_v<T, ResizeViewCmd>)
            return "ResizeView";
        else if constexpr (std::is_same_v<T, PauseViewCmd>)
            return "PauseView";
        else if constexpr (std::is_same_v<T, ResumeViewCmd>)
            return "ResumeView";
        else if constexpr (std::is_same_v<T, UpdateViewQualityCmd>)
            return "UpdateViewQuality";
        else if constexpr (std::is_same_v<T, SetViewFeatureEnabledCmd>)
            return "SetViewFeatureEnabled";
        else
            return "Unknown";
    }, cmd);
}

/**
 * @brief Extract uint32_t from any command
 * 
 * All commands operate on a specific view, so they all have a view_handle field.
 */
inline uint32_t getCommanduint32_t(const ViewControlCommand& cmd)
{
    return std::visit([](auto&& arg) {
        return arg.view_handle;
    }, cmd);
}

} // namespace lux::render
