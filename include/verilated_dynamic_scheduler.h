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
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <utility>
#include <queue>

class VerilatedEvalMsgQueue;
class VerilatedScopeNameMap;
class VerilatedTimedQueue;
class VerilatedVar;
class VerilatedVarNameMap;
class VerilatedVcd;
class VerilatedVcdC;
class VerilatedVcdSc;
class VerilatedFst;
class VerilatedFstC;
class VerilatedThread;
template <typename T, typename Enable = void> class MonitoredValue;
template <typename T> class MonitoredPointer;
class MonitoredValueCallback;

class VerilatedThreadPool final {
private:
    std::mutex m_mtx;
    std::vector<VerilatedThread*> m_threads;
    std::vector<VerilatedThread*> m_free_threads;

    std::atomic_uint m_idle_counter;
    std::mutex m_idle_mtx;
    std::condition_variable m_idle_cv;

    VerilatedContext* m_context;

public:
    VerilatedThreadPool(VerilatedContext* contextp)
        : m_idle_counter(0)
        , m_context(contextp) {}
    ~VerilatedThreadPool();
    VerilatedThread* run_once(std::function<void(VerilatedThread*)> func,
                              const std::string& name = "");
    void free(VerilatedThread* thread);
    void idle(bool w);
    void wait_for_idle();
    void should_exit(bool flag);
};

class VerilatedThread final {
public:
    class Join final {
    private:
        VerilatedThread& m_thread;
        unsigned m_expected;
        unsigned m_counter;

    public:
        Join(VerilatedThread& thread, size_t expected)
            : m_thread(thread)
            , m_expected(expected)
            , m_counter(0) {}

        void joined();
        void await();
    };

private:
    std::function<void(VerilatedThread*)> m_func;
    std::atomic<bool> m_ready;
    std::atomic<bool> m_joined;

    std::atomic<bool> m_should_exit;
    std::atomic<bool> m_idle;
    std::thread m_thr;

    std::mutex m_mtx;
    std::condition_variable m_cv;

    VerilatedContext* m_context;

    template <typename T, std::size_t I = 0, typename... Ts>
    static inline typename std::enable_if<I == sizeof...(Ts), bool>::type
    any_equal(std::tuple<Ts...>&, const T&) {
        return false;
    }

    template <typename T, std::size_t I = 0, typename... Ts>
        static inline typename std::enable_if
        < I<sizeof...(Ts), bool>::type any_equal(std::tuple<Ts...>& values, const T& value) {
        return std::get<I>(values) == value || any_equal<T, I + 1, Ts...>(values, value);
    }

    // This function is needed to create MonitoredValueCallbacks in place (as arguments) instead of
    // copying/moving them
    void wait_internal(std::atomic_bool& done, MonitoredValueCallback&&...);

    void set_idle(bool idle);

public:
    VerilatedThread(VerilatedContext* contextp, std::function<void(VerilatedThread*)> func);
    ~VerilatedThread() { exit(); }

    void should_exit(bool e);
    void ready(bool r);
    void wait_for_ready();
    void wait_for_idle();
    void idle(bool w);
    void func(std::function<void(VerilatedThread*)> func);

    void join();
    void exit();
    void kick();

    template <typename P> void wait_until(P pred) {
        std::unique_lock<std::mutex> lck(m_mtx);
        set_idle(true);
        while (!should_exit() && !pred()) { m_cv.wait(lck); }
        set_idle(false);
    }

    template <typename P, typename... Ts>
    void wait_until(P pred, MonitoredValue<Ts>&... mon_vals) {
        std::atomic_bool done(false);
        auto f = [this, &done, pred, &mon_vals...]() {
            if (pred(std::forward_as_tuple(mon_vals...))) {
                done = true;
                m_cv.notify_all();
                set_idle(false);
            }
        };
        if (!pred(std::forward_as_tuple(mon_vals...)))
            wait_internal(done, MonitoredValueCallback(&mon_vals, f)...);
    }

    void wait_for_time(vluint64_t time);

    bool should_exit() { return m_should_exit; }
    bool ready() { return m_ready; }
    bool idle() { return m_idle; }

    // debug only
    std::string m_name;
    void name(std::string n) { m_name = n; }

    std::string name() { return m_name; }
};

class MonitoredValueBase VL_NOT_FINAL {
public:
    virtual std::size_t type_size() const = 0;
    virtual std::size_t size() const = 0;
    virtual vluint8_t* data_u8() = 0;

    void subscribe(MonitoredValueCallback& callback);
    void unsubscribe(MonitoredValueCallback& callback);

    std::mutex& mtx() const { return m_mtx; }

    void written();

protected:
    mutable std::mutex m_mtx;

    std::vector<MonitoredValueCallback*> m_callbacks;
};

class MonitoredValueCallback final {
public:
    template <typename F>
    MonitoredValueCallback(MonitoredValueBase* mv, F func)
        : m_callback(func)
        , m_mon_val(mv) {
        m_mon_val->subscribe(*this);
    }

    MonitoredValueCallback(const MonitoredValueCallback&) = delete;
    MonitoredValueCallback(MonitoredValueCallback&&) = delete;
    MonitoredValueCallback& operator=(const MonitoredValueCallback&) = delete;
    MonitoredValueCallback& operator=(MonitoredValueCallback&&) = delete;
    ~MonitoredValueCallback();
    void operator()();

    MonitoredValueBase* m_mon_val = nullptr;

private:
    std::function<void()> m_callback;
};

template <typename T, typename Enable = void> class MonitoredReference final {
public:
    MonitoredReference(MonitoredValueBase* m, T* p)
        : mon_val(m)
        , ptr(p) {}

    operator T() const { return *ptr; }

    MonitoredReference& operator=(const MonitoredReference& other) { mon_val = other.mon_val; *ptr = *other.ptr; }

    template <class U> MonitoredReference operator=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr = rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator&=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr &= rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator|=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr |= rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator^=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr ^= rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator+=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr += rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator-=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr -= rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator*=(U&& rhs) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr *= rhs;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator>>=(int s) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr >>= s;
        mon_val->written();
        return *this;
    }

    template <class U> MonitoredReference operator--() {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        --*ptr;
        mon_val->written();
        return *this;
    }

    T operator--(int) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        T v = *ptr--;
        mon_val->written();
        return v;
    }

    MonitoredReference operator++() {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        ++*ptr;
        mon_val->written();
        return *this;
    }

    T operator++(int) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        T v = *ptr--;
        mon_val->written();
        return v;
    }

    template <class U> bool operator==(const U& b) { return *ptr == b; }
    template <class U> bool operator>(const U& b) { return *ptr > b; }
    template <class U> bool operator>=(const U& b) { return *ptr >= b; }
    template <class U> bool operator<(const U& b) { return *ptr < b; }
    template <class U> bool operator<=(const U& b) { return *ptr <= b; }

    void assign_no_notify(T v) {
        std::unique_lock<std::mutex> lck(mon_val->mtx());
        *ptr = v;
    }

    void assign_no_lock(T v) {
        *ptr = v;
        mon_val->written();
    }

    MonitoredPointer<T> operator&() { return MonitoredPointer<T>(mon_val, ptr); }

    MonitoredPointer<const T> operator&() const { return MonitoredPointer<T>(mon_val, ptr); }

    std::mutex& mtx() const { return mon_val->mtx(); }

    T* data() const { return ptr; }

    T value() const { return *ptr; }

