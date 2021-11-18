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
#include <list>
#include <utility>
#include <queue>
#include <coroutine>
#include <unordered_map>
#include <unordered_set>
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

    int nextTimeSlot() {
        if (!empty())
            return queue.top().first;
        else
            return VL_TIME_Q();
    }

    bool empty() { return queue.empty(); }
};

using Event = void*;

using EventSet = std::unordered_set<Event>;
struct EventMap {
    struct Hash {
        size_t operator()(const EventSet& set) const {
            size_t result = 0;
            for (auto event : set) result ^= std::hash<Event>()(event);
            return result;
        }
    };
    std::unordered_multimap<EventSet, Task, Hash> eventSetsToTasks;
    std::unordered_multimap<Event, EventSet> eventsToEventSets;

    void insert(const EventSet& events, Task task) {
        for (auto event : events) { eventsToEventSets.insert(std::make_pair(event, events)); }
        eventSetsToTasks.insert(std::make_pair(events, task));
    }

    void activate(const EventSet& events, std::vector<Task>& tasks) {
        auto range = eventSetsToTasks.equal_range(events);
        for (auto it = range.first; it != range.second; ++it) { tasks.push_back(it->second); }
        eventSetsToTasks.erase(range.first, range.second);
    }

    void activate(Event event, std::vector<Task>& tasks) {
        auto range = eventsToEventSets.equal_range(event);
        for (auto it = range.first; it != range.second; ++it) { activate(it->second, tasks); }
    }
};

struct CoroutineTask;

struct CoroutineTaskPromise {
    std::coroutine_handle<> continuation;
    bool done = false;

    CoroutineTask get_return_object();

    std::suspend_never initial_suspend() { return {}; }

    void unhandled_exception() { std::terminate(); }

    std::suspend_never final_suspend() noexcept {
        done = true;
        if (continuation) continuation.resume();
        return {};
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

    auto await_transform(std::pair<EventMap&, EventSet> e) {
        struct Awaitable {
            EventMap& map;
            EventSet events;

            bool await_ready() { return false; }
            void await_suspend(std::coroutine_handle<CoroutineTaskPromise> coro) {
                map.insert(events, [coro]() { coro.resume(); });
            }
            void await_resume() {}
        };
        return Awaitable{e.first, e.second};
    }

    auto await_transform(const CoroutineTask& coro_task);
};

struct CoroutineTask {
    using promise_type = CoroutineTaskPromise;

    CoroutineTask(std::coroutine_handle<promise_type> coro)
        : handle(coro) {}
    CoroutineTask(const CoroutineTask& other)
        : handle(other.handle) {}

    std::coroutine_handle<promise_type> handle;
};

inline auto CoroutineTaskPromise::await_transform(const CoroutineTask& coro_task) {
    struct Awaitable {
        std::coroutine_handle<CoroutineTaskPromise> handle;

        bool await_ready() { return !handle || handle.promise().done; }
        void await_suspend(std::coroutine_handle<> coro) { handle.promise().continuation = coro; }
        auto await_resume() {}
    };
    return Awaitable{coro_task.handle};
}

inline CoroutineTask CoroutineTaskPromise::get_return_object() {
    return {std::coroutine_handle<CoroutineTaskPromise>::from_promise(*this)};
}

#endif  // Guard
