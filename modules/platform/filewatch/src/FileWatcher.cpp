#include <lux/engine/filewatch/FileWatcher.hpp>

#include <efsw/efsw.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

namespace lux::platform
{
    // efsw 的回调在**监视线程**上到达 —— 这里只做一件事:转换 + 压队列。
    // 任何更聪明的处理(去抖/匹配资产)都属于主线程消费者。
    struct FileWatcher::Impl final : public efsw::FileWatchListener
    {
        efsw::FileWatcher               watcher;
        std::mutex                      mtx;
        std::vector<FileEvent>          queue;
        std::vector<efsw::WatchID>      watch_ids;
        std::vector<std::filesystem::path> watched_dirs;

        void handleFileAction(efsw::WatchID /*id*/, const std::string& dir,
                              const std::string& filename, efsw::Action action,
                              std::string old_filename) override
        {
            FileEvent ev;
            ev.path = std::filesystem::path(dir) / filename;
            switch (action)
            {
            case efsw::Actions::Add:      ev.kind = EFileEvent::FILE_ADDED;    break;
            case efsw::Actions::Delete:   ev.kind = EFileEvent::FILE_REMOVED;  break;
            case efsw::Actions::Modified: ev.kind = EFileEvent::FILE_MODIFIED; break;
            case efsw::Actions::Moved:
                ev.kind     = EFileEvent::FILE_RENAMED;
                ev.old_path = std::filesystem::path(dir) / old_filename;
                break;
            default:                      ev.kind = EFileEvent::FILE_MODIFIED; break;
            }
            std::lock_guard lock(mtx);
            queue.push_back(std::move(ev));
        }
    };

    FileWatcher::FileWatcher()
        : impl_(std::make_unique<Impl>())
    {
        impl_->watcher.watch();   // 启动监视线程(efsw 自管生命周期)
    }

    FileWatcher::~FileWatcher() = default;   // efsw::FileWatcher 析构 join 线程

    bool FileWatcher::watch(const std::filesystem::path& dir, bool recursive)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return false;

        const auto norm = dir.lexically_normal();
        if (std::find(impl_->watched_dirs.begin(), impl_->watched_dirs.end(), norm)
            != impl_->watched_dirs.end())
            return true;   // 重复 watch 是 no-op

        const auto id = impl_->watcher.addWatch(norm.string(), impl_.get(), recursive);
        if (id < 0) return false;   // efsw 负值 = 错误码
        impl_->watch_ids.push_back(id);
        impl_->watched_dirs.push_back(norm);
        return true;
    }

    void FileWatcher::unwatchAll()
    {
        for (const auto id : impl_->watch_ids)
            impl_->watcher.removeWatch(id);
        impl_->watch_ids.clear();
        impl_->watched_dirs.clear();
    }

    std::vector<FileEvent> FileWatcher::drain()
    {
        std::lock_guard lock(impl_->mtx);
        return std::exchange(impl_->queue, {});
    }

} // namespace lux::platform