private:
    MonitoredValueBase* mon_val;
    T* ptr;
};

template <typename T> class MonitoredPointer final {
public:
    template <typename U = T, typename = typename std::enable_if<std::is_const<U>::value>::type>
    MonitoredPointer(const MonitoredPointer<typename std::remove_const<T>::type>& copied)
        : mon_val(copied.monitored_value())
        , ptr(copied.data()) {}
    MonitoredPointer(const MonitoredPointer<T>& copied)
        : mon_val(copied.monitored_value())
        , ptr(copied.data()) {}
    MonitoredPointer(MonitoredValue<T>* m)
        : mon_val(m)
        , ptr(m->data()) {}
    template <typename U>
    explicit MonitoredPointer(const MonitoredPointer<U>& copied)
        : mon_val(copied.monitored_value())
        , ptr((T*)copied.data()) {}
    MonitoredPointer(MonitoredValueBase* m)
        : mon_val(m)
        , ptr((T*)m->data_u8()) {}
    MonitoredPointer(MonitoredValueBase* m, T* p)
        : mon_val(m)
        , ptr(p) {}
    MonitoredPointer(std::nullptr_t)
        : mon_val(nullptr)
        , ptr(nullptr) {}

    template <typename U = T, typename = typename std::enable_if<std::is_const<U>::value>::type>
    MonitoredPointer&
    operator=(const MonitoredPointer<typename std::remove_const<T>::type>& copied) {
        mon_val = copied.monitored_value();
        ptr = copied.data();
        return *this;
    }

    MonitoredReference<T> operator[](int i) { return {mon_val, ptr + i}; }
    MonitoredReference<T> operator[](unsigned int i) { return {mon_val, ptr + i}; }
    MonitoredReference<T> operator[](size_t i) { return {mon_val, ptr + i}; }

    MonitoredReference<const T> operator[](int i) const { return {mon_val, ptr + i}; }
    MonitoredReference<const T> operator[](unsigned int i) const { return {mon_val, ptr + i}; }
    MonitoredReference<const T> operator[](size_t i) const { return {mon_val, ptr + i}; }

    MonitoredReference<T> operator*() { return {mon_val, ptr}; }

    MonitoredValue<T>* operator->() { return (MonitoredValue<T>*)mon_val; }

    MonitoredReference<const T> operator*() const { return {mon_val, ptr}; }

    std::mutex& mtx() const { return mon_val->mtx(); }

    operator bool() const { return mon_val != nullptr; }

    operator void*() const { return (void*) ptr; }
    operator const void*() const { return ptr; }
    operator const T*() const { return ptr; }
    operator MonitoredValueBase*() const { return mon_val; }

    T* data() const { return ptr; }  // XXX fix const correctness

    MonitoredValueBase* monitored_value() const { return mon_val; }  // XXX fix const correctness

    bool operator==(std::nullptr_t) const { return mon_val == nullptr; }

    MonitoredPointer operator+(int i) const { return {mon_val, ptr + i}; }

    MonitoredPointer& operator+=(int i) {
        ptr += i;
        return *this;
    }

    MonitoredPointer operator++(int) {
        MonitoredPointer ptr2(mon_val, ptr);
        ptr++;
        return ptr2;
    }

private:
    mutable MonitoredValueBase* mon_val;
    mutable T* ptr;
};

template <> class MonitoredPointer<void> final {
public:
    template <typename U>
    MonitoredPointer(const MonitoredPointer<U>& copied)
        : mon_val(copied.monitored_value())
        , ptr(copied.data()) {}
    template <typename U>
    explicit MonitoredPointer(MonitoredValue<U>& m)
        : mon_val(&m)
        , ptr(m.data()) {}
    MonitoredPointer(MonitoredValueBase* m)
        : mon_val(m)
        , ptr(m->data_u8()) {}
    template <typename U = void, typename = typename std::enable_if<std::is_array<U>::value>::type>
    MonitoredPointer(MonitoredValueBase& m)
        : mon_val(&m)
        , ptr(m.data_u8()) {}
    MonitoredPointer(MonitoredValueBase* m, void* p)
        : mon_val(m)
        , ptr(p) {}
    MonitoredPointer(std::nullptr_t)
        : mon_val(nullptr)
        , ptr(nullptr) {}

    std::mutex& mtx() const { return mon_val->mtx(); }

    operator bool() const { return mon_val != nullptr; }
    operator void*() { return ptr; }
    operator const void*() const { return ptr; }
    operator MonitoredValueBase*() const { return mon_val; }

    void* data() const { return ptr; }  // XXX fix const correctness

    MonitoredValueBase* monitored_value() const { return mon_val; }  // XXX fix const correctness

    bool operator==(std::nullptr_t) const { return mon_val == nullptr; }

private:
    mutable MonitoredValueBase* mon_val;
    mutable void* ptr;
};

template <> class MonitoredPointer<const void> final {
public:
    template <typename U>
    MonitoredPointer(const MonitoredPointer<U>& copied)
        : mon_val(copied.monitored_value())
        , ptr(copied.data()) {}
    // XXX fix const correctness:
    MonitoredPointer(const MonitoredValueBase* m)
        : mon_val((MonitoredValueBase*)m)
        , ptr(((MonitoredValueBase*)m)->data_u8()) {}
    MonitoredPointer(std::nullptr_t)
        : mon_val(nullptr)
        , ptr(nullptr) {}

    std::mutex& mtx() const { return mon_val->mtx(); }

    operator bool() { return mon_val != nullptr; }
    operator const void*() const { return ptr; }
    operator MonitoredValueBase*() const { return mon_val; }

    const void* data() const { return ptr; }  // XXX fix const correctness

    MonitoredValueBase* monitored_value() const { return mon_val; }  // XXX fix const correctness

    bool operator==(std::nullptr_t) const { return mon_val == nullptr; }

private:
    mutable MonitoredValueBase* mon_val;
    const void* ptr;
};

template <typename T>
class MonitoredReference<T, typename std::enable_if<std::is_array<T>::value>::type> final {
public:
    MonitoredReference(MonitoredValueBase* m, T* p)
        : mon_val(m)
        , ptr(p) {}

    MonitoredReference<typename std::remove_extent<T>::type> operator[](int i) {
        return {mon_val, &(*ptr)[i]};
    }

    MonitoredReference<typename std::remove_extent<T>::type> operator[](unsigned int i) {
        return {mon_val, &(*ptr)[i]};
    }

    operator MonitoredPointer<
        typename std::remove_const<typename std::remove_extent<T>::type>::type>() {
        return {mon_val, *ptr};
    }
    operator MonitoredPointer<
        typename std::add_const<typename std::remove_extent<T>::type>::type>() {
        return {mon_val, *ptr};
    }

    MonitoredPointer<T> operator&() { return MonitoredPointer<T>(mon_val, ptr); }
    MonitoredPointer<const T> operator&() const { return MonitoredPointer<T>(mon_val, ptr); }

    operator typename std::add_const<typename std::remove_extent<T>::type>::type *() const {
        return data();
    }

