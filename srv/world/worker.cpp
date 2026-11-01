#include "worker.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace srv::generation
{
    namespace
    {
        struct state {
            std::mutex mutex;
            std::condition_variable wake;

            std::deque<job> pending;
            std::deque<completion> completed;
            std::exception_ptr failure;

            std::thread worker;
            bool stop = false;
        };

        state& get()
        {
            static state data;
            return data;
        }

        void run()
        {
            auto& data = get();

            for (;;)
            {
                job work;

                {
                    std::unique_lock<std::mutex> lock(data.mutex);

                    data.wake.wait(lock, [&] {
                        return data.stop || !data.pending.empty();
                    });

                    if (data.stop)
                    {
                        break;
                    }

                    work = std::move(data.pending.front());
                    data.pending.pop_front();
                }
                try
                {
                    completion result = work();

                    if (result)
                    {
                        std::lock_guard<std::mutex> lock(data.mutex);
                        data.completed.push_back(std::move(result));
                    }
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(data.mutex);
                    if (!data.failure)
                    {
                        data.failure = std::current_exception();
                    }
                    data.pending.clear();
                }
            }
        }
    } // namespace

    void init()
    {
        auto& data = get();

        if (data.worker.joinable())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(data.mutex);
            data.stop = false;
            data.failure = {};
        }
        data.worker = std::thread(run);
    }

    bool submit(job work)
    {
        auto& data = get();
        std::lock_guard<std::mutex> lock(data.mutex);

        if (data.stop || data.failure || !data.worker.joinable() ||
            data.pending.size() >= 64)
        {
            return false;
        }

        data.pending.push_back(std::move(work));
        data.wake.notify_one();

        return true;
    }

    void commit()
    {
        auto& data = get();
        std::deque<completion> tasks;
        std::exception_ptr failure;

        {
            std::lock_guard<std::mutex> lock(data.mutex);
            failure = data.failure;

            for (int i = 0; i < 2 && !data.completed.empty(); ++i)
            {
                tasks.push_back(std::move(data.completed.front()));
                data.completed.pop_front();
            }
        }

        if (failure)
        {
            std::rethrow_exception(failure);
        }

        for (auto& task : tasks)
        {
            task();
        }
    }

    void shutdown()
    {
        auto& data = get();

        {
            std::lock_guard<std::mutex> lock(data.mutex);
            data.stop = true;
            data.pending.clear();
        }

        data.wake.notify_all();

        if (data.worker.joinable())
        {
            data.worker.join();
        }

        std::lock_guard<std::mutex> lock(data.mutex);
        data.completed.clear();
        data.failure = {};
    }
} // namespace srv::generation
