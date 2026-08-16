#pragma once
/**
 * @file AssetFileWatcher.hpp — 资产文件监视(私有,engine/editor/src/app)。
 *
 * 热更新批6(用户裁决):**文件变化是内容变更的唯一根因** —— 编辑器 Save、
 * 外部程序覆写、将来的重导入,全部经盘上文件收敛到同一条触发链,底层不为
 * 新资产类型换触发代码:
 *
 *   OS 文件事件(platform::FileWatcher，由 LuxEditor 在主线程 drain 后
 *   直接交给本单消费者；原始文件通知不是领域事实，不经过 DomainEvents)
 *     → observe:匹配到已注册资产(AssetRegistry 的 path→id 视图)
 *     → typed operation 内的 Asio timer 合并一个短窗
 *     → typed ReloadAsset operation
 *     → CPU pool 上 VFS 读盘 + parseLuxAssetMemory（纯解析，不碰账本）
 *     → MainThreadScheduler 回主线程 adoptReloadResult
 *     → replaceAsset(同 id 原子换对象,++revision +
 *       content_changed 广播 —— 旧数据挂到新对象就位,**无壳窗口**)
 *     → 驻留三件套级联重建 / 实例句柄比对
 *       (热更新批5 的链条)接手 —— 场景数帧内呈现新内容。
 *
 * (批6 首版是 mtime 全量轮询;批7 换 OS 事件驱动;事件批B 把 OS 原语的
 *  drain 与「翻译成事件」上提到装配层;批H4 把解码下池 —— 大资产热重载
 *  不再卡主线程帧。装配层没接缝是响亮配置错误，不提供同步旁路。)
 *
 * 失败安全:解码失败(文件写一半/损坏)→ 旧对象原样在场(异步路连壳窗口
 * 都没有),GPU 旧副本继续画;重试超限后一次性诊断并放弃。飞行期文件又变
 * → 装回本次结果后重新走稳定窗(最终一致)。
 *
 * tick() 只在 typed queue 暂时饱和时重试 admission。500ms debounce 由
 * coordinator 的 Asio timer 推进，不随画面帧率伸缩；reload/upload 不要求
 * frame OPEN，最终账本采纳仍由 MainThreadScheduler 串行化。
 */

#include <lux/engine/resource/asset/Asset.hpp>            // asset_id_t / LuxAsset
#include <lux/engine/filewatch/FileWatcher.hpp>  // EFileEvent(事件字段类型)
#include <lux/cxx/core/move_only_function.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lux::asset
{
    class AssetManager;
    class AssetVfs;
}

namespace lux::editor
{
    class AssetRegistry;
    class ThumbnailService;

    // ── 热重载的异步三段式(批H4;设计 §7.9-⑤)────────────────────────
    //
    // 稳定窗过后 watcher 经派发缝提交 typed operation；池上 VFS 读盘与
    // 纯解析；MainThreadScheduler 将拥有型结果送回主线程采纳。中间结果不是事件。

    class AssetFileWatcher
    {
    public:
        struct Services
        {
            std::shared_ptr<lux::asset::AssetManager> assets;
            AssetRegistry*    registry{nullptr};     ///< path→id 表(借用,宿主持有)
            ThumbnailService* thumbnails{nullptr};   ///< 可空:变更后作废缩略图
        };

        explicit AssetFileWatcher(Services services)
            : svc_(std::move(services)) {}

        /// Sole-consumer ingress for drained OS notifications. Matching assets
        /// enter the debounce table; unrelated files are ignored.
        void observe(const lux::platform::FileEvent& event);

        /// 主线程安全点调用：只重试因 typed queue 背压而未受理的意图。
        /// 正常 debounce 与 IO 不依赖此 tick。
        void tick();

        /// reload 单消费者派发缝：装配层提交 typed ReloadAsset operation。
        /// 缝空是装配错误。
        using ReloadDispatch = lux::cxx::move_only_function<bool(
            const lux::asset::asset_id_t&,
            const std::filesystem::path&,
            std::uint64_t)>;
        void setReloadDispatch(ReloadDispatch dispatch)
        { reload_dispatch_ = std::move(dispatch); }

        /// MainThreadScheduler completion target（主线程）。
        /// 成功 → replaceAsset(++revision + 广播)+ 缩略图作废;失败 →
        /// 重试计账(重新等稳)或超限放弃。条目已被清走(删除流程)则结果
        /// 作废丢弃。
        void adoptReloadResult(const lux::asset::asset_id_t&        id,
                               std::uint64_t                         generation,
                               std::unique_ptr<lux::asset::LuxAsset> asset,
                               std::string_view                      error);

    private:
        struct PendingChange
        {
            std::filesystem::path            abs_path;
            std::uint64_t                    generation{0u};
            std::uint64_t                    active_generation{0u};
            int                              retries{0};
            bool                             in_flight{false};
            bool                             needs_dispatch{false};
        };

        void processPending();
        bool tryDispatch(
            const lux::asset::asset_id_t& id,
            PendingChange& pending);

        Services                     svc_;
        ReloadDispatch               reload_dispatch_;
        std::unordered_map<lux::asset::asset_id_t, PendingChange> pending_;

        static constexpr int kMaxRetries   = 10;
    };

} // namespace lux::editor
