#include "verilated.h"

void MonitoredValueBase::subscribe(MonitoredValueCallback& callback) {
    std::unique_lock<std::mutex> lck(m_mtx);
    callback.m_mon_val = this;
    m_callbacks.push_back(&callback);
}

void MonitoredValueBase::unsubscribe(MonitoredValueCallback& callback) {
    std::unique_lock<std::mutex> lck(m_mtx);
    callback.m_mon_val = nullptr;
    auto it = std::remove(m_callbacks.begin(), m_callbacks.end(), &callback);
    m_callbacks.erase(it, m_callbacks.end());
}

void MonitoredValueBase::written() {
    for (auto callback : m_callbacks) { (*callback)(); }
}

MonitoredValueCallback::~MonitoredValueCallback() {
    if (m_mon_val) m_mon_val->unsubscribe(*this);
}

void MonitoredValueCallback::operator()() { m_callback(); }

void Monitor::off() { m_callbacks.clear(); }

void Monitor::on() {
    if (!m_callbacks.empty()) return;
    m_mon_vals.reserve(m_mon_vals.size());
    for (auto mon_val : m_mon_vals) { m_callbacks.emplace_back(mon_val, m_func); }
}

VerilatedThreadPool::~VerilatedThreadPool() {
    for (auto thread : m_threads) delete thread;
}

VerilatedThread* VerilatedThreadPool::run_once(std::function<void(VerilatedThread*)> func,
                                               const std::string& name) {
    std::unique_lock<std::mutex> lck(m_mtx);
    if (!m_free_threads.empty()) {
        auto* thread = m_free_threads.back();
        thread->func(func);
        thread->name(name);
        thread->kick();
        m_free_threads.pop_back();
        return thread;
    } else {
        m_threads.push_back(new VerilatedThread(m_context, func));
        m_threads.back()->name(name);
        m_threads.back()->kick();
        return m_threads.back();
    }
}

void VerilatedThreadPool::free(VerilatedThread* thread) {
    std::unique_lock<std::mutex> lck(m_mtx);
    m_free_threads.push_back(thread);
}

void VerilatedThreadPool::idle(bool flag) {
    std::unique_lock<std::mutex> lck(m_idle_mtx);
    if (flag) {
        m_idle_counter++;
        m_idle_cv.notify_all();
    } else
        m_idle_counter--;
}

void VerilatedThreadPool::wait_for_idle() {
    std::unique_lock<std::mutex> lck(m_idle_mtx);
    m_idle_counter++;
    while (m_idle_counter != m_threads.size() + 1) {  // The +1 is for the main thread
        m_idle_cv.wait(lck);
    }
    m_idle_counter--;
}

void VerilatedThreadPool::should_exit(bool flag) {
    for (size_t i = 0;; i++) {
        std::unique_lock<std::mutex> lck(m_mtx);
        if (i < m_threads.size()) {
            auto* thread = m_threads[i];
            lck.unlock();
            thread->should_exit(flag);
        } else
            break;
    }
}

void VerilatedThread::Join::joined() {
    std::unique_lock<std::mutex> lck(m_thread.m_mtx);
    m_counter++;
    if (m_counter == m_expected) { m_thread.m_cv.notify_all(); }
}

void VerilatedThread::Join::await() {
    std::unique_lock<std::mutex> lck(m_thread.m_mtx);
    m_thread.m_idle = true;
    m_thread.m_context->dynamic->thread_pool.idle(true);
    while (!m_thread.should_exit() && m_counter < m_expected) { m_thread.m_cv.wait(lck); }
    m_thread.m_idle = false;
    m_thread.m_context->dynamic->thread_pool.idle(false);
}

void VerilatedThread::wait_internal(std::atomic_bool& done, MonitoredValueCallback&&...) {
    std::unique_lock<std::mutex> lck(m_mtx);
    set_idle(true);
    while (!should_exit() && !done) m_cv.wait(lck);
}

