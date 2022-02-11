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
    VerilatedMutex m_mutex;

    void push(double time, std::coroutine_handle<> coro) {
        const VerilatedLockGuard guard{m_mutex};
        queue.push(std::make_pair(time, coro));
    }

    void activate(double time) {
        VerilatedLockGuard guard{m_mutex};
        while (!queue.empty() && queue.top().first <= time) {
            auto coro = queue.top().second;
            queue.pop();
            guard.unlock();
            coro();
            guard.lock();
        }
    }

    double nextTimeSlot() {
        const VerilatedLockGuard guard{m_mutex};
        if (!queue.empty())
            return queue.top().first;
        else
            return VL_TIME_D();
    }

    bool empty() {
        const VerilatedLockGuard guard{m_mutex};
        return queue.empty();
    }

    auto operator[](double time) {
        const VerilatedLockGuard guard{m_mutex};
        struct Awaitable {
            DelayedQueue& queue;
            double time;

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> coro) { queue.push(time, coro); }

            void await_resume() {}
        };
        return Awaitable{*this, time};
    }
};

using Event = CData;

using EventSet = std::unordered_set<Event*>;

struct EventSetToCoroMap {
private:
    struct Hash {
        size_t operator()(const EventSet& set) const {
            size_t result = 0;
            for (auto event : set) result ^= std::hash<Event*>()(event);
            return result;
        }
    };
    std::unordered_multimap<EventSet, std::coroutine_handle<>, Hash> eventSetsToCoros;
    std::unordered_multimap<Event*, EventSet> eventsToEventSets;

    void move(const EventSet& events, std::vector<std::coroutine_handle<>>& coros) {
        auto range = eventSetsToCoros.equal_range(events);
        for (auto it = range.first; it != range.second; ++it) coros.push_back(it->second);
        eventSetsToCoros.erase(range.first, range.second);
    }

public:
    void move(Event* event, std::vector<std::coroutine_handle<>>& coros) {
        auto range = eventsToEventSets.equal_range(event);
        for (auto it = range.first; it != range.second; ++it) move(it->second, coros);
    }

    bool contains(const EventSet& events) {
        auto range = eventSetsToCoros.equal_range(events);
        return range.first != range.second;
    }

    void insert(const EventSet& events, std::coroutine_handle<> coro) {
        for (auto event : events) eventsToEventSets.insert(std::make_pair(event, events));
        eventSetsToCoros.insert(std::make_pair(events, coro));
    }
};

struct EventDispatcher {
private:
    EventSetToCoroMap eventSetsToCoros;
    std::vector<Event*> triggeredEvents;
    std::vector<std::coroutine_handle<>> primedCoros;
    VerilatedMutex m_mutex;

public:
    void insert(const EventSet& events, std::coroutine_handle<> coro) {
        if (isSetWaiting(events)) {
            primeTriggered();
        }
        VerilatedLockGuard guard{m_mutex};
        eventSetsToCoros.insert(events, coro);
    }

    void primeTriggered() {
        VerilatedLockGuard guard{m_mutex};
        std::vector<Event*> queue = std::move(triggeredEvents);
        for (auto event : queue) eventSetsToCoros.move(event, primedCoros);
    }

    void resumeTriggered(Event& dlyEvent) {
        do {
            resumeTriggered();
            trigger(dlyEvent);
            primeTriggered();
        } while (!primedEmpty());
    }

    void resumeTriggered() {
        primeTriggered();
        while (!primedEmpty()) {
            m_mutex.lock();
            std::vector<std::coroutine_handle<>> queue = std::move(primedCoros);
            m_mutex.unlock();
            for (auto coro : queue) coro();
            primeTriggered();
        }
    }

    bool primedEmpty() {
        const VerilatedLockGuard guard{m_mutex};
        return primedCoros.empty();
    }

    bool isSetWaiting(const EventSet& events) {
        const VerilatedLockGuard guard{m_mutex};
        return eventSetsToCoros.contains(events);
    }

    void trigger(Event& event) {
        event = 1;
        const VerilatedLockGuard guard{m_mutex};
        triggeredEvents.push_back(&event);
    }

    auto operator[](EventSet&& events) {
        struct Awaitable {
            EventDispatcher& dispatcher;
            EventSet events;

            bool await_ready() { return false; }

            void await_suspend(std::coroutine_handle<> coro) { dispatcher.insert(events, coro); }

            void await_resume() {}
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
