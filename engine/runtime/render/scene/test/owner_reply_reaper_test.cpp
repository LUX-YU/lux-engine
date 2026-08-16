#include <lux/engine/runtime/render/scene/detail/residency/OwnerReplyReaper.hpp>

#include <lux/engine/function/render/client/RenderRequest.hpp>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    struct ProbeReply
    {
        std::uint64_t owner{0};
    };

    using ProbeFactory = lux::render::RenderRequestFactory<ProbeReply>;

    void deliver(
        ProbeFactory::Callback& callback,
        ProbeReply reply
    )
    {
        ProbeFactory::Packet packet;
        packet.payload.resize(sizeof(reply));
        std::memcpy(packet.payload.data(), &reply, sizeof(reply));

        lux::render::ReplyRecord record;
        record.payload_size = sizeof(reply);
        callback(packet, record);
    }

    struct OwnerProbe
    {
        explicit OwnerProbe(int& destroyed) noexcept
            : destroyed_(&destroyed)
        {}

        ~OwnerProbe() noexcept
        {
            if (destroyed_ != nullptr)
                ++*destroyed_;
        }

        OwnerProbe(const OwnerProbe&) = delete;
        OwnerProbe& operator=(const OwnerProbe&) = delete;

        OwnerProbe(OwnerProbe&& other) noexcept
            : destroyed_(std::exchange(other.destroyed_, nullptr))
        {}

        OwnerProbe& operator=(OwnerProbe&& other) noexcept
        {
            if (this == &other)
                return *this;
            if (destroyed_ != nullptr)
                ++*destroyed_;
            destroyed_ = std::exchange(other.destroyed_, nullptr);
            return *this;
        }

    private:
        int* destroyed_{nullptr};
    };

    int failures{0};

    void check(bool condition, const char* label)
    {
        std::printf("  [%s] %s\n", condition ? "OK" : "FAIL", label);
        if (!condition)
            ++failures;
    }
}

