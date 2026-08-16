#pragma once
/**
 * @file RenderResourceStateManager.hpp — 渲染资源状态表(驻留 T4R,
 *       J6-六修)。
 *
 * **纯关系表**:(asset_id, resident owner, state),附 revision /
 * sealed / 失败原因 / 依赖边 等列。引擎资产注册期建表(可渲染资产全量
 * 入行:句柄无效、状态未读取),此后由**引擎编排**在调用链里更新 ——
 * 本类只提供 建行/查询/更新(经返回的行引用)/擦除/遍历,**不持任何
 * 回调、不调任何人、不认识组件/AssetManager**(asset_id 只是不透明键)。
 *
 * 去重就是表本身:「读取中/上传中」的行不会被编排起第二条链。
 * 进程域、每渲染通道一份(资产驻留裁决二),切场景不重建;主线程独占。
 *
 * 谁改状态、何时改 —— 全在 RenderResourceOrchestrator(引擎编排);
 * 等待名单在 RenderResourceService(RAII 订阅注册表)。三者分工见
 * 设计稿 J6-六修。
 */

#include <lux/engine/runtime/render/scene/visibility.h>

#include <lux/engine/resource/asset/Asset.hpp>                       // asset_id_t(键)
#include <lux/engine/resource/asset/AssetRef.hpp>                    // 依赖票列(RAII 数据)
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>   // 域/失败词汇
#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lux::runtime
{
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC RenderResourceStateManager
    {
    public:
        enum class EState : std::uint8_t
        {
            UNLOADED,    ///< 未读取(建表初值)
            LOADING,     ///< CPU 数据读取中(编排已起链)
            UPLOADING,   ///< 上传 RPC 在途
            READY,       ///< resident 有效
            FAILED,      ///< 终态(原因见 last_fail;revision 变更/解封重走)
        };

        struct Row
        {
            lux::ecs::EResourceDomain domain{lux::ecs::EResourceDomain::TEXTURE};
            EState                    state{EState::UNLOADED};
            std::uint32_t             revision{0};   ///< 建行盖章;失配=内容换代
            /// 每次 UNLOADED→LOADING 分配的新代次。异步链的每一段都
            /// 捕获并核对它，旧回执不能落进同 id 的新行（ABA）。
            std::uint64_t             operation_serial{0};
            bool                      sealed{false}; ///< 失效封印(裁决七;正交于 state)
            lux::ecs::ResourceFailure last_fail;     ///< FAILED 的原因(查询复述)
            /// 依赖边(关系数据,级联行走的键)+ 代持驻留票(行亡票随
            /// 析构归还 → 依赖 1→0 自然触发)。谁写:编排。
            std::vector<lux::asset::asset_id_t> depends_on;
            std::vector<lux::asset::AssetRef>   dep_refs;
            /// Must remain the last owning member: row destruction releases
            /// the GPU object before dependency tickets can cascade to their
            /// own rows. Copyable handles exposed elsewhere are observations.
            ResidentResourceLease resident;
        };

        RenderResourceStateManager() noexcept = default;
        RenderResourceStateManager(const RenderResourceStateManager&)            = delete;
        RenderResourceStateManager& operator=(const RenderResourceStateManager&) = delete;

        /// 建行(不存在时;存在原样返回)。域/盖章由调用方(编排)填。
        Row& upsert(const lux::asset::asset_id_t& id) { return rows_[id]; }

        [[nodiscard]] Row* find(const lux::asset::asset_id_t& id)
        {
            auto it = rows_.find(id);
            return it != rows_.end() ? &it->second : nullptr;
        }
        [[nodiscard]] const Row* find(const lux::asset::asset_id_t& id) const
        {
            auto it = rows_.find(id);
            return it != rows_.end() ? &it->second : nullptr;
        }

        /// 擦行(ResidentResourceLease + dep_refs 随之按序析构归还)。
        /// 返回被擦的行(移动出),编排只需收尾等待；不存在返回空行(state
        /// UNLOADED,bits 0)。
        Row takeErase(const lux::asset::asset_id_t& id)
        {
            auto it = rows_.find(id);
            if (it == rows_.end()) return {};
            Row r = std::move(it->second);
            rows_.erase(it);
            return r;
        }

        template <class Fn>   // Fn(const asset_id_t&, Row&)
        void forEach(Fn&& fn)
        {
            for (auto& [id, row] : rows_) fn(id, row);
        }

        [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

        /// Allocate an attempt identity from the table that owns the rows.
        /// Context is intentionally only a wiring aggregate and may be
        /// reconstructed; putting the counter here prevents two Context
        /// objects sharing one table from manufacturing the same ABA token.
        [[nodiscard]] std::uint64_t nextOperationSerial() noexcept
        {
            const std::uint64_t serial = next_operation_serial_++;
            if (next_operation_serial_ == 0)
                next_operation_serial_ = 1; // zero means "not started"
            return serial;
        }

    private:
        std::unordered_map<lux::asset::asset_id_t, Row> rows_;
        std::uint64_t next_operation_serial_{1};
    };

} // namespace lux::runtime