    typename std::remove_const<typename std::remove_extent<T>::type>::type* data() { return ptr; }
    typename std::add_const<typename std::remove_extent<T>::type>::type* data() const {
        return (typename std::add_const<typename std::remove_extent<T>::type>::type*)ptr;
    }

private:
    mutable MonitoredValueBase* mon_val;
    T* ptr;
};

template <typename T, typename Enable> class MonitoredValue final : public MonitoredValueBase {
public:
    MonitoredValue()
        : m_value() {}

    MonitoredValue(const MonitoredValue& copied)
        : m_value(copied.m_value) {}

    MonitoredValue& operator=(const MonitoredValue& assigned) {
        m_value = assigned.m_value;
        return *this;
    }

    template <class U> MonitoredValue(U v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value = (T)v;
    }

    operator T() const { return m_value; }

    operator MonitoredReference<T>() { return {this, &m_value}; }

    operator MonitoredReference<T>() const { return {this, &m_value}; }

    template <class U> MonitoredValue& operator=(U&& rhs) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value = rhs;
        written();
        return *this;
    }

    MonitoredValue& operator&=(const MonitoredValue& v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value &= (T)v;
        written();
        return *this;
    }
    MonitoredValue& operator|=(const MonitoredValue& v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value |= (T)v;
        written();
        return *this;
    }
    MonitoredValue& operator^=(const MonitoredValue& v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value ^= (T)v;
        written();
        return *this;
    }
    MonitoredValue& operator+=(const MonitoredValue& v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value += (T)v;
        written();
        return *this;
    }
    MonitoredValue& operator-=(const MonitoredValue& v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value -= (T)v;
        written();
        return *this;
    }
    MonitoredValue& operator*=(const MonitoredValue& v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value *= (T)v;
        written();
        return *this;
    }

    MonitoredValue& operator>>=(int s) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value >>= s;
        written();
        return *this;
    }

    MonitoredValue& operator--() {
        std::unique_lock<std::mutex> lck(m_mtx);
        --m_value;
        written();
        return *this;
    }
    MonitoredValue operator--(int) {
        std::unique_lock<std::mutex> lck(m_mtx);
        MonitoredValue v(m_value--);
        written();
        return v;
    }
    MonitoredValue& operator++() {
        std::unique_lock<std::mutex> lck(m_mtx);
        ++m_value;
        written();
        return *this;
    }
    MonitoredValue operator++(int) {
        std::unique_lock<std::mutex> lck(m_mtx);
        MonitoredValue v(m_value++);
        written();
        return v;
    }

    template <class U> bool operator==(const U& b) { return m_value == b; }
    template <class U> bool operator>(const U& b) { return m_value > b; }
    template <class U> bool operator>=(const U& b) { return m_value >= b; }
    template <class U> bool operator<(const U& b) { return m_value < b; }
    template <class U> bool operator<=(const U& b) { return m_value <= b; }

    void assign_no_notify(T v) {
        std::unique_lock<std::mutex> lck(m_mtx);
        m_value = v;
    }

    void assign_no_lock(T v) {
        m_value = v;
        written();
    }

    MonitoredPointer<T> operator&() { return {this, &m_value}; }

    MonitoredPointer<const T> operator&() const { return {(MonitoredValueBase*) this, &m_value}; }

    T* data() { return &m_value; }

    const T* data() const { return &m_value; }

    T value() const { return m_value; }

    virtual std::size_t type_size() const { return sizeof(T); }
    virtual std::size_t size() const { return sizeof(*this); }
    virtual vluint8_t* data_u8() { return (vluint8_t*)&m_value; }

private:
    T m_value;
};

template <typename T>
class MonitoredValue<T, typename std::enable_if<std::is_array<T>::value>::type> final
    : public MonitoredValueBase {
public:
    MonitoredValue() { std::memset(m_value, 0, sizeof(T)); }

    MonitoredValue(const MonitoredValue& copied) {
        std::memcpy(m_value, copied.m_value, sizeof(T));
    }

    MonitoredValue(const MonitoredReference<T>& copied) {
        std::memcpy(m_value, copied.data(), sizeof(T));
    }

    // XXX Use a statically-checked initialization instead of these constructors (so get rid of
    // these initializer_lists)
    MonitoredValue(std::initializer_list<typename std::remove_all_extents<T>::type> initializer) {
        for (size_t i = 0; i < std::min(std::extent<T>::value, initializer.size()); i++) {
            auto it = initializer.begin() + i;
            m_value[i] = *it;
        }
    }

    MonitoredValue(
        std::initializer_list<std::initializer_list<typename std::remove_all_extents<T>::type>>
            initializer) {
        for (size_t i = 0; i < std::min(std::extent<T, 0>::value, initializer.size()); i++) {
            for (size_t j = 0; j < std::min(std::extent<T, 1>::value, initializer.size()); j++) {
                auto it = (initializer.begin() + i)->begin() + j;
                m_value[i][j] = *it;
            }
        }
    }

    MonitoredValue& operator=(const MonitoredValue& assigned) {
        std::memcpy(m_value, assigned.m_value, sizeof(T));
        return *this;
    }

    MonitoredValue& operator=(const MonitoredReference<T>& assigned) {
        std::memcpy(m_value, assigned.ptr, sizeof(T));
        return *this;
    }

    MonitoredReference<typename std::remove_extent<T>::type> operator[](int i) {
        return {this, &m_value[i]};
    }

    MonitoredReference<typename std::remove_extent<T>::type> operator[](unsigned int i) {
        return {this, &m_value[i]};
    }

    MonitoredReference<const typename std::remove_extent<T>::type> operator[](int i) const {
        return {(MonitoredValueBase*)this, &m_value[i]};
    }

    MonitoredReference<const typename std::remove_extent<T>::type>
    operator[](unsigned int i) const {
        return {(MonitoredValueBase*)this, &m_value[i]};
    }

    MonitoredPointer<typename std::remove_extent<T>::type> operator&() { return {this, data()}; }

    MonitoredPointer<const typename std::remove_extent<T>::type> operator&() const { return {(MonitoredValueBase*) this, data()}; }

    operator MonitoredPointer<void>() { return {this, &m_value}; }
    operator MonitoredPointer<typename std::remove_extent<T>::type>() {
        return {this, (typename std::remove_extent<T>::type*)&m_value};
    }

    operator MonitoredPointer<const typename std::remove_extent<T>::type>() const {
        return {(MonitoredValueBase*)this, m_value};
    }

    operator typename std::add_const<typename std::remove_extent<T>::type>::type *() const {
        return m_value;
    }

    typename std::remove_const<typename std::remove_extent<T>::type>::type* data() {
        return m_value;
    }
    typename std::add_const<typename std::remove_extent<T>::type>::type* data() const {
        return m_value;
    }

    virtual std::size_t type_size() const { return sizeof(T); }
    virtual std::size_t size() const { return sizeof(*this); }
    virtual vluint8_t* data_u8() { return (vluint8_t*)m_value; }

private:
    T m_value;
};

template <typename T> class MonitoredValueHash final {
public:
    size_t operator()(const T& t) const { return t.value(); }
};

class Monitor final {
public:
    void off();
    void on();

    template <typename F, typename... Ts> void on(F func, MonitoredValue<Ts>&... mon_vals) {
        off();
        m_func = func;
        m_mon_vals.clear();
        on_internal(mon_vals...);
        on();
    }

private:
    void on_internal() {}