void VerilatedThread::set_idle(bool idle) {
    if (m_idle != idle) {
        m_idle = idle;
        m_context->dynamic->thread_pool.idle(idle);
        m_cv.notify_all();
    }
}

VerilatedThread::VerilatedThread(VerilatedContext* contextp,
                                 std::function<void(VerilatedThread*)> func)
    : m_func(func)
    , m_ready(false)
    , m_joined(false)
    , m_should_exit(false)
    , m_idle(false)
    , m_context(contextp)
    , m_name("forked_thread") {
    m_thr = std::thread([this]() {
        do {
            wait_for_ready();
            if (!should_exit()) m_func(this);
            ready(false);
            m_context->dynamic->thread_pool.free(this);
        } while (!should_exit());
        idle(true);
    });
}

void VerilatedThread::should_exit(bool e) {
    std::unique_lock<std::mutex> lck_d(m_mtx);
    m_should_exit = e;
    m_cv.notify_all();
}

void VerilatedThread::ready(bool r) {
    std::unique_lock<std::mutex> lck(m_mtx);
    m_ready = r;
    m_cv.notify_all();
}

void VerilatedThread::wait_for_ready() {
    std::unique_lock<std::mutex> lck(m_mtx);

    set_idle(true);
    while (!m_ready && !m_should_exit) { m_cv.wait(lck); }
    set_idle(false);
}

void VerilatedThread::wait_for_idle() {
    std::unique_lock<std::mutex> lck(m_mtx);

    m_context->dynamic->thread_pool.idle(true);
    while (m_ready && !m_should_exit && !m_idle) { m_cv.wait(lck); }
    m_context->dynamic->thread_pool.idle(false);
}

void VerilatedThread::idle(bool w) {
    std::unique_lock<std::mutex> lck(m_mtx);
    set_idle(w);
}

void VerilatedThread::func(std::function<void(VerilatedThread*)> func) {
    std::unique_lock<std::mutex> lck(m_mtx);
    m_func = func;
}

void VerilatedThread::join() {
    if (!m_joined) m_thr.join();
    m_joined = true;
}

void VerilatedThread::kick() {
    std::unique_lock<std::mutex> lck(m_mtx);
    m_ready = true;
    m_cv.notify_all();
    if (!should_exit()) { m_cv.wait(lck); }
}

void VerilatedThread::wait_for_time(vluint64_t time) {
    std::unique_lock<std::mutex> lck(m_mtx);
    m_context->dynamic->timed_queue.push(time, this);
    set_idle(true);
    while (m_idle && !should_exit()) { m_context->dynamic->timed_queue.m_cv.wait(lck); }
    set_idle(false);
}

void VerilatedThread::exit() {
    should_exit(true);
    m_context->dynamic->timed_queue.m_cv.notify_all();
    join();
}

void VerilatedDynamicContext::timeBackwardsError() VL_MT_SAFE {
    // Slowpath
    VL_FATAL_MT("unknown", 0, "", "Time attempted to flow backwards");
    VL_UNREACHABLE
}

bool VerilatedDynamicContext::timedQEmpty() VL_MT_SAFE { return timed_queue.empty(); }

vluint64_t VerilatedDynamicContext::timedQEarliestTime() VL_MT_SAFE {
    // wait for all threads to be in idle state first, otherwise
    // we might not have the real earliest time yet
    thread_pool.wait_for_idle();

    return timed_queue.earliestTime();
}

void VerilatedDynamicContext::timedQPush(vluint64_t time, VerilatedThread* thread) VL_MT_SAFE {
    timed_queue.push(time, thread);
}

void VerilatedDynamicContext::timedQActivate(vluint64_t time) VL_MT_SAFE {
    timed_queue.activate(time);
}

void VerilatedDynamicContext::timedQWait(std::unique_lock<std::mutex>& lck) VL_MT_SAFE {
    timed_queue.m_cv.wait(lck);
}