int main()
{
    using Factory = ProbeFactory;
    using Task = lux::cxx::move_only_function<void()>;
    using Reaper = lux::runtime::detail::OwnerReplyReaper<ProbeReply>;
    struct PendingAtDestroyProbe
    {
        Reaper*       reaper{nullptr};
        std::size_t*  observed{nullptr};

        ~PendingAtDestroyProbe() noexcept
        {
            if (reaper != nullptr)
                *observed = reaper->pending();
        }

        PendingAtDestroyProbe(Reaper& value, std::size_t& result) noexcept
            : reaper(&value)
            , observed(&result)
        {}

        PendingAtDestroyProbe(const PendingAtDestroyProbe&) = delete;
        PendingAtDestroyProbe& operator=(
            const PendingAtDestroyProbe&) = delete;

        PendingAtDestroyProbe(PendingAtDestroyProbe&& other) noexcept
            : reaper(std::exchange(other.reaper, nullptr))
            , observed(std::exchange(other.observed, nullptr))
        {}
    };

    std::puts("OwnerReplyReaper lifecycle checks");

    // A reply still on the wire is detached by abandon. Its domain handler is
    // destroyed synchronously, which is where already-known owners are reaped.
    {
        std::vector<Task> main_tasks;
        int owner_destroys = 0;
        int handler_calls  = 0;
        Reaper reaper(
            [&main_tasks](Task task)
            {
                main_tasks.push_back(std::move(task));
                return true;
            }
        );
        auto pending = Factory::make();
        reaper.track(
            std::move(pending.request),
            [owner = OwnerProbe{owner_destroys}, &handler_calls]
            (const ProbeReply&, bool) mutable noexcept
            {
                ++handler_calls;
            }
        );
        check(reaper.pending() == 1 && owner_destroys == 0,
              "wire request and known owner are both retained");
        reaper.abandon();
        check(reaper.pending() == 0 && owner_destroys == 1
                  && handler_calls == 0 && main_tasks.empty(),
              "abandon detaches wire reply and destroys known owner once");
        deliver(pending.callback, {.owner = 0});
        check(reaper.pending() == 0 && owner_destroys == 1
                  && handler_calls == 0 && main_tasks.empty(),
              "late wire settlement stays detached after abandon");
    }

    // A pre-completed request calls RenderRequest::then synchronously.  The
    // trampoline must snapshot State::value before its wire hook erases the
    // sole ScopedRenderRequest owner.
    {
        std::vector<Task> main_tasks;
        bool observed = false;
        Reaper reaper(
            [&main_tasks](Task task)
            {
                main_tasks.push_back(std::move(task));
                return true;
            }
        );
        reaper.track(
            Factory::makeImmediate({.owner = 99}),
            [&observed](const ProbeReply& reply, bool compensate) noexcept
            { observed = !compensate && reply.owner == 99; }
        );
        check(reaper.pending() == 1 && main_tasks.size() == 1,
              "immediate reply is snapshotted before its State is released");
        auto task = std::move(main_tasks.back());
        main_tasks.pop_back();
        task();
        check(observed && reaper.pending() == 0,
              "immediate reply snapshot reaches main intact");
    }

    // Once a wire reply has posted onto main, abandon must not make pending
    // zero until compensation ran.
    {
        std::vector<Task> main_tasks;
        int owner_destroys = 0;
        int handler_calls  = 0;
        bool compensation_only = false;
        Reaper reaper(
            [&main_tasks](Task task)
            {
                main_tasks.push_back(std::move(task));
                return true;
            }
        );
        auto pending = Factory::make();
        reaper.track(
            std::move(pending.request),
            [owner = OwnerProbe{owner_destroys},
             &handler_calls,
             &compensation_only]
            (const ProbeReply& reply, bool compensate) mutable noexcept
            {
                ++handler_calls;
                compensation_only = compensate && reply.owner == 42;
            }
        );
        check(reaper.pending() == 1 && main_tasks.empty(),
              "wire reply is pending before renderer settlement");
        deliver(pending.callback, {.owner = 42});
        check(reaper.pending() == 1 && main_tasks.size() == 1,
              "wire settlement transfers pending ownership to the main queue");
        reaper.abandon();
        check(reaper.pending() == 1,
              "abandon preserves an already-posted owner reply");
        auto task = std::move(main_tasks.back());
        main_tasks.pop_back();
        task();
        check(reaper.pending() == 0 && handler_calls == 1
                  && compensation_only && owner_destroys == 1,
              "posted reply compensates and destroys capture before pending zero");
    }

    // Normal delivery uses the same path without entering compensation mode.
    {
        std::vector<Task> main_tasks;
        bool normal = false;
        Reaper reaper(
            [&main_tasks](Task task)
            {
                main_tasks.push_back(std::move(task));
                return true;
            }
        );
        auto pending = Factory::make();
        reaper.track(
            std::move(pending.request),
            [&normal](const ProbeReply& reply, bool compensate) noexcept
            { normal = !compensate && reply.owner == 7; }
        );
        deliver(pending.callback, {.owner = 7});
        auto task = std::move(main_tasks.back());
        main_tasks.pop_back();
        task();
        check(normal && reaper.pending() == 0,
              "normal posted reply settles exactly once");
    }

    // A valid poster may execute inline. Returning true still means the task
    // ran exactly once; the reaper must not mistake synchronous completion for
    // rejection and invoke compensation a second time.
    {
        int handler_calls = 0;
        bool normal = false;
        Reaper reaper(
            [](Task task) noexcept
            {
                task();
                return true;
            }
        );
        reaper.track(
            Factory::makeImmediate({.owner = 17}),
            [&handler_calls, &normal]
            (const ProbeReply& reply, bool compensate) noexcept
            {
                ++handler_calls;
                normal = !compensate && reply.owner == 17;
            }
        );
        check(handler_calls == 1 && normal && reaper.pending() == 0,
              "synchronous accepted poster settles once without compensation");
    }

    // The handler capture must be destroyed while posted accounting is still
    // non-zero. This is the close proof used by RAII transactions: known GPU
    // owners are released before pending() can report quiescence.
    {
        std::vector<Task> main_tasks;
        std::size_t pending_at_capture_destroy = 0;
        Reaper reaper(
            [&main_tasks](Task task)
            {
                main_tasks.push_back(std::move(task));
                return true;
            }
        );
        auto pending = Factory::make();
        reaper.track(
            std::move(pending.request),
            [probe = PendingAtDestroyProbe{
                 reaper,
                 pending_at_capture_destroy
             }]
            (const ProbeReply&, bool) mutable noexcept
            {}
        );
        deliver(pending.callback, {.owner = 23});
        auto task = std::move(main_tasks.back());
        main_tasks.pop_back();
        task();
        check(pending_at_capture_destroy == 1 && reaper.pending() == 0,
              "handler capture is destroyed before posted accounting reaches zero");
    }

    // Destruction abandons an already-posted call without invalidating its
    // control block. The queued call remains the unique compensation owner and
    // can run safely after the Reaper object itself is gone.
    {
        std::vector<Task> main_tasks;
        int owner_destroys = 0;
        int handler_calls = 0;
        bool compensation_only = false;
        auto reaper = std::make_unique<Reaper>(
            [&main_tasks](Task task)
            {
                main_tasks.push_back(std::move(task));
                return true;
            }
        );
        auto pending = Factory::make();
        reaper->track(
            std::move(pending.request),
            [owner = OwnerProbe{owner_destroys},
             &handler_calls,
             &compensation_only]
            (const ProbeReply& reply, bool compensate) mutable noexcept
            {
                ++handler_calls;
                compensation_only = compensate && reply.owner == 31;
            }
        );
        deliver(pending.callback, {.owner = 31});
        reaper.reset();
        auto task = std::move(main_tasks.back());
        main_tasks.pop_back();
        task();
        check(handler_calls == 1 && compensation_only
                  && owner_destroys == 1,
              "posted reply compensates safely after reaper destruction");
    }

    // An expired executor generation refuses the phase hop. The reaper must
    // still consume the transferred raw owner synchronously, but must never
    // expose that reply as a normal business completion.
    {
        int owner_destroys = 0;
        int handler_calls  = 0;
        bool compensation_only = false;
        Reaper reaper(
            [](Task) noexcept
            { return false; }
        );
        auto pending = Factory::make();
        reaper.track(
            std::move(pending.request),
            [owner = OwnerProbe{owner_destroys},
             &handler_calls,
             &compensation_only]
            (const ProbeReply& reply, bool compensate) mutable noexcept
            {
                ++handler_calls;
                compensation_only = compensate && reply.owner == 123;
            }
        );
        deliver(pending.callback, {.owner = 123});
        check(reaper.pending() == 0 && handler_calls == 1
                  && compensation_only && owner_destroys == 1,
              "rejected main hop compensates and releases owner synchronously");
        reaper.abandon();
        check(reaper.pending() == 0 && handler_calls == 1
                  && owner_destroys == 1,
              "post rejection remains exactly-once across later abandon");
    }

    if (failures == 0)
        std::puts("ALL CHECKS PASSED");
    else
        std::printf("%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
