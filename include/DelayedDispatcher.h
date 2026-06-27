#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Utils
{
    class DelayedDispatcher
    {
    public:
        using Task = std::move_only_function<void()>;
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        static DelayedDispatcher& Get()
        {
            static DelayedDispatcher instance;
            return instance;
        }

        template <class Rep, class Period>
        void PostDelayed(std::chrono::duration<Rep, Period> delay, Task&& task)
        {
            const auto executeAt = Clock::now() + delay;
            {
                std::scoped_lock lock(m_mutex);
                m_queue.emplace(executeAt, std::move(task));
            }
            m_cv.notify_one();
        }

        void Stop()
        {
            m_worker.request_stop();
            m_cv.notify_all();
        }

    private:
        struct ScheduledTask
        {
            TimePoint time;
            mutable Task task;

            bool operator>(const ScheduledTask& other) const
            {
                return time > other.time;
            }
        };

        DelayedDispatcher()
        {
            m_worker = std::jthread([this](std::stop_token stoken) {
                RunLoop(std::move(stoken));
            });
        }

        ~DelayedDispatcher()
        {
            Stop();
        }

        void RunLoop(std::stop_token stoken)
        {
            while (!stoken.stop_requested()) {
                Task taskToRun;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, stoken, [this] {
                        return !m_queue.empty();
                    });

                    if (stoken.stop_requested()) {
                        return;
                    }

                    const auto now = Clock::now();
                    const auto& topTask = m_queue.top();
                    if (topTask.time <= now) {
                        taskToRun = std::move(topTask.task);
                        m_queue.pop();
                    } else {
                        const auto sleepUntil = topTask.time;
                        m_cv.wait_until(lock, stoken, sleepUntil, [this, sleepUntil] {
                            return !m_queue.empty() && m_queue.top().time < sleepUntil;
                        });
                        continue;
                    }
                }

                if (taskToRun) {
                    taskToRun();
                }
            }
        }

        std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, std::greater<>> m_queue;
        std::mutex m_mutex;
        std::condition_variable_any m_cv;
        std::jthread m_worker;
    };
}
