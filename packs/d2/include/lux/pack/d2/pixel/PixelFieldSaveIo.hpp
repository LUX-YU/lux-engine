#pragma once
// ============================================================================
//  PixelFieldSaveIo.hpp — asynchronous file IO for pixel-field save states
//  (C2-02a, lux::pack).
//
//  The blob CODEC lives on PixelFieldRuntime (captureState/restoreState —
//  content knowledge); this class owns only the FILE + THREAD discipline: one
//  worker thread does every read/write (the render thread never touches a
//  file — hard project rule; neither does the gameplay main thread block on
//  disk), results queue up and the CALLER drains them on its own thread via
//  poll() — completions never run concurrently with gameplay code.
//
//  Usage:
//      PixelFieldSaveIo io;
//      io.saveAsync(path, runtime.captureState(h),
//                   [](bool ok, std::string err){ ... });
//      io.loadAsync(path,
//                   [&](bool ok, std::vector<std::byte> blob, std::string err)
//                   { if (ok) h2 = runtime.restoreState(blob); });
//      ... each frame: io.poll();      // fire completions HERE
//
//  Destruction joins the worker after finishing queued work; completions not
//  yet polled are dropped (the owner is going away — nothing to notify).
// ============================================================================

#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lux::pack
{
    class LUX_FUNCTION_PUBLIC PixelFieldSaveIo
    {
    public:
        using SaveCallback = std::function<void(bool ok, std::string error)>;
        using LoadCallback = std::function<void(bool ok, std::vector<std::byte> blob,
                                                std::string error)>;

        PixelFieldSaveIo();
        ~PixelFieldSaveIo();

        PixelFieldSaveIo(const PixelFieldSaveIo&) = delete;
        PixelFieldSaveIo& operator=(const PixelFieldSaveIo&) = delete;

        /// Queue an atomic write (temp file + rename) of @p blob to @p path.
        /// @p on_done fires from a later poll() on the CALLER's thread.
        void saveAsync(std::filesystem::path path, std::vector<std::byte> blob,
                       SaveCallback on_done = {});

        /// Queue a full read of @p path; the blob arrives via poll().
        void loadAsync(std::filesystem::path path, LoadCallback on_done);

        /// Fire every completed operation's callback (caller-thread marshalling).
        /// Returns the number of completions fired.
        std::size_t poll();

        /// Operations queued or in flight (poll() until it reaches zero to drain).
        [[nodiscard]] std::size_t pendingCount() const noexcept;

    private:
        struct Impl;
        Impl* impl_;
    };

} // namespace lux::pack