    template <typename T, typename... Ts>
    void on_internal(MonitoredValue<T>& mon_val, MonitoredValue<Ts>&... rest) {
        m_mon_vals.push_back(&mon_val);
        on_internal(rest...);
    }

    std::list<MonitoredValueCallback> m_callbacks;
    std::vector<MonitoredValueBase*> m_mon_vals;
    std::function<void()> m_func;
};

class Strobe final {
public:
    template <typename F> void push(F func) { m_strobes.push_back(func); }

    void display() {
        for (auto& strobe : m_strobes) strobe();
        m_strobes.clear();
    }

private:
    std::vector<std::function<void()>> m_strobes;
};

class VerilatedTimedQueue;
class VerilatedThread;

class VerilatedNBACtrl final {
private:
    // XXX make this whole mechanism faster
    std::vector<std::function<void()>> assignments;
    std::mutex mtx;

public:
    template <typename T, typename U> void schedule(MonitoredReference<T> lhs, U rhs) {
        assignments.push_back([lhs, rhs]() mutable { lhs.assign_no_lock(rhs); });
    }

    template <typename T, typename U>
    void schedule(MonitoredReference<T> lhs, MonitoredReference<U> rhs) {
        U raw_rhs = rhs;
        assignments.push_back([lhs, raw_rhs]() mutable { lhs.assign_no_lock(raw_rhs); });
    }

    template <typename T, typename U>
    void schedule(MonitoredReference<T> lhs, const MonitoredValue<U>& rhs) {
        U raw_rhs = rhs;
        assignments.push_back([lhs, raw_rhs]() mutable { lhs.assign_no_lock(raw_rhs); });
    }

    template <typename T, typename U> void schedule(MonitoredValue<T>& lhs, U rhs) {
        assignments.push_back([&lhs, rhs]() mutable { lhs.assign_no_lock(rhs); });
    }

    template <typename T, typename U>
    void schedule(MonitoredValue<T>& lhs, MonitoredReference<U> rhs) {
        U raw_rhs = rhs;
        assignments.push_back([&lhs, raw_rhs]() mutable { lhs.assign_no_lock(raw_rhs); });
    }

    template <typename T, typename U>
    void schedule(MonitoredValue<T>& lhs, const MonitoredValue<U>& rhs) {
        U raw_rhs = rhs;
        assignments.push_back([&lhs, raw_rhs]() mutable { lhs.assign_no_lock(raw_rhs); });
    }

    void schedule(std::function<void()> expr) {
        std::unique_lock<std::mutex> lck(mtx);
        assignments.push_back(expr);
    }

    void assign() {
        std::unique_lock<std::mutex> lck(mtx);
        for (auto const& assignment : assignments) assignment();
        assignments.clear();
    }
};

//======================================================================
// VerilatedTimedQueue
/// A priority queue of events to activate at a given time
class VerilatedTimedQueue final {
    typedef std::pair<vluint64_t, VerilatedThread*> TimeEvent;  // time, eventp

    struct CustomCompare {
        bool operator()(const TimeEvent& lhs, const TimeEvent& rhs) {
            return lhs.first > rhs.first;
        }
    };

    using TimedQueue = std::priority_queue<int, std::vector<TimeEvent>, CustomCompare>;
    mutable std::mutex m_mutex;  // Mutex protecting m_timeq
    TimedQueue m_timeq;  // Times, ordered by least at top()

    // CONSTRUCTORS
    VL_UNCOPYABLE(VerilatedTimedQueue);

public:
    std::condition_variable m_cv;

    VerilatedTimedQueue() {}
    ~VerilatedTimedQueue() { m_cv.notify_all(); }

    // METHODS
    bool empty() const { return m_timeq.empty(); }
    /// Top/earliest time in the queue; determines when to advance time to
    vluint64_t earliestTime() const VL_EXCLUDES(m_mutex) VL_MT_SAFE {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (VL_UNLIKELY(m_timeq.empty())) return VL_TIME_Q();
        vluint64_t topTime = m_timeq.top().first;
        return topTime;
    }
    /// Push to activate given event at given time
    void push(vluint64_t time, VerilatedThread* thread) VL_EXCLUDES(m_mutex) VL_MT_SAFE {
        // VL_DEBUG_IF(if (VL_UNLIKELY(time < VL_TIME_Q())) Verilated::timeBackwardsError();); XXX
        std::unique_lock<std::mutex> lock(m_mutex);
        m_timeq.push(std::make_pair(time, thread));
    }
    /// Activate and pop all events earlier than given time
    void activate(vluint64_t time) VL_EXCLUDES(m_mutex) VL_MT_SAFE {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (VL_LIKELY(!m_timeq.empty() && m_timeq.top().first <= time)) {
            VerilatedThread* thread = m_timeq.top().second;
            thread->idle(false);
            // VL_DEBUG_IF(VL_DBG_MSGF("+    activate %p\n", thread);); XXX
            m_timeq.pop();
        }
        m_cv.notify_all();
    }
};

struct VerilatedDynamicContext {
    VerilatedDynamicContext(VerilatedContext* contextp)
        : thread_pool(contextp) {}
    VerilatedTimedQueue timed_queue;
    VerilatedThreadPool thread_pool;
    Monitor monitor;
    Strobe strobe;
    std::vector<VerilatedThread*> verilated_threads;

    void timeBackwardsError() VL_MT_SAFE;
    bool timedQEmpty() VL_MT_SAFE;
    vluint64_t timedQEarliestTime() VL_MT_SAFE;
    void timedQPush(vluint64_t time, VerilatedThread* thread) VL_MT_SAFE;
    void timedQActivate(vluint64_t time) VL_MT_SAFE;
    void timedQWait(std::unique_lock<std::mutex>& lck) VL_MT_SAFE;
};

#undef VL_SIG8
#undef VL_SIG16
#undef VL_SIG64
#undef VL_SIG
#undef VL_SIGW
#undef VL_IN8
#undef VL_IN16
#undef VL_IN64
#undef VL_IN
#undef VL_INW
#undef VL_INOUT8
#undef VL_INOUT16
#undef VL_INOUT64
#undef VL_INOUT
#undef VL_INOUTW
#undef VL_OUT8
#undef VL_OUT16
#undef VL_OUT64
#undef VL_OUT
#undef VL_OUTW
#define VL_SIG8(name, msb, lsb) MonitoredValue<CData> name  ///< Declare signal, 1-8 bits
#define VL_SIG16(name, msb, lsb) MonitoredValue<SData> name  ///< Declare signal, 9-16 bits
#define VL_SIG64(name, msb, lsb) MonitoredValue<QData> name  ///< Declare signal, 33-64 bits
#define VL_SIG(name, msb, lsb) MonitoredValue<IData> name  ///< Declare signal, 17-32 bits
#define VL_SIGW(name, msb, lsb, words) \
    MonitoredValue<WData[words]> name  ///< Declare signal, 65+ bits
#define VL_IN8(name, msb, lsb) MonitoredValue<CData> name  ///< Declare input signal, 1-8 bits
#define VL_IN16(name, msb, lsb) MonitoredValue<SData> name  ///< Declare input signal, 9-16 bits
#define VL_IN64(name, msb, lsb) MonitoredValue<QData> name  ///< Declare input signal, 33-64 bits
#define VL_IN(name, msb, lsb) MonitoredValue<IData> name  ///< Declare input signal, 17-32 bits
#define VL_INW(name, msb, lsb, words) \
    MonitoredValue<WData[words]> name  ///< Declare input signal, 65+ bits
