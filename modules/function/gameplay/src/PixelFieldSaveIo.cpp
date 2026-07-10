/**
 * @file PixelFieldSaveIo.cpp
 * @brief One-worker async file IO for pixel-field save blobs (C2-02a).
 *        Atomic writes (temp + rename); completions marshalled to poll().
 */

#include <lux/engine/gameplay/2d/pixel/PixelFieldSaveIo.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace lux::gameplay::d2
{
    struct PixelFieldSaveIo::Impl
    {
        struct Task
        {
            bool                   is_save{false};
            std::filesystem::path  path;
            std::vector<std::byte> blob;      // save payload
            SaveCallback           on_save;
            LoadCallback           on_load;
        };
        struct Done
        {
            bool                   ok{false};
            std::string            error;
            std::vector<std::byte> blob;      // load result
            SaveCallback           on_save;
            LoadCallback           on_load;
        };

        std::mutex              mtx;
        std::condition_variable cv;
        std::deque<Task>        tasks;
        std::deque<Done>        done;
        std::atomic<std::size_t> pending{0};
        bool                    quit{false};
        std::thread             worker;

        Impl()
        {
            worker = std::thread([this] { run(); });
        }
        ~Impl()
        {
            {
                std::lock_guard lk(mtx);
                quit = true;
            }
            cv.notify_all();
            if (worker.joinable()) worker.join();
        }

        void run()
        {
            for (;;)
            {
                Task t;
                {
                    std::unique_lock lk(mtx);
                    cv.wait(lk, [this] { return quit || !tasks.empty(); });
                    if (tasks.empty())
                    {
                        if (quit) return;
                        continue;
                    }
                    t = std::move(tasks.front());
                    tasks.pop_front();
                }

                Done d;
                d.on_save = std::move(t.on_save);
                d.on_load = std::move(t.on_load);
                if (t.is_save)
                {
                    // Atomic write: temp sibling + rename (a torn write never
                    // clobbers the previous good save).
                    const auto tmp = t.path.string() + ".tmp";
                    std::error_code ec;
                    {
                        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
                        if (ofs)
                            ofs.write(reinterpret_cast<const char*>(t.blob.data()),
                                      static_cast<std::streamsize>(t.blob.size()));
                        d.ok = static_cast<bool>(ofs);
                    }
                    if (d.ok)
                    {
                        std::filesystem::rename(tmp, t.path, ec);
                        d.ok = !ec;
                        if (ec) d.error = "rename failed: " + ec.message();
                    }
                    else
                        d.error = "write failed: " + tmp;
                    if (!d.ok)
                        std::filesystem::remove(tmp, ec);
                }
                else
                {
                    std::ifstream ifs(t.path, std::ios::binary | std::ios::ate);
                    if (!ifs)
                        d.error = "open failed: " + t.path.string();
                    else
                    {
                        const std::streamoff n = ifs.tellg();
                        ifs.seekg(0, std::ios::beg);
                        d.blob.resize(static_cast<std::size_t>(n));
                        if (n > 0)
                            ifs.read(reinterpret_cast<char*>(d.blob.data()),
                                     static_cast<std::streamsize>(n));
                        d.ok = static_cast<bool>(ifs);
                        if (!d.ok) d.error = "read failed: " + t.path.string();
                    }
                }
                {
                    std::lock_guard lk(mtx);
                    done.push_back(std::move(d));
                }
            }
        }
    };

    PixelFieldSaveIo::PixelFieldSaveIo() : impl_(new Impl) {}
    PixelFieldSaveIo::~PixelFieldSaveIo() { delete impl_; }

    void PixelFieldSaveIo::saveAsync(std::filesystem::path path, std::vector<std::byte> blob,
                                     SaveCallback on_done)
    {
        Impl::Task t;
        t.is_save = true;
        t.path    = std::move(path);
        t.blob    = std::move(blob);
        t.on_save = std::move(on_done);
        {
            std::lock_guard lk(impl_->mtx);
            impl_->tasks.push_back(std::move(t));
        }
        impl_->pending.fetch_add(1, std::memory_order_relaxed);
        impl_->cv.notify_one();
    }

    void PixelFieldSaveIo::loadAsync(std::filesystem::path path, LoadCallback on_done)
    {
        Impl::Task t;
        t.is_save = false;
        t.path    = std::move(path);
        t.on_load = std::move(on_done);
        {
            std::lock_guard lk(impl_->mtx);
            impl_->tasks.push_back(std::move(t));
        }
        impl_->pending.fetch_add(1, std::memory_order_relaxed);
        impl_->cv.notify_one();
    }

    std::size_t PixelFieldSaveIo::poll()
    {
        std::deque<Impl::Done> ready;
        {
            std::lock_guard lk(impl_->mtx);
            ready.swap(impl_->done);
        }
        for (auto& d : ready)
        {
            if (d.on_save) d.on_save(d.ok, std::move(d.error));
            else if (d.on_load) d.on_load(d.ok, std::move(d.blob), std::move(d.error));
            impl_->pending.fetch_sub(1, std::memory_order_relaxed);
        }
        return ready.size();
    }

    std::size_t PixelFieldSaveIo::pendingCount() const noexcept
    {
        return impl_->pending.load(std::memory_order_relaxed);
    }

} // namespace lux::gameplay::d2
