#pragma once
/**
 * @file Selection.hpp
 * @brief Stable editor object selection plus an optional materialized ECS proxy.
 *
 * Scene-domain state: EditorScene 以值成员持有,面板经指针借用、每帧读
 * (immediate-mode, via registry()/entity()); every mutation goes through the
 * single writer select() (hierarchy click / viewport pick / scene swap).
 *
 * This is the ECS-coupled piece (`entt`), which is exactly why it lives in the
 * editor layer and NOT in the generic `ui` framework. registry() must be
 * non-null while a panel reads it; treat entity() as advisory and guard with
 * `registry()->valid(entity())` (a scene swap can leave a stale id for one
 * frame).
 *
 * The "current value" lives here (immediate-mode panels pull it every frame).
 * 边沿通知(选中变化的副作用钩子)当前不存在 —— 全仓消费者都是 pull 模型;
 * 真需要时在 select() 的落值点发布总线事件(见那里的注释),不再回到 Signal。
 */

#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/authoring/world/WorldIdentifiers.hpp>
#include <lux/engine/authoring/world/WorldPartition.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <optional>
#include <variant>

namespace lux::editor
{
    struct TransientEntityId final
    {
        entt::entity value{entt::null};

        friend bool operator==(
            const TransientEntityId&,
            const TransientEntityId&) = default;
    };

    struct TerrainSelection final
    {
        uuids::uuid terrain{};
        lux::authoring::WorldCellKey page;
        std::uint32_t element{0u};

        friend bool operator==(
            const TerrainSelection&,
            const TerrainSelection&) = default;
    };

    struct TileSelection final
    {
        uuids::uuid tilemap{};
        lux::authoring::WorldCellKey chunk;
        std::uint32_t element{0u};

        friend bool operator==(
            const TileSelection&,
            const TileSelection&) = default;
    };

    struct PixelSelection final
    {
        uuids::uuid field{};
        lux::authoring::WorldCellKey chunk;
        std::uint32_t element{0u};

        friend bool operator==(
            const PixelSelection&,
            const PixelSelection&) = default;
    };

    using EditorObjectId = std::variant<
        TransientEntityId,
        lux::entity_scene::PersistentEntityId,
        lux::authoring::WorldInstanceId,
        TerrainSelection,
        TileSelection,
        PixelSelection>;

    class Selection
    {
    public:
        [[nodiscard]] lux::meta::EntityRegistryBase* registry() const noexcept
        {
            return registry_;
        }
        [[nodiscard]] entt::entity    entity()   const noexcept { return entity_; }
        [[nodiscard]] const std::optional<EditorObjectId>& object() const noexcept
        {
            return object_;
        }

        /// Single mutation entry point: atomically set world + entity.
        /// 自去重(真变化才落值)—— 这里是将来「选中变化」边沿通知的唯一
        /// 挂点:哪天出现真实消费者,在此发布总线事件(SelectionChanged),
        /// 编辑器层可直接 publish。(曾有 Signal changed —— 全仓零订阅者,
        /// 随信号层退役批删除;消费者全是 pull 模型,每帧读值。)
        void select(
            lux::meta::EntityRegistryBase* reg,
            entt::entity e)
        {
            std::optional<EditorObjectId> object;
            if (reg && e != entt::null && reg->valid(e))
            {
                if (const auto* stable = reg->try_get<
                        lux::ecs::PersistentEntityIdComponent>(e);
                    stable && !stable->id().empty())
                {
                    object = lux::entity_scene::PersistentEntityId{
                        stable->id().value()};
                }
                else
                {
                    object = TransientEntityId{e};
                }
            }
            if (reg == registry_ && e == entity_ && object == object_)
                return;
            registry_ = reg;
            entity_   = e;
            object_   = std::move(object);
        }

        /// Select a stable object before its ECS proxy exists. Panels which
        /// require components see entity()==null until resolveProxy() adopts
        /// the asynchronously materialized proxy.
        void selectObject(EditorObjectId object)
        {
            if (object_ == object && entity_ == entt::null)
                return;
            object_ = std::move(object);
            entity_ = entt::null;
        }

        void resolveProxy(
            lux::meta::EntityRegistryBase* reg,
            entt::entity entity,
            const EditorObjectId& object)
        {
            if (!object_ || *object_ != object)
                return;
            registry_ = reg;
            entity_ = entity;
        }

        void releaseProxy(entt::entity entity)
        {
            if (entity_ == entity)
                entity_ = entt::null;
        }

        /// Keep the current world, change the selected entity (hierarchy click,
        /// viewport pick, spawn). entt::null clears the selection.
        void selectEntity(entt::entity e) { select(registry_, e); }

        /// Clear the entity, keep the current world.
        void clear()
        {
            entity_ = entt::null;
            object_.reset();
        }

    private:
        lux::meta::EntityRegistryBase* registry_{nullptr};
        entt::entity    entity_{entt::null};
        std::optional<EditorObjectId> object_;
    };

} // namespace lux::editor
