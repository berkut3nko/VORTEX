module;

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

module vortex.core;

namespace vortex {

    struct JobSystem::Impl {
        std::vector<std::thread> threads;
        std::deque<std::function<void()>> queue;
        mutable std::mutex mutex;
        std::condition_variable wake;      // workers wait here
        std::condition_variable idle;      // WaitIdle waits here
        size_t running = 0;
        bool stop = false;

        void WorkerLoop() {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [this] { return stop || !queue.empty(); });
                    if (queue.empty()) return;   // stop requested and nothing left to do
                    job = std::move(queue.front());
                    queue.pop_front();
                    ++running;
                }
                Run(job);
                {
                    std::scoped_lock lock(mutex);
                    --running;
                    if (queue.empty() && running == 0) idle.notify_all();
                }
            }
        }

        static void Run(std::function<void()>& job) {
            try {
                job();
            } catch (const std::exception& e) {
                Log::Error("Unhandled exception in job: {}", e.what());
            } catch (...) {
                Log::Error("Unhandled non-standard exception in job");
            }
        }
    };

    JobSystem::JobSystem(unsigned threadCount) : m_Impl(std::make_unique<Impl>()) {
        if (threadCount == 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            threadCount = hw > 1 ? hw - 1 : 1;
        }
        m_Impl->threads.reserve(threadCount);
        for (unsigned i = 0; i < threadCount; ++i) {
            m_Impl->threads.emplace_back([impl = m_Impl.get()] { impl->WorkerLoop(); });
        }
    }

    JobSystem::~JobSystem() {
        {
            std::scoped_lock lock(m_Impl->mutex);
            m_Impl->stop = true;
        }
        m_Impl->wake.notify_all();
        for (auto& t : m_Impl->threads) {
            if (t.joinable()) t.join();
        }
    }

    void JobSystem::Submit(std::function<void()> job) {
        {
            std::scoped_lock lock(m_Impl->mutex);
            m_Impl->queue.push_back(std::move(job));
        }
        m_Impl->wake.notify_one();
    }

    void JobSystem::ParallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t)>& fn) {
        if (count == 0) return;
        grain = std::max<size_t>(grain, 1);
        const size_t rangeCount = (count + grain - 1) / grain;
        if (rangeCount == 1) {
            fn(0, count);
            return;
        }

        // Shared between the caller and the helper jobs. Helpers that start after the work is
        // finished find `next >= rangeCount` and return immediately, so `fn` is never touched
        // after ParallelFor returns.
        struct State {
            std::atomic<size_t> next{0};
            std::atomic<size_t> done{0};
            std::mutex mutex;
            std::condition_variable finished;
        };
        auto state = std::make_shared<State>();

        auto work = [state, count, grain, rangeCount, &fn] {
            for (;;) {
                const size_t i = state->next.fetch_add(1, std::memory_order_relaxed);
                if (i >= rangeCount) return;
                const size_t begin = i * grain;
                fn(begin, std::min(count, begin + grain));
                if (state->done.fetch_add(1, std::memory_order_acq_rel) + 1 == rangeCount) {
                    std::scoped_lock lock(state->mutex);
                    state->finished.notify_all();
                }
            }
        };

        const size_t helpers = std::min<size_t>(rangeCount - 1, m_Impl->threads.size());
        for (size_t i = 0; i < helpers; ++i) {
            // `fn` is captured by reference inside `work`; that is safe because a helper only
            // dereferences it after claiming a range, and all ranges are claimed and completed
            // before this function returns (see the wait below).
            Submit(work);
        }
        work();

        std::unique_lock lock(state->mutex);
        state->finished.wait(lock, [&] { return state->done.load(std::memory_order_acquire) >= rangeCount; });
    }

    void JobSystem::WaitIdle() {
        std::unique_lock lock(m_Impl->mutex);
        m_Impl->idle.wait(lock, [this] { return m_Impl->queue.empty() && m_Impl->running == 0; });
    }

    unsigned JobSystem::ThreadCount() const { return static_cast<unsigned>(m_Impl->threads.size()); }

    size_t JobSystem::PendingJobs() const {
        std::scoped_lock lock(m_Impl->mutex);
        return m_Impl->queue.size() + m_Impl->running;
    }
}
