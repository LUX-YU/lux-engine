#pragma once
/**
 * @file EditorEvents.hpp
 * @brief 编辑器域的应用级事件类型(统一事件系统条例②:事件 struct 是领域
 *        词汇,定义在字段类型可见的最低层)。
 *
 * 这里只放**真正多对多**的编辑器事实。发布走进程域 lux::events::DomainEvents
 * (面板属编辑器应用层,可直接 publish;订阅集中在装配层 —— LuxEditor 的
 * subs_ 与 EditorShell 的 panel_subs_)。
 *
 * (曾有 `EditorEvents` 结构体:一个 Signal 集线器,LuxEditor 持 shared_ptr
 *  经 events() 分发给生产者。事件批D 起 Signal 载体退役 —— 事件类型即通道,
 *  集线器与访问器都不再需要。)
 *
 * Scope note (deliberate): scene / project lifecycle is intentionally NOT
 * eventified here:
 *   - `ProjectController::openProject` is an ordered PIPELINE (register assets
 *     -> index -> mount vfs -> point browser -> load scene) whose steps have
 *     strict data dependencies; scattering it across subscribers would trade a
 *     clear sequential algorithm for fragile emit-order coupling.
 *   - Scene load/unload side-effects (viewport texture, selection rebind, the
 *     pick/resize forwarding) are already handled directly where they belong
 *     (SceneController), so a broadcast would add indirection for no gain.
 * Add an event here only when a real, ORDER-INDEPENDENT fan-out appears.
 */

#include <lux/engine/resource/identity/AssetId.hpp>

#include <cstdint>
#include <filesystem>

namespace lux::editor
{
    /// What was committed to the editor-visible on-disk asset catalogue.
    enum class EEditorAssetChange : std::uint8_t
    {
        ADDED,
        CONTENT_UPDATED,
        REMOVED
    };

    /// Precise, already-committed editor asset fact.  This is deliberately not
    /// a generic "content changed" invalidation: consumers can ignore content
    /// updates when their index depends only on id/type/path, and can use the
    /// path to decide whether a visible directory is affected.
    struct EditorAssetChanged
    {
        lux::asset::asset_id_t id;
        EEditorAssetChange     change{EEditorAssetChange::CONTENT_UPDATED};
        std::uint32_t          revision{0};
        std::filesystem::path  path;
    };

} // namespace lux::editor