#define VL_INOUT8(name, msb, lsb) MonitoredValue<CData> name  ///< Declare bidir signal, 1-8 bits
#define VL_INOUT16(name, msb, lsb) MonitoredValue<SData> name  ///< Declare bidir signal, 9-16 bits
#define VL_INOUT64(name, msb, lsb) \
    MonitoredValue<QData> name  ///< Declare bidir signal, 33-64 bits
#define VL_INOUT(name, msb, lsb) MonitoredValue<IData> name  ///< Declare bidir signal, 17-32 bits
#define VL_INOUTW(name, msb, lsb, words) \
    MonitoredValue<WData[words]> name  ///< Declare bidir signal, 65+ bits
#define VL_OUT8(name, msb, lsb) MonitoredValue<CData> name  ///< Declare output signal, 1-8 bits
#define VL_OUT16(name, msb, lsb) MonitoredValue<SData> name  ///< Declare output signal, 9-16 bits
#define VL_OUT64(name, msb, lsb) MonitoredValue<QData> name  ///< Declare output signal, 33-64bits
#define VL_OUT(name, msb, lsb) MonitoredValue<IData> name  ///< Declare output signal, 17-32 bits
#define VL_OUTW(name, msb, lsb, words) \
    MonitoredValue<WData[words]> name  ///< Declare output signal, 65+ bits

// XXX
// Wide function overloads for dynamic scheduler types.
// Not fully thred-safe, as the mutexes aren't locked for the read-only pointers.
// Also, these functions should probably call notify() on the MonitoredPointers
// Perhaps there's a way to auto-generate all this?
// XXX

#define VL_LOCK_MONITORED_VALUE(value) std::unique_lock<std::mutex> lck_##value(value.mtx())

#ifndef VL_NO_LEGACY
static inline MonitoredPointer<WData> VL_RANDOM_W(int obits, MonitoredPointer<WData> outwp) {
    VL_LOCK_MONITORED_VALUE(outwp);
    VL_RANDOM_W(obits, outwp.data());
    return outwp;
}
#endif

static inline MonitoredPointer<WData> VL_RAND_RESET_W(int obits, MonitoredPointer<WData> outwp) {
    VL_LOCK_MONITORED_VALUE(outwp);
    VL_RAND_RESET_W(obits, outwp.data());
    return outwp;
}

static inline MonitoredPointer<WData> VL_ZERO_RESET_W(int obits, MonitoredPointer<WData> outwp) {
    VL_LOCK_MONITORED_VALUE(outwp);
    VL_ZERO_RESET_W(obits, outwp.data());
    return outwp;
}

static inline MonitoredPointer<WData> _vl_moddiv_w(int lbits, MonitoredPointer<WData> owp,
                                                   WDataInP const lwp, WDataInP const rwp,
                                                   bool is_modulus) {
    VL_LOCK_MONITORED_VALUE(owp);
    _vl_moddiv_w(lbits, owp.data(), lwp, rwp, is_modulus);
    return owp;
}

static inline MonitoredPointer<WData> _vl_clean_inplace_w(int obits,
                                                          MonitoredPointer<WData> owp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    _vl_clean_inplace_w(obits, owp.data());
    return owp;
}

static inline MonitoredPointer<WData> VL_CLEAN_WW(int obits, int, MonitoredPointer<WData> owp,
                                                  WDataInP const lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CLEAN_WW(obits, 0, owp.data(), lwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_ZERO_W(int obits,
                                                MonitoredPointer<WData> owp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ZERO_W(obits, owp.data());
    return owp;
}

static inline MonitoredPointer<WData> VL_ALLONES_W(int obits,
                                                   MonitoredPointer<WData> owp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ALLONES_W(obits, owp.data());
    return owp;
}

static inline MonitoredPointer<WData> VL_ASSIGN_W(int obits, MonitoredPointer<WData> owp,
                                                  WDataInP const lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ASSIGN_W(obits, owp.data(), lwp);
    return owp;
}

static inline void VL_ASSIGNBIT_II(int, int bit, MonitoredReference<CData> lhsr, IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_II(0, bit, *lhsr.data(), rhs);
}

static inline void VL_ASSIGNBIT_II(int, int bit, MonitoredReference<SData> lhsr, IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_II(0, bit, *lhsr.data(), rhs);
}

static inline void VL_ASSIGNBIT_II(int, int bit, MonitoredReference<IData> lhsr, IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_II(0, bit, *lhsr.data(), rhs);
}

static inline void VL_ASSIGNBIT_QI(int, int bit, MonitoredReference<QData> lhsr, IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_QI(0, bit, *lhsr.data(), rhs);
}

static inline void VL_ASSIGNBIT_WI(int, int bit, MonitoredPointer<WData> owp,
                                   IData rhs) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ASSIGNBIT_WI(0, bit, owp.data(), rhs);
}

static inline void VL_ASSIGNBIT_IO(int, int bit, MonitoredReference<CData> lhsr, IData) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_IO(0, bit, *lhsr.data(), 0);
}

static inline void VL_ASSIGNBIT_IO(int, int bit, MonitoredReference<SData> lhsr, IData) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_IO(0, bit, *lhsr.data(), 0);
}

static inline void VL_ASSIGNBIT_IO(int, int bit, MonitoredReference<IData> lhsr, IData) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_IO(0, bit, *lhsr.data(), 0);
}

static inline void VL_ASSIGNBIT_QO(int, int bit, MonitoredReference<QData> lhsr, IData) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNBIT_QO(0, bit, *lhsr.data(), 0);
}

static inline void VL_ASSIGNBIT_WO(int, int bit, MonitoredPointer<WData> owp, IData) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ASSIGNBIT_WO(0, bit, owp.data(), 0);
}

static inline MonitoredPointer<WData> VL_EXTEND_WI(int obits, int, MonitoredPointer<WData> owp,
                                                   IData ld) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_EXTEND_WI(obits, 0, owp.data(), ld);
    return owp;
}

static inline MonitoredPointer<WData> VL_EXTEND_WQ(int obits, int, MonitoredPointer<WData> owp,
                                                   QData ld) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_EXTEND_WQ(obits, 0, owp.data(), ld);
    return owp;
}

static inline MonitoredPointer<WData>
VL_EXTEND_WW(int obits, int lbits, MonitoredPointer<WData> owp, WDataInP const lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_EXTEND_WW(obits, lbits, owp.data(), lwp);
    return owp;
}

static inline MonitoredPointer<WData>
VL_EXTENDS_WI(int obits, int lbits, MonitoredPointer<WData> owp, IData ld) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_EXTENDS_WI(obits, lbits, owp.data(), ld);
    return owp;
}

static inline MonitoredPointer<WData>
VL_EXTENDS_WQ(int obits, int lbits, MonitoredPointer<WData> owp, QData ld) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_EXTENDS_WQ(obits, lbits, owp.data(), ld);
    return owp;
}

