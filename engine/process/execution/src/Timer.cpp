#include <lux/engine/process/Timer.hpp>

#include <lux/engine/process/detail/TimerFailureInjection.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

namespace lux::process::detail
{
    struct TimerState final
    {
        explicit TimerState(std::size_t requested_capacity)
            : capacity(requested_capacity)
        {
            heap.reserve(capacity);
            cancelled.reserve(capacity);
            shutdown_scratch.reserve(capacity);
        }

        ~TimerState()
        {
            stop();
        }

        TimerState(const TimerState&) = delete;
        TimerState& operator=(const TimerState&) = delete;

        void startWorker()
        {
            worker = std::jthread([this](std::stop_token token) noexcept {
                run(token);
            });
        }

        void swapNodes(std::size_t lhs, std::size_t rhs) noexcept
        {
            std::swap(heap[lhs], heap[rhs]);
            heap[lhs]->heap_index = lhs;
            heap[rhs]->heap_index = rhs;
        }

        [[nodiscard]] bool earlier(std::size_t lhs, std::size_t rhs) const noexcept
        {
            return heap[lhs]->deadline < heap[rhs]->deadline;
        }

        void siftUp(std::size_t index) noexcept
        {
            while (index != 0U)
            {
                const auto parent = (index - 1U) / 2U;
                if (!earlier(index, parent))
                    break;
                swapNodes(index, parent);
                index = parent;
            }
        }

        void siftDown(std::size_t index) noexcept
        {
            for (;;)
            {
                const auto left = index * 2U + 1U;
                if (left >= heap.size())
                    return;

                auto smallest = left;
                const auto right = left + 1U;
                if (right < heap.size() && earlier(right, left))
                    smallest = right;
                if (!earlier(smallest, index))
                    return;

                swapNodes(index, smallest);
                index = smallest;
            }
        }

        void removeAt(std::size_t index) noexcept
        {
            const auto last = heap.size() - 1U;
            auto* removed = heap[index];
            if (index != last)
            {
                heap[index] = heap[last];
                heap[index]->heap_index = index;
            }
            heap.pop_back();
            removed->queued = false;
            removed->heap_index = static_cast<std::size_t>(-1);

            if (index >= heap.size())
                return;
            const bool should_sift_up = index != 0U && earlier(index, (index - 1U) / 2U);
            if (should_sift_up)
                siftUp(index);
            else
                siftDown(index);
        }

        void run(std::stop_token token) noexcept
        {
            std::unique_lock lock{mutex};
            for (;;)
            {
                if (!cancelled.empty())
                {
                    auto* request = cancelled.back();
                    cancelled.pop_back();
                    --outstanding;
                    const auto complete = request->complete;
                    lock.unlock();
                    complete(request, true);
                    lock.lock();
                    continue;
                }
                if (stopping || token.stop_requested())
                    return;
                if (heap.empty())
                {
                    cv.wait(lock, [this, &token] {
                        return stopping || token.stop_requested() || !cancelled.empty() || !heap.empty();
                    });
                    continue;
                }

                const auto deadline = heap.front()->deadline;
                const bool interrupted = cv.wait_until(lock, deadline, [this, deadline, &token] {
                    return stopping || token.stop_requested() || !cancelled.empty() || heap.empty() ||
                        heap.front()->deadline < deadline;
                });
                if (interrupted || heap.empty())
                    continue;

                auto* request = heap.front();
                if (request->deadline > TimerRequest::Clock::now())
                    continue;
                removeAt(0U);
                --outstanding;
                const auto complete = request->complete;
                lock.unlock();
                complete(request, false);
                lock.lock();
            }
        }

