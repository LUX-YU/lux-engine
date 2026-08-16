#pragma once
/**
 * @file AssetDeleteController.hpp — 资产删除流程(私有,engine/editor/src/app)。
 *
 * 策略(用户裁决):**列出引用者 + 允许强删**(Unreal 的 Delete 对话框形状;
 * 强删后场景里的实体经裁决七的失效广播换装 M_Missing 醒目材质,不消失)。
 *
 * 相位分工(与 ImportController/MaterialPreviewHost 同款纪律):
 *   · request()      — AssetBrowser 的 Delete… 信号(面板 paint 期):只做
 *                      **只读**的引用者扫描 + 记下请求,打开对话框;
 *   · paintDialog()  — 每帧(EditorMenuBar 驱动,ImGui 帧内):画确认对话框,
 *                      「Delete」只置 confirmed_,**不做任何变更**;
 *   · tick()         — 主循环 frame-OPEN 段:执行真正的删除(removeAsset →
 *                      失效广播、删盘上文件、缩略图作废(要发 destroyTexture,
 *                      必须帧开着)、content_changed 驱动 registry/browser 重扫)。
 *
 * 引用者扫描的两层账(互补,缺一个都会漏):
 *   · **场景内组件字段**(可列举):ComponentTypeCatalog × RefClass.fields ×
 *     serialize::isAssetRefField —— 「什么算资产引用」与序列化同一张表;
 *   · **驻留票**(不可列举,只能否决):AssetManager::isReferenced —— 材质级联
 *     钉的贴图、动画/脚本的票都在这本账上,但账本是数字不是名单。
 */

#include <lux/engine/editor/panels/AssetBrowser.hpp>   // DeleteAssetCommand
#include <lux/engine/meta/LuxObject.hpp>               // EntityRegistry

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lux::asset  { class AssetManager; }
namespace lux::ecs { class ComponentTypeCatalog; }
namespace lux::events { class DomainEvents; }

namespace lux::editor
{
    class ThumbnailService;

    class AssetDeleteController
    {
    public:
        struct Services
        {
            std::shared_ptr<lux::asset::AssetManager> assets;
            ThumbnailService*      thumbnails{nullptr};   ///< 可空(缩略图作废)
            lux::events::DomainEvents* events{nullptr};       ///< committed facts
            const lux::ecs::ComponentTypeCatalog& components;
            /// 当前场景的注册表(无场景 = null)。每次扫描现取 —— 场景会换。
            std::function<lux::meta::EntityRegistry*()> scene_registry;
        };

        explicit AssetDeleteController(Services services)
            : svc_(std::move(services)) {}

        /// AssetBrowser 的 Delete… 请求:只读扫描 + 打开对话框(paint 期安全)。
        void request(const DeleteAssetCommand& command);

        /// 每帧画确认对话框(ImGui 帧内;EditorMenuBar 驱动)。
        void paintDialog();

        /// 主循环 frame-OPEN 段:执行已确认的删除。
        void tick();

    private:
        struct Referencer
        {
            std::string entity;      ///< NameComponent 或 #id
            std::string component;   ///< 组件全名(短化显示)
            std::string field;
        };

        void scanReferencers();
        void executeDelete();

        Services             svc_;
        DeleteAssetCommand command_{};
        std::vector<Referencer> refs_;
        bool has_live_tickets_{false};   ///< isReferenced:场景扫描之外的驻留票
        bool open_{false};               ///< 对话框在场
        bool confirmed_{false};          ///< paint 期置位,tick 执行
    };

} // namespace lux::editor