static inline MonitoredPointer<WData>
VL_EXTENDS_WW(int obits, int lbits, MonitoredPointer<WData> owp, WDataInP const lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_EXTENDS_WW(obits, lbits, owp.data(), lwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_AND_W(int words, MonitoredPointer<WData> owp,
                                               WDataInP const lwp, WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_AND_W(words, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_OR_W(int words, MonitoredPointer<WData> owp,
                                              WDataInP const lwp, WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_OR_W(words, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_XOR_W(int words, MonitoredPointer<WData> owp,
                                               WDataInP const lwp, WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_XOR_W(words, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_NOT_W(int words, MonitoredPointer<WData> owp,
                                               WDataInP const lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_NOT_W(words, owp.data(), lwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_NEGATE_W(int words, MonitoredPointer<WData> owp,
                                                  WDataInP const lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_NEGATE_W(words, owp.data(), lwp);
    return owp;
}

static inline void VL_NEGATE_INPLACE_W(int words, MonitoredPointer<WData> owp_lwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp_lwp);
    VL_NEGATE_INPLACE_W(words, owp_lwp);
}

static inline MonitoredPointer<WData> VL_ADD_W(int words, MonitoredPointer<WData> owp,
                                               WDataInP const lwp, WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ADD_W(words, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_SUB_W(int words, MonitoredPointer<WData> owp,
                                               WDataInP const lwp, WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SUB_W(words, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_MUL_W(int words, MonitoredPointer<WData> owp,
                                               WDataInP const lwp, WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_MUL_W(words, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_MULS_WWW(int, int lbits, int, MonitoredPointer<WData> owp,
                                                  WDataInP const lwp,
                                                  WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_MULS_WWW(0, lbits, 0, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_DIVS_WWW(int lbits, MonitoredPointer<WData> owp,
                                                  WDataInP const lwp,
                                                  WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_DIVS_WWW(lbits, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_MODDIVS_WWW(int lbits, MonitoredPointer<WData> owp,
                                                     WDataInP const lwp,
                                                     WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_MODDIVS_WWW(lbits, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_POW_WWW(int obits, int, int rbits,
                                                 MonitoredPointer<WData> owp, WDataInP const lwp,
                                                 WDataInP const rwp) {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_POW_WWW(obits, 0, rbits, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData>
VL_POW_WWQ(int obits, int, int rbits, MonitoredPointer<WData> owp, WDataInP const lwp, QData rhs) {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_POW_WWQ(obits, 0, rbits, owp.data(), lwp, rhs);
    return owp;
}

static inline MonitoredPointer<WData> VL_POWSS_WWW(int obits, int, int rbits,
                                                   MonitoredPointer<WData> owp, WDataInP const lwp,
                                                   WDataInP const rwp, bool lsign, bool rsign) {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_POWSS_WWW(obits, 0, rbits, owp.data(), lwp, rwp, lsign, rsign);
    return owp;
}

static inline MonitoredPointer<WData> VL_POWSS_WWQ(int obits, int, int rbits,
                                                   MonitoredPointer<WData> owp, WDataInP const lwp,
                                                   QData rhs, bool lsign, bool rsign) {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_POWSS_WWQ(obits, 0, rbits, owp.data(), lwp, rhs, lsign, rsign);
    return owp;
}

static inline void _vl_insert_WI(int, MonitoredPointer<WData> owp, IData ld, int hbit, int lbit,
                                 int rbits = 0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    _vl_insert_WI(0, owp.data(), ld, hbit, lbit, rbits);
}

static inline void _vl_insert_WW(int, MonitoredPointer<WData> owp, WDataInP const lwp, int hbit,
                                 int lbit, int rbits = 0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    _vl_insert_WW(0, owp.data(), lwp, hbit, lbit, rbits);
}

static inline void _vl_insert_WQ(int obits, MonitoredPointer<WData> owp, QData ld, int hbit,
                                 int lbit, int rbits = 0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    _vl_insert_WQ(obits, owp.data(), ld, hbit, lbit, rbits);
}

static inline MonitoredPointer<WData> VL_REPLICATE_WII(int obits, int lbits, int,
                                                       MonitoredPointer<WData> owp, IData ld,
                                                       IData rep) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_REPLICATE_WII(obits, lbits, 0, owp.data(), ld, rep);
    return owp;
}

static inline MonitoredPointer<WData> VL_REPLICATE_WQI(int obits, int lbits, int,
                                                       MonitoredPointer<WData> owp, QData ld,
                                                       IData rep) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_REPLICATE_WQI(obits, lbits, 0, owp.data(), ld, rep);
    return owp;
}

static inline MonitoredPointer<WData> VL_REPLICATE_WWI(int obits, int lbits, int,
                                                       MonitoredPointer<WData> owp,
                                                       WDataInP const lwp, IData rep) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_REPLICATE_WWI(obits, lbits, 0, owp.data(), lwp, rep);
    return owp;
}

static inline MonitoredPointer<WData> VL_STREAML_WWI(int, int lbits, int,
                                                     MonitoredPointer<WData> owp,
                                                     WDataInP const lwp, IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_STREAML_WWI(0, lbits, 0, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WII(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp, IData ld,
                                                    IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WII(obits, lbits, rbits, owp.data(), ld, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WWI(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp, IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WWI(obits, lbits, rbits, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WIW(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp, IData ld,
                                                    WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WIW(obits, lbits, rbits, owp.data(), ld, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WIQ(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp, IData ld,
                                                    QData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WIQ(obits, lbits, rbits, owp.data(), ld, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WQI(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp, QData ld,
                                                    IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WQI(obits, lbits, rbits, owp.data(), ld, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WQQ(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp, QData ld,
                                                    QData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WQQ(obits, lbits, rbits, owp.data(), ld, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WWQ(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp, QData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WWQ(obits, lbits, rbits, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WQW(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp, QData ld,
                                                    WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WQW(obits, lbits, rbits, owp.data(), ld, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONCAT_WWW(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp,
                                                    WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_CONCAT_WWW(obits, lbits, rbits, owp.data(), lwp, rwp);
    return owp;
}

static inline void _vl_shiftl_inplace_w(int obits, MonitoredPointer<WData> iowp,
                                        IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(iowp);
    _vl_shiftl_inplace_w(obits, iowp.data(), rd);
}

static inline MonitoredPointer<WData> VL_SHIFTL_WWI(int obits, int, int,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp, IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTL_WWI(obits, 0, 0, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTL_WWW(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp,
                                                    WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTL_WWW(obits, lbits, rbits, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTL_WWQ(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp, QData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTL_WWQ(obits, lbits, rbits, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTR_WWI(int obits, int, int,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp, IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTR_WWI(obits, 0, 0, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTR_WWW(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp,
                                                    WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTR_WWW(obits, lbits, rbits, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTR_WWQ(int obits, int lbits, int rbits,
                                                    MonitoredPointer<WData> owp,
                                                    WDataInP const lwp, QData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTR_WWQ(obits, lbits, rbits, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTRS_WWI(int obits, int lbits, int,
                                                     MonitoredPointer<WData> owp,
                                                     WDataInP const lwp, IData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTRS_WWI(obits, lbits, 0, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTRS_WWW(int obits, int lbits, int rbits,
                                                     MonitoredPointer<WData> owp,
                                                     WDataInP const lwp,
                                                     WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTRS_WWW(obits, lbits, rbits, owp.data(), lwp, rwp);
    return owp;
}

static inline MonitoredPointer<WData> VL_SHIFTRS_WWQ(int obits, int lbits, int rbits,
                                                     MonitoredPointer<WData> owp,
                                                     WDataInP const lwp, QData rd) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SHIFTRS_WWQ(obits, lbits, rbits, owp.data(), lwp, rd);
    return owp;
}

static inline MonitoredPointer<WData> VL_SEL_WWII(int obits, int lbits, int, int,
                                                  MonitoredPointer<WData> owp, WDataInP const lwp,
                                                  IData lsb, IData width) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_SEL_WWII(obits, lbits, 0, 0, owp.data(), lwp, lsb, width);
    return owp;
}

static inline MonitoredPointer<WData> VL_RTOIROUND_W_D(int obits, MonitoredPointer<WData> owp,
                                                       double lhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_RTOIROUND_W_D(obits, owp.data(), lhs);
    return owp;
}

static inline void VL_ASSIGNSEL_IIII(int rbits, int obits, int lsb, MonitoredReference<CData> lhsr,
                                     IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNSEL_IIII(rbits, obits, lsb, (CData&)lhsr, rhs);
}

static inline void VL_ASSIGNSEL_IIII(int rbits, int obits, int lsb, MonitoredReference<SData> lhsr,
                                     IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNSEL_IIII(rbits, obits, lsb, (SData&)lhsr, rhs);
}

static inline void VL_ASSIGNSEL_IIII(int rbits, int obits, int lsb, MonitoredReference<IData> lhsr,
                                     IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNSEL_IIII(rbits, obits, lsb, (IData&)lhsr, rhs);
}

static inline void VL_ASSIGNSEL_QIII(int rbits, int obits, int lsb, MonitoredReference<QData> lhsr,
                                     IData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNSEL_QIII(rbits, obits, lsb, (QData&)lhsr, rhs);
}

static inline void VL_ASSIGNSEL_QQII(int rbits, int obits, int lsb, MonitoredReference<QData> lhsr,
                                     QData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNSEL_QQII(rbits, obits, lsb, (QData&)lhsr, rhs);
}

static inline void VL_ASSIGNSEL_QIIQ(int rbits, int obits, int lsb, MonitoredReference<QData> lhsr,
                                     QData rhs) VL_PURE {
    VL_LOCK_MONITORED_VALUE(lhsr);
    VL_ASSIGNSEL_QIIQ(rbits, obits, lsb, (QData&)lhsr, rhs);
}

static inline void VL_ASSIGNSEL_WIII(int rbits, int obits, int lsb, MonitoredPointer<WData> owp,
                                     IData rhs) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ASSIGNSEL_WIII(rbits, obits, lsb, owp.data(), rhs);
}

static inline void VL_ASSIGNSEL_WIIQ(int rbits, int obits, int lsb, MonitoredPointer<WData> owp,
                                     QData rhs) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ASSIGNSEL_WIIQ(rbits, obits, lsb, owp.data(), rhs);
}

static inline void VL_ASSIGNSEL_WIIW(int rbits, int obits, int lsb, MonitoredPointer<WData> owp,
                                     WDataInP const rwp) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_ASSIGNSEL_WIIW(rbits, obits, lsb, owp.data(), rwp);
}

static inline MonitoredPointer<WData> VL_COND_WIWW(int obits, int, int, int,
                                                   MonitoredPointer<WData> owp, int cond,
                                                   WDataInP const w1p,
                                                   WDataInP const w2p) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(owp);
    VL_COND_WIWW(obits, 0, 0, 0, owp.data(), cond, w1p, w2p);
    return owp;
}

static inline MonitoredPointer<WData> VL_CONST_W_1X(int obits, MonitoredPointer<WData> o,
                                                    EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_1X(obits, o.data(), d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_2X(int obits, MonitoredPointer<WData> o, EData d1,
                                                    EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_2X(obits, o.data(), d1, d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_3X(int obits, MonitoredPointer<WData> o, EData d2,
                                                    EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_3X(obits, o.data(), d2, d1, d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_4X(int obits, MonitoredPointer<WData> o, EData d3,
                                                    EData d2, EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_4X(obits, o.data(), d3, d2, d1, d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_5X(int obits, MonitoredPointer<WData> o, EData d4,
                                                    EData d3, EData d2, EData d1,
                                                    EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_5X(obits, o.data(), d4, d3, d2, d1, d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_6X(int obits, MonitoredPointer<WData> o, EData d5,
                                                    EData d4, EData d3, EData d2, EData d1,
                                                    EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_6X(obits, o.data(), d5, d4, d3, d2, d1, d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_7X(int obits, MonitoredPointer<WData> o, EData d6,
                                                    EData d5, EData d4, EData d3, EData d2,
                                                    EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_7X(obits, o.data(), d6, d5, d4, d3, d2, d1, d0);
    return o;
}

static inline MonitoredPointer<WData> VL_CONST_W_8X(int obits, MonitoredPointer<WData> o, EData d7,
                                                    EData d6, EData d5, EData d4, EData d3,
                                                    EData d2, EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(o);
    VL_CONST_W_8X(obits, o.data(), d7, d6, d5, d4, d3, d2, d1, d0);
    return o;
}

static inline MonitoredPointer<WData>
VL_CONSTHI_W_1X(int obits, int lsb, MonitoredPointer<WData> obase, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_1X(obits, lsb, obase.data(), d0);
    return obase;
}

static inline MonitoredPointer<WData>
VL_CONSTHI_W_2X(int obits, int lsb, MonitoredPointer<WData> obase, EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_2X(obits, lsb, obase.data(), d1, d0);
    return obase;
}

static inline MonitoredPointer<WData> VL_CONSTHI_W_3X(int obits, int lsb,
                                                      MonitoredPointer<WData> obase, EData d2,
                                                      EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_3X(obits, lsb, obase.data(), d2, d1, d0);
    return obase;
}

static inline MonitoredPointer<WData> VL_CONSTHI_W_4X(int obits, int lsb,
                                                      MonitoredPointer<WData> obase, EData d3,
                                                      EData d2, EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_4X(obits, lsb, obase.data(), d3, d2, d1, d0);
    return obase;
}

static inline MonitoredPointer<WData> VL_CONSTHI_W_5X(int obits, int lsb,
                                                      MonitoredPointer<WData> obase, EData d4,
                                                      EData d3, EData d2, EData d1,
                                                      EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_5X(obits, lsb, obase.data(), d4, d3, d2, d1, d0);
    return obase;
}

static inline MonitoredPointer<WData> VL_CONSTHI_W_6X(int obits, int lsb,
                                                      MonitoredPointer<WData> obase, EData d5,
                                                      EData d4, EData d3, EData d2, EData d1,
                                                      EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_6X(obits, lsb, obase.data(), d5, d4, d3, d2, d1, d0);
    return obase;
}

static inline MonitoredPointer<WData> VL_CONSTHI_W_7X(int obits, int lsb,
                                                      MonitoredPointer<WData> obase, EData d6,
                                                      EData d5, EData d4, EData d3, EData d2,
                                                      EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_7X(obits, lsb, obase.data(), d6, d5, d4, d3, d2, d1, d0);
    return obase;
}

static inline MonitoredPointer<WData> VL_CONSTHI_W_8X(int obits, int lsb,
                                                      MonitoredPointer<WData> obase, EData d7,
                                                      EData d6, EData d5, EData d4, EData d3,
                                                      EData d2, EData d1, EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTHI_W_8X(obits, lsb, obase.data(), d7, d6, d5, d4, d3, d2, d1, d0);
    return obase;
}

static inline void VL_CONSTLO_W_8X(int lsb, MonitoredPointer<WData> obase, EData d7, EData d6,
                                   EData d5, EData d4, EData d3, EData d2, EData d1,
                                   EData d0) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(obase);
    VL_CONSTLO_W_8X(lsb, obase.data(), d7, d6, d5, d4, d3, d2, d1, d0);
}

template<typename T>
static inline IData VL_FGETS_IXI(int obits, MonitoredPointer<T> destp, IData fpi) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destp);
    return VL_FGETS_IXI(obits, destp.data(), fpi);
}

static inline IData VL_FGETS_IXI(int obits, MonitoredPointer<void> destp, IData fpi) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destp);
    return VL_FGETS_IXI(obits, destp.data(), fpi);
}

template<typename T>
static inline IData VL_FREAD_I(int width, int array_lsb, int array_size, MonitoredPointer<T> memp, IData fpi,
                               IData start, IData count) {
    VL_LOCK_MONITORED_VALUE(memp);
    return VL_FREAD_I(width, array_lsb, array_size, memp.data(), fpi, start, count);
}

static inline IData VL_FREAD_I(int width, int array_lsb, int array_size, MonitoredPointer<void> memp, IData fpi,
                               IData start, IData count) {
    VL_LOCK_MONITORED_VALUE(memp);
    return VL_FREAD_I(width, array_lsb, array_size, memp.data(), fpi, start, count);
}

template<typename T>
void VL_READMEM_N(bool hex, int bits, QData depth, int array_lsb,
                                const std::string& filename, MonitoredPointer<T> memp,
                                QData start, QData end) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(memp);
    VL_READMEM_N(hex, bits, depth, array_lsb, filename, memp.data(), start, end);
}

static inline void VL_READMEM_N(bool hex, int bits, QData depth, int array_lsb,
                                const std::string& filename, MonitoredPointer<void> memp,
                                QData start, QData end) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(memp);
    VL_READMEM_N(hex, bits, depth, array_lsb, filename, memp.data(), start, end);
}

template<typename T>
static inline void VL_WRITEMEM_N(bool hex, int bits, QData depth, int array_lsb,
                                 const std::string& filename, MonitoredPointer<T> memp,
                                 QData start, QData end) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(memp);
    VL_WRITEMEM_N(hex, bits, depth, array_lsb, filename, memp.data(), start, end);
}

static inline void VL_WRITEMEM_N(bool hex, int bits, QData depth, int array_lsb,
                                 const std::string& filename, MonitoredPointer<const void> memp,
                                 QData start, QData end) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(memp);
    VL_WRITEMEM_N(hex, bits, depth, array_lsb, filename, memp.data(), start, end);
}

template <typename T> T _vl_primitive_cast(T arg) { return arg; }

template <typename T>
typename std::enable_if<!std::is_array<T>::value, T>::type
_vl_primitive_cast(const MonitoredValue<T>& arg) {
    return arg.value();
}

template <typename T>
typename std::enable_if<std::is_array<T>::value, typename std::remove_extent<T>::type*>::type
_vl_primitive_cast(const MonitoredValue<T>& arg) {
    return const_cast<typename std::remove_extent<T>::type*>(arg.data());
}

template <typename T>
T* _vl_primitive_cast(const MonitoredReference<T>& arg) {
    return arg.data();
}

template <typename T>
T* _vl_primitive_cast(const MonitoredPointer<T>& arg) {
    return arg.data();
}

template <typename... Ts>
void VL_WRITEF(const char* formatp, Ts&&... args) VL_MT_SAFE {
    _VL_WRITEF(formatp, _vl_primitive_cast(args)...);
}

template <typename... Ts>
void VL_FWRITEF(IData fpi, const char* formatp, Ts&&... args) VL_MT_SAFE { \
    _VL_FWRITEF(fpi, formatp, _vl_primitive_cast(args)...); \
}

template <typename... Ts>
IData VL_FSCANF_IX(IData fpi, const char* formatp, Ts&&... args) VL_MT_SAFE { \
    return _VL_FSCANF_IX(fpi, formatp, _vl_primitive_cast(args)...); \
}

template <typename... Ts>
IData VL_SSCANF_IIX(int lbits, IData ld, const char* formatp, Ts&&... args) VL_MT_SAFE { \
    return _VL_SSCANF_IIX(lbits, ld, formatp, _vl_primitive_cast(args)...); \
}

template <typename... Ts>
IData VL_SSCANF_IQX(int lbits, QData ld, const char* formatp, Ts&&... args) VL_MT_SAFE { \
    return _VL_SSCANF_IQX(lbits, ld, formatp, _vl_primitive_cast(args)...); \
}

template <typename T, typename... Ts>
typename std::enable_if<std::is_array<T>::value, IData>::type VL_SSCANF_IWX(int lbits, const MonitoredValue<T>& lwp, const char* formatp, Ts&&... args) VL_MT_SAFE { \
    VL_LOCK_MONITORED_VALUE(lwp);
    return _VL_SSCANF_IWX(lbits, lwp.data(), formatp, _vl_primitive_cast(args)...); \
}

template <typename... Ts>
void VL_SFORMAT_X(int obits, MonitoredValue<CData>& destr, const char* formatp,
                  Ts&&... args) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destr);
    _VL_SFORMAT_X(obits, *destr.data(), formatp, _vl_primitive_cast(args)...);
}

template <typename... Ts>
void VL_SFORMAT_X(int obits, MonitoredValue<SData>& destr, const char* formatp,
                  Ts&&... args) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destr);
    _VL_SFORMAT_X(obits, *destr.data(), formatp, _vl_primitive_cast(args)...);
}

template <typename... Ts>
void VL_SFORMAT_X(int obits, MonitoredValue<IData>& destr, const char* formatp,
                  Ts&&... args) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destr);
    _VL_SFORMAT_X(obits, *destr.data(), formatp, _vl_primitive_cast(args)...);
}

template <typename... Ts>
void VL_SFORMAT_X(int obits, MonitoredValue<QData>& destr, const char* formatp,
                  Ts&&... args) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destr);
    _VL_SFORMAT_X(obits, *destr.data(), formatp, _vl_primitive_cast(args)...);
}

template <typename T, typename... Ts>
typename std::enable_if<std::is_array<T>::value, void>::type VL_SFORMAT_X(int obits, MonitoredValue<T>& destr, const char* formatp,
                  Ts&&... args) VL_MT_SAFE {
    VL_LOCK_MONITORED_VALUE(destr);
    _VL_SFORMAT_X(obits, *destr.data(), formatp, _vl_primitive_cast(args)...);
}

template <typename... Ts>
IData VL_SSCANF_INX(int lbits, const std::string& ld, const char* formatp, Ts&&... args) VL_MT_SAFE { \
    return _VL_SSCANF_INX(lbits, ld, formatp, _vl_primitive_cast(args)...); \
}

template <typename... Ts>
std::string VL_SFORMATF_NX(const char* formatp, Ts&&... args) VL_MT_SAFE {
    return _VL_SFORMATF_NX(formatp, _vl_primitive_cast(args)...);
}

#endif  // Guard
