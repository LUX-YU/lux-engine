#include "app/AssetDeleteController.hpp"

#include <lux/engine/editor/app/EditorEvents.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/meta/Meta.hpp>          // RefClass/RefField 真定义(注册表只前置声明)
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/log/Log.hpp>

#include <imgui.h>
#include <uuid.h>

#include <cstring>
#include <system_error>

namespace lux::editor
{
    namespace
    {
        /// 组件全名短化:"lux::ecs::MeshComponent" → "MeshComponent"。
        std::string_view shortName(std::string_view full)
        {
            const auto pos = full.rfind("::");
            return pos == std::string_view::npos ? full : full.substr(pos + 2);
        }
    } // namespace

    void AssetDeleteController::request(const DeleteAssetCommand& command)
    {
        command_   = command;
        confirmed_ = false;
        scanReferencers();
        open_ = true;
    }

    void AssetDeleteController::scanReferencers()
    {
        refs_.clear();
        has_live_tickets_ = svc_.assets
            && svc_.assets->isReferenced(command_.id);

        auto* reg = svc_.scene_registry ? svc_.scene_registry() : nullptr;
        if (!reg) return;

        // 场景内引用者:逐组件类型 × 逐 AssetRef 字段 × 逐实体。判据复用序列化
        // 的分类表(isAssetRefField)—— 「什么算资产引用」只有一处定义。实体数 ×
        // 组件类型数的一次性扫描,只在用户点了 Delete… 时跑,不在每帧。
        const auto entries = svc_.components.all();
        for (const auto e : reg->view<entt::entity>())
        {
            for (const auto& entry : entries)
            {
                if (!entry.ref_class || !entry.operations.has ||
                    !entry.operations.get) continue;
                if (!entry.has(*reg, e)) continue;
                const void* comp = entry.operations.get(*reg, e);
                for (const auto& field : entry.ref_class->fields)
                {
                    if (!lux::serialize::isAssetRefField(field)) continue;
                    uuids::uuid value{};
                    std::memcpy(&value,
                                static_cast<const std::byte*>(comp) + field.offset,
                                sizeof(value));
                    if (value != command_.id) continue;

                    Referencer r;
                    if (const auto* nc = reg->try_get<lux::ecs::NameComponent>(e);
                        nc && !nc->name.empty())
                        r.entity = nc->name;
                    else
                        r.entity = "#" + std::to_string(
                            static_cast<unsigned>(entt::to_integral(e)));
                    r.component = std::string(shortName(entry.fullName()));
                    r.field     = std::string(field.name);
                    refs_.push_back(std::move(r));
                }
            }
        }
    }

    void AssetDeleteController::paintDialog()
    {
        // 仓式 modal 纪律(ImportDialog 同款):open_ 只触发一次 OpenPopup,
        // 之后由 ImGui 自身的弹窗状态维持;Cancel/确认走 CloseCurrentPopup。
        constexpr const char* kId = "Delete Asset##asset_delete";
        if (open_)
        {
            ImGui::OpenPopup(kId);
            open_ = false;
        }
        const auto centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kId, nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::Text("Delete \"%s\"?", command_.name.c_str());
        ImGui::TextDisabled("%s", command_.abs_path.string().c_str());
        ImGui::Separator();

        if (refs_.empty() && !has_live_tickets_)
        {
            ImGui::TextUnformatted("No references in the current scene.");
        }
        else
        {
            if (!refs_.empty())
            {
                ImGui::Text("Referenced by %zu component field(s) in the scene:",
                            refs_.size());
                if (ImGui::BeginTable("##refs", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Entity");
                    ImGui::TableSetupColumn("Component");
                    ImGui::TableSetupColumn("Field");
                    ImGui::TableHeadersRow();
                    for (const auto& r : refs_)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.entity.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.component.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.field.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            if (has_live_tickets_)
                // 账本只能否决不能列举:材质级联钉的贴图、动画/脚本的票都算。
                ImGui::TextUnformatted(
                    "Residency tickets are still held (material cascade / "
                    "animation / script).");
            ImGui::Separator();
            ImGui::TextWrapped(
                "Force-deleting swaps referencing entities onto the magenta "
                "M_Missing material (reversible: re-import restores them).");
        }

        const char* verb = (refs_.empty() && !has_live_tickets_)
                               ? "Delete" : "Force Delete";
        if (ImGui::Button(verb))
        {
            confirmed_ = true;   // 只置位 —— 真正的删除在 tick(frame OPEN)
            open_      = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            open_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void AssetDeleteController::tick()
    {
        if (!confirmed_) return;
        confirmed_ = false;
        executeDelete();
    }

    void AssetDeleteController::executeDelete()
    {
        // ① 对象移除:失效广播(裁决七)→ 缓存停发僵尸 load,resolver 把引用
        //    实体换装 M_Missing;账本不清,票随持有者流干 → GPU 副本归零回收。
        const std::uint32_t revision = svc_.assets
            ? svc_.assets->contentRevision(command_.id)
            : 0;
        if (svc_.assets)
            svc_.assets->removeAsset(command_.id);

        // ② 持久化移除:删盘上文件。LooseAssetProvider 不感知文件消失,靠 ③ 的
        //    content_changed 驱动 rescan。
        std::error_code ec;
        std::filesystem::remove(command_.abs_path, ec);
        if (ec)
            lux::log::warn("editor", "delete: failed to remove '{}': {}",
                           command_.abs_path.string(), ec.message());

        // ②b 缩略图作废(帧开着:Ready 条目携带 GPU 纹理,当场归还)。
        if (svc_.thumbnails)
            svc_.thumbnails->invalidate(command_.id);

        // ③ 发布已经提交的精确删除事实。目录消费者才重建目录视图；驻留
        //    消费者依赖上面的 AssetInvalidated，不从编辑器事实猜状态。
        if (svc_.events)
        {
            svc_.events->publish(EditorAssetChanged{
                command_.id,
                EEditorAssetChange::REMOVED,
                revision,
                command_.abs_path
            });
        }

        lux::log::info("editor", "deleted asset '{}' ({} scene reference(s){})",
                       command_.name, refs_.size(),
                       has_live_tickets_ ? ", live residency tickets" : "");
    }

} // namespace lux::editor
