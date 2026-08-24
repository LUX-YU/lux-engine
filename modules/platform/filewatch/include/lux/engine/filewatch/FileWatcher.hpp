#pragma once
/**
 * @file FileWatcher.hpp
 * @brief 跨平台文件系统监视 —— OS 事件驱动,主线程排空。
 *
 * 后端是 efsw(Win32 ReadDirectoryChangesW / Linux inotify / macOS FSEvents /
 * BSD kqueue,平台后端不可用时自带 generic 轮询兜底)—— 本头把 efsw 类型
 * 完全挡在 .cpp 里,上层只见本接口。
 *
 * 线程模型(这层薄包装存在的全部理由):OS 的文件事件都在**监视线程**上
 * 送达；调用方若有线程亲和状态，必须在自己的边界显式转发。
 * 本类把监视线程的回调压进内部队列,主线程用 drain() 取走(取走即清空)——
 * 跨线程交接只发生在这一个出口,消费者不必知道监视线程存在。
 *
 * 事件语义:原始、不去抖。同一次保存可能连发多条 MODIFIED(写入过程中的
 * 每次 flush 都算),重命名平台差异也大 —— 去抖/稳定判据是**消费者**的事
 * (编辑器的 AssetFileWatcher 用「mtime 稳定一个周期」防半写)。
 */

#include <lux/engine/filewatch/visibility.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace lux::platform
{
    enum class EFileEvent : std::uint8_t
    {
        FILE_ADDED,
        FILE_MODIFIED,
        FILE_REMOVED,
        FILE_RENAMED,
    };

    struct FileEvent
    {
        std::filesystem::path path;       ///< 绝对路径
        std::filesystem::path old_path;   ///< FILE_RENAMED 的旧路径,其余为空
        EFileEvent            kind{ EFileEvent::FILE_MODIFIED };
    };

    class LUX_PLATFORM_FILEWATCH_PUBLIC FileWatcher
    {
    public:
        FileWatcher();
        ~FileWatcher();

        FileWatcher(const FileWatcher&)            = delete;
        FileWatcher& operator=(const FileWatcher&) = delete;

        /// 递归监视一个目录树。可多次调用(多个根);同一目录重复 watch 是
        /// no-op。返回 false = 目录不存在/后端拒绝。
        bool watch(const std::filesystem::path& dir, bool recursive = true);

        /// 撤掉所有监视(重开工程换根时用)。已入队未取走的事件保留。
        void unwatchAll();

        /// 主线程取走监视线程攒下的事件(取走即清空)。
        [[nodiscard]] std::vector<FileEvent> drain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::platform