        void stop() noexcept
        {
            bool initiated{};
            {
                std::lock_guard lock{mutex};
                if (!stopping)
                {
                    initiated = true;
                    stopping = true;
                    shutdown_scratch.insert(shutdown_scratch.end(), cancelled.begin(), cancelled.end());
                    cancelled.clear();
                    shutdown_scratch.insert(shutdown_scratch.end(), heap.begin(), heap.end());
                    heap.clear();
                    outstanding = 0U;
                    for (auto* request : shutdown_scratch)
                    {
                        request->queued = false;
                        request->heap_index = static_cast<std::size_t>(-1);
                    }
                }
            }

            worker.request_stop();
            cv.notify_all();
            if (initiated)
            {
                for (auto* request : shutdown_scratch)
                    request->complete(request, true);
                shutdown_scratch.clear();
            }
            if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
                worker.join();
        }

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<TimerRequest*> heap;
        std::vector<TimerRequest*> cancelled;
        std::vector<TimerRequest*> shutdown_scratch;
        std::size_t capacity{};
        std::size_t outstanding{};
        bool stopping{};
        std::jthread worker;
    };

    TimerSubmitResult submitTimer(const std::shared_ptr<TimerState>& state, TimerRequest& request) noexcept
    {
        if (!state)
            return lux::cxx::unexpected(ETimerError::STOPPING);

        {
            std::lock_guard lock{state->mutex};
            if (state->stopping)
                return lux::cxx::unexpected(ETimerError::STOPPING);
            if (request.cancel_requested.load(std::memory_order_acquire))
                return ETimerSubmitStatus::STOPPED;
            if (request.queued)
                return lux::cxx::unexpected(ETimerError::BACKEND_FAILURE);
            if (state->outstanding >= state->capacity)
                return lux::cxx::unexpected(ETimerError::CAPACITY_EXCEEDED);

            ++state->outstanding;
            request.queued = true;
            request.heap_index = state->heap.size();
            state->heap.push_back(&request);
            state->siftUp(request.heap_index);
        }
        state->cv.notify_one();
        return ETimerSubmitStatus::SUBMITTED;
    }

    void cancelTimer(TimerState* state, TimerRequest& request) noexcept
    {
        if (state == nullptr)
            return;
        {
            std::lock_guard lock{state->mutex};
            if (!request.queued)
                return;
            const auto index = request.heap_index;
            const bool is_valid_index = index < state->heap.size() && state->heap[index] == &request;
            if (!is_valid_index)
                return;
            state->removeAt(index);
            state->cancelled.push_back(&request);
        }
        state->cv.notify_one();
    }

    void stopTimerState(const std::shared_ptr<TimerState>& state) noexcept
    {
        if (state)
            state->stop();
    }
}

namespace lux::process
{
    TimerQueue::CreateResult TimerQueue::create(TimerQueueConfig config) noexcept
    {
        if (config.capacity == 0U)
            return lux::cxx::unexpected(ETimerError::INVALID_ARGUMENT);

        const auto injected = detail::testing::consumeTimerCreateFailure();
        if (injected == detail::testing::ETimerCreateFailure::ALLOCATION)
            return lux::cxx::unexpected(ETimerError::ALLOCATION_FAILURE);

        try
        {
            auto state = std::make_shared<detail::TimerState>(config.capacity);
            if (injected == detail::testing::ETimerCreateFailure::WORKER)
                return lux::cxx::unexpected(ETimerError::WORKER_CREATION_FAILURE);
            if (injected == detail::testing::ETimerCreateFailure::BACKEND)
                return lux::cxx::unexpected(ETimerError::BACKEND_FAILURE);
            state->startWorker();
            return TimerQueue{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(ETimerError::ALLOCATION_FAILURE);
        }
        catch (const std::system_error&)
        {
            return lux::cxx::unexpected(ETimerError::WORKER_CREATION_FAILURE);
        }
        catch (...)
        {
            return lux::cxx::unexpected(ETimerError::BACKEND_FAILURE);
        }
    }

    TimerQueue::~TimerQueue()
    {
        requestStop();
    }

    TimerQueue::TimerQueue(TimerQueue&&) noexcept = default;
    TimerQueue& TimerQueue::operator=(TimerQueue&&) noexcept = default;

    void TimerQueue::requestStop() noexcept
    {
        detail::stopTimerState(state_);
    }
}
