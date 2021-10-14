#ifndef VERILATOR_VERILATED_DYNAMIC_SCHEDULER_H_
#define VERILATOR_VERILATED_DYNAMIC_SCHEDULER_H_

#ifndef VERILATOR_VERILATED_H_INTERNAL_
#error "verilated_dynamic_scheduler.h should only be included by verilated.h"
#endif

// Dynamic scheduler-related includes
#include <vector>
#include <list>
#include <map>
#include <algorithm>
#include <thread>
#include <functional>
#include <utility>
#include <queue>
#include <coroutine>
#include <unordered_map>
#include <functional>

class VerilatedNBACtrl final {
private:
    std::vector<std::function<void()>> assignments;

public:
    template <typename T, typename U> void schedule(T& lhs, const U& rhs) {
        assignments.push_back([&lhs, rhs]() mutable { lhs = rhs; });
    }

    void schedule(std::function<void()> expr) { assignments.push_back(expr); }

    void assign() {
        for (auto const& assignment : assignments) assignment();
        assignments.clear();
    }
};

using Task = std::function<void()>;

struct TimedQueue {
    using TimedTask = std::pair<int, Task>;

    struct CustomCompare {
        bool operator()(const TimedTask& lhs, const TimedTask& rhs) {
            return lhs.first > rhs.first;
        }
    };

    std::priority_queue<TimedTask, std::vector<TimedTask>, CustomCompare> queue;

    void push(int time, Task fn) { queue.push(std::make_pair(time, fn)); }

    void activate(int time, std::vector<Task>& tasks) {
        while (!queue.empty() && queue.top().first <= time) {
            tasks.push_back(queue.top().second);
            queue.pop();
        }
    }

    int nextTimeSlot() { return queue.top().first; }

    bool empty() { return queue.empty(); }
};

using Event = void*;

struct EventMap {
    std::unordered_map<Event, Task> events;

    void insert(Event event, Task task) {
        auto it = events.find(event);
        if (it != events.end()) {
            auto f = it->second;
            task = [f, task]() {
                f();
                task();
            };
        }
        events.insert(std::make_pair(event, task));
    }

    void activate(Event event, std::vector<Task>& tasks) {
        auto it = events.find(event);
        if (it != events.end()) {
            tasks.push_back(it->second);
            events.erase(it);
        }
    }
};

struct CoroutineTask;

struct CoroutineTaskPromise {
    std::coroutine_handle<> continuation;

    CoroutineTask get_return_object();

    std::suspend_never initial_suspend() { return {}; }

    void unhandled_exception() { std::terminate(); }

    auto final_suspend() noexcept {
        struct Awaitable {
            std::coroutine_handle<> continuation;

            bool await_ready() noexcept { return false; }
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<CoroutineTaskPromise> coro) noexcept {
                return continuation ? continuation : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        return Awaitable{continuation};
    }

    void return_void() const {}

    auto await_transform(std::pair<TimedQueue&, int> t) {
        struct Awaitable {
            TimedQueue& queue;
            int time;

            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<CoroutineTaskPromise> coro) {
                queue.push(time, [coro]() { coro.resume(); });
            }
            void await_resume() {}
        };
        return Awaitable{t.first, t.second};
    }

    auto await_transform(std::pair<EventMap&, Event> e) {
        struct Awaitable {
            EventMap& map;
            Event event;

            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<CoroutineTaskPromise> coro) {
                map.insert(event, [coro]() { coro.resume(); });
            }
            void await_resume() {}
        };
        return Awaitable{e.first, e.second};
    }

    auto await_transform(CoroutineTask&& coro_task);
};

struct CoroutineTask {
    using promise_type = CoroutineTaskPromise;

    CoroutineTask(std::coroutine_handle<promise_type> coro)
        : handle(coro) {}

    CoroutineTask(CoroutineTask&& other)
        : handle(std::exchange(other.handle, nullptr)) {}

    CoroutineTask& operator=(CoroutineTask&& other) {
        if (handle) handle.destroy();
        handle = std::exchange(other.handle, nullptr);
        return *this;
    }

    ~CoroutineTask() {
        if (handle) handle.destroy();
    }

    std::coroutine_handle<promise_type> handle;
};

// We need this as forks can outlive the locals from the stackframe they were spawned in
struct CoroutinePool {
    std::vector<CoroutineTask> coro_tasks;
    std::vector<std::function<CoroutineTask()>> lambda_coros;

    void run(std::function<CoroutineTask()> lambda_coro) {
        lambda_coros.push_back(lambda_coro);  // Store the captured vars
        coro_tasks.emplace_back(lambda_coros.back()());
    }
};

// static void fork(std::function<CoroutineTask ()> lambda_coro) {
static void fork(CoroutineTask&& task) {
    static CoroutinePool coro_pool;
    // coro_pool.run(lambda_coro);
    coro_pool.coro_tasks.push_back(std::move(task));
}

inline auto CoroutineTaskPromise::await_transform(CoroutineTask&& coro_task) {
    struct Awaitable {
        std::coroutine_handle<CoroutineTaskPromise> handle;

        bool await_ready() { return static_cast<bool>(handle); }
        void await_suspend(std::coroutine_handle<> coro) { handle.promise().continuation = coro; }
        auto await_resume() {}
    };
    return Awaitable{coro_task.handle};
}

inline CoroutineTask CoroutineTaskPromise::get_return_object() {
    return {std::coroutine_handle<CoroutineTaskPromise>::from_promise(*this)};
}

#endif  // Guard
