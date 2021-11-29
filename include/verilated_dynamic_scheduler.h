#ifndef VERILATOR_VERILATED_DYNAMIC_SCHEDULER_H_
#define VERILATOR_VERILATED_DYNAMIC_SCHEDULER_H_

#ifndef VERILATOR_VERILATED_H_INTERNAL_
#error "verilated_dynamic_scheduler.h should only be included by verilated.h"
#endif

// Dynamic scheduler-related includes
#include <vector>
#include <map>
#include <functional>
#include <utility>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#ifdef __clang__
#ifdef _LIBCPP_VERSION
// Using libc++, coroutine library is in std::experimental
#include <experimental/coroutine>
namespace std {
using namespace experimental;  // Bring std::experimental into the std namespace
}
#else
// Using stdlibc++, coroutine library is in std namespace
#define __cpp_impl_coroutine 1  // clang doesn't define this, but it's needed in <coroutine>
#include <coroutine>
namespace std {
namespace experimental
    = ::std;  // Bring coroutine library into std::experimental, as clang expects it to be there
}
#endif
#else  // Not clang
#include <coroutine>
#endif

struct DelayedQueue {
    using DelayedCoro = std::pair<double, std::coroutine_handle<>>;

    struct CustomCompare {
        bool operator()(const DelayedCoro& lhs, const DelayedCoro& rhs) {
            return lhs.first > rhs.first;
        }
    };

    std::priority_queue<DelayedCoro, std::vector<DelayedCoro>, CustomCompare> queue;

    void push(double time, std::coroutine_handle<> coro) {
        queue.push(std::make_pair(time, coro));
    }

    void activate(double time, std::vector<std::coroutine_handle<>>& coros) {
        while (!queue.empty() && queue.top().first <= time) {
            coros.push_back(queue.top().second);
            //XXX wake up immediately?
            //auto coro = queue.top().second;
            queue.pop();
            //coro();
        }
    }

    void activate(double time, std::vector<std::function<void()>>& tasks) {
        while (!queue.empty() && queue.top().first <= time) {
            auto coro = queue.top().second;
            tasks.push_back([coro]() mutable { coro.resume(); });
            queue.pop();
        }
    }

    double nextTimeSlot() {
        if (!empty())
            return queue.top().first;
        else
            return VL_TIME_D();
    }

    bool empty() { return queue.empty(); }

    auto operator[](double time) {
        struct Awaitable {
            DelayedQueue& queue;
            double time;

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> coro) {
                queue.push(time, coro);
            }

            void await_resume() {}
        };
        return Awaitable{*this, time};
    }
};

using Event = void*;

using EventSet = std::unordered_set<Event>;
struct EventDispatcher {
    struct AwaitingCoro {
        std::coroutine_handle<> coro;
        Event& triggeringEvent;
    };

    struct Hash {
        size_t operator()(const EventSet& set) const {
            size_t result = 0;
            for (auto event : set) result ^= std::hash<Event>()(event);
            return result;
        }
    };

    std::unordered_multimap<EventSet, AwaitingCoro, Hash> eventSetsToCoros;
    std::unordered_multimap<Event, EventSet> eventsToEventSets;

    void insert(const EventSet& events, AwaitingCoro coro) {
        for (auto event : events) {
            eventsToEventSets.insert(std::make_pair(event, events));
        }
        eventSetsToCoros.insert(std::make_pair(events, coro));
    }

    void activate(Event triggeringEvent, const EventSet& events, std::vector<std::coroutine_handle<>>& coros) {
        auto range = eventSetsToCoros.equal_range(events);
        for (auto it = range.first; it != range.second; ++it) {
            it->second.triggeringEvent = triggeringEvent;
            coros.push_back(it->second.coro);
        }
        eventSetsToCoros.erase(range.first, range.second);
    }

    void activate(Event triggeringEvent, std::vector<std::coroutine_handle<>>& coros) {
        auto range = eventsToEventSets.equal_range(triggeringEvent);
        for (auto it = range.first; it != range.second; ++it) {
            activate(triggeringEvent, it->second, coros);
        }
    }

    //XXX wake up immediately?
    //void activate(Event triggeringEvent, const EventSet& events, std::vector<std::coroutine_handle<>>& coros) {
    //    auto range = eventSetsToCoros.equal_range(events);
    //    for (auto it = range.first; it != range.second; ++it) {
    //        it->second.triggeringEvent = triggeringEvent;
    //        coros.push_back(it->second.coro);
    //    }
    //    eventSetsToCoros.erase(range.first, range.second);
    //}

    //void activate(Event triggeringEvent, std::vector<std::coroutine_handle<>>&) {
    //    auto range = eventsToEventSets.equal_range(triggeringEvent);
    //    std::vector<std::coroutine_handle<>> coros;
    //    for (auto it = range.first; it != range.second; ++it) {
    //        activate(triggeringEvent, it->second, coros);
    //    }
    //    for (auto coro : coros) coro();
    //}
    
    auto operator[](EventSet&& events) {
        struct Awaitable {
            EventDispatcher& dispatcher;
            EventSet events;
            Event triggeringEvent;

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> coro) {
                dispatcher.insert(events, {coro, triggeringEvent});
            }

            Event await_resume() { return triggeringEvent; }
        };
        return Awaitable{*this, std::move(events)};
    }
};

struct CoroutineTask {
    struct promise_type {
        CoroutineTask get_return_object() { return {this}; }

        std::suspend_never initial_suspend() { return {}; }

        std::suspend_never final_suspend() noexcept {
            if (VL_LIKELY(task)) task->promise = nullptr;
            if (VL_UNLIKELY(continuation)) continuation.resume();
            return {};
        }

        void unhandled_exception() { std::terminate(); }

        void return_void() const {}

        std::coroutine_handle<> continuation;
        CoroutineTask* task = nullptr;
    };

    CoroutineTask(promise_type* p)
        : promise(p) {
        promise->task = this;
    }

    CoroutineTask(CoroutineTask&& other)
        : promise(std::exchange(other.promise, nullptr)) {
        if (VL_LIKELY(promise)) promise->task = this;
    }

    ~CoroutineTask() {
        if (VL_UNLIKELY(promise)) promise->task = nullptr;
    }

    bool await_ready() const noexcept { return !promise; }

    void await_suspend(std::coroutine_handle<> coro) { promise->continuation = coro; }

    void await_resume() const noexcept {}

    promise_type* promise;
};

#endif  // Guard
