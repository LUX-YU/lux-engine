#include "app/AssetFileWatcher.hpp"

#include <lux/engine/editor/AssetRegistry.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/log/Log.hpp>

#include <algorithm>
#include <cwctype>
#include <string>
#include <system_error>

namespace lux::editor
{
    namespace
    {
        /// Windows 路径大小写不敏感,而 OS 事件带的是文件系统的实际大小写,
        /// registry 里存的是扫描时的 —— 比较前统一小写化的 generic 形式。
        std::wstring canonKey(const std::filesystem::path& p)
        {
            std::wstring s = p.lexically_normal().generic_wstring();
            std::transform(s.begin(), s.end(), s.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return s;
        }
    } // namespace

    void AssetFileWatcher::observe(const lux::platform::FileEvent& ev)
    {
        if (!svc_.assets || !svc_.registry) return;
        const auto& root = svc_.registry->root();
        if (root.empty()) return;

        // 删除走删除流程(AssetDeleteController),新增走 content_changed
        // 重扫 —— 本表只管「已注册资产的内容变了」。RENAMED 的新路径若
        // 恰好匹配某资产,按修改对待(外部「写临时文件再改名」的保存法)。
        if (ev.kind == lux::platform::EFileEvent::FILE_REMOVED) return;

        // 匹配已注册资产:事件路径 vs root/rel_path(大小写不敏感)。
        // 事件量小(编辑动作级),线性扫可行;registry 重扫会换表,不缓存。
        const auto key = canonKey(ev.path);
        for (const auto& m : svc_.registry->all())
        {
            if (m.id.is_nil()) continue;
            if (canonKey(root / m.rel_path) != key) continue;

            auto& p = pending_[m.id];
            p.abs_path = ev.path;
            ++p.generation;
            p.retries = 0;
            p.needs_dispatch = true;
            (void)tryDispatch(m.id, p);
            break;
        }
    }

    void AssetFileWatcher::tick()
    {
        if (!svc_.assets || !svc_.registry) return;
        processPending();
    }

    void AssetFileWatcher::processPending()
    {
        for (auto& [id, pending] : pending_)
            (void)tryDispatch(id, pending);
    }

    bool AssetFileWatcher::tryDispatch(
        const lux::asset::asset_id_t& id,
        PendingChange& pending)
    {
        if (pending.in_flight || !pending.needs_dispatch)
            return true;
        if (!reload_dispatch_)
        {
            lux::log::error(
                "editor",
                "asset reload operation is not installed: '{}'",
                pending.abs_path.string());
            pending.needs_dispatch = false;
            return false;
        }
        if (!reload_dispatch_(id, pending.abs_path, pending.generation))
            return false;
        pending.active_generation = pending.generation;
        pending.in_flight = true;
        pending.needs_dispatch = false;
        return true;
    }

    void AssetFileWatcher::adoptReloadResult(const lux::asset::asset_id_t&        id,
                                             std::uint64_t                         generation,
                                             std::unique_ptr<lux::asset::LuxAsset> asset,
                                             std::string_view                      error)
    {
        auto it = pending_.find(id);
        if (it == pending_.end()) return;   // 条目已被清走(删除流程/放弃):结果作废
        auto& p = it->second;
        if (generation != p.active_generation)
            return;
        p.in_flight = false;

        // A newer OS event arrived while this generation was in flight. Start
        // only its timer now: repeated writes coalesce without concurrent reads,
        // and a stale decode can never mutate the asset ledger.
        if (generation != p.generation)
        {
            p.needs_dispatch = true;
            (void)tryDispatch(id, p);
            return;
        }

        if (!asset)
        {
            // 池上读盘/解码失败(写一半/损坏/id 失配):旧对象原样在场,
            // 重试要重新等稳。
            if (++p.retries > kMaxRetries)
            {
                lux::log::warn("editor",
                    "asset hot-reload gave up after {} tries: '{}' "
                    "({}; old GPU copy stays)",
                    kMaxRetries, p.abs_path.string(), error);
                pending_.erase(it);
                return;
            }
            p.needs_dispatch = true;
            (void)tryDispatch(id, p);
            return;
        }

        if (!svc_.assets || !svc_.assets->replaceAsset(std::move(asset)))
        {
            // 账上已没这个 id(飞行期删除流程赢了):结果作废,静默归位。
            pending_.erase(it);
            return;
        }

        // replaceAsset 已 ++revision + content_changed 广播 → 批5 链条接手。
        if (svc_.thumbnails) svc_.thumbnails->invalidate(id);
        lux::log::info("editor", "asset hot-reloaded from disk: '{}'",
                       p.abs_path.filename().string());

        pending_.erase(it);
    }

} // namespace lux::editor
