module;

#include <cstddef>
#include <functional>
#include <memory>

export module vortex.core:jobs;

export namespace vortex {

    /**
     * @brief Fixed-size worker thread pool.
     * @details Used for everything that must not stall the frame: chunk generation, region I/O,
     * collider cooking, particle simulation. Jobs are plain callables executed in FIFO order.
     *
     * Rules of thumb:
     *  - a job must not block on another job submitted to the same pool (use ParallelFor instead);
     *  - results are handed back to the main thread through the owner's own queue, the pool
     *    itself does not return values.
     */
    class JobSystem {
    public:
        /// @param threadCount 0 = one thread per hardware core, minus one for the main thread.
        explicit JobSystem(unsigned threadCount = 0);
        ~JobSystem();
        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        /// @brief Queues a job. Thread safe.
        void Submit(std::function<void()> job);

        /**
         * @brief Runs fn(begin, end) over [0, count) split into ranges of `grain` items.
         * @details Blocks until every range has run; the calling thread takes part in the work,
         * so it is safe to call from inside a job.
         */
        void ParallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t)>& fn);

        /// @brief Blocks until the queue is empty and no job is running.
        void WaitIdle();

        unsigned ThreadCount() const;
        /// @brief Jobs queued or running.
        size_t PendingJobs() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
