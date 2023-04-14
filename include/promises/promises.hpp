#ifndef PROMISES_HPP
#define PROMISES_HPP

#include <promises/export.hpp>

#include <atomic>
#include <cassert>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <vector>

namespace promises {

enum State { PENDING, SUBSCRIBING, FULFILLED, REJECTED };

class Scheduler {
public:
    using job_type = std::function<void()>;
    virtual void schedule(job_type&& job) = 0;
    virtual ~Scheduler() {}
};

class SingleThreadedScheduler : public Scheduler {
private:
    std::list<job_type> jobs_;
public:
    static SingleThreadedScheduler* dflt() {
        thread_local SingleThreadedScheduler scheduler;
        return &scheduler;
    }

    void run() {
        while (!jobs_.empty()) {
            auto& job = jobs_.front();
            job();
            jobs_.pop_front();
        }
    }

    virtual void schedule(job_type&& job) override {
        jobs_.push_back(std::move(job));
    }

    virtual ~SingleThreadedScheduler() {
    }
};

namespace detail {

struct construct_callbacks {};
struct construct_value {};
struct construct_error {};

template <typename C, typename V>
union Storage {
    using callback_type = C;
    using value_type = V;
    using error_type = std::exception_ptr;

    std::vector<callback_type> callbacks_;
    value_type value_;
    error_type error_;

    Storage() : callbacks_{} {}

    template <typename... Args>
    Storage(construct_value, Args&&... args)
    : value_(std::forward<Args>(args)...)
    {}

    // This form was written before landing on the chosen `error_type`.
    template <typename... Args>
    Storage(construct_error, Args&&... args)
    : error_(std::forward<Args>(args)...)
    {}

    ~Storage() {}

    template <typename... Args>
    void construct_error(Args&&... args) {
        std::construct_at(&error_, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void construct_value(Args&&... args) {
        std::construct_at(&value_, std::forward<Args>(args)...);
    }

    value_type const& get_value() const {
        return value_;
    }

    void destroy_value() {
        std::destroy_at(&value_);
    }
};

template <typename C>
union Storage<C, void> {
    using callback_type = C;
    using value_type = void;
    using error_type = std::exception_ptr;

    std::vector<callback_type> callbacks_;
    error_type error_;

    Storage() : callbacks_{} {}

    Storage(construct_value) {}

    template <typename... Args>
    Storage(construct_error, Args&&... args)
    : error_(std::forward<Args>(args)...)
    {}

    ~Storage() {}

    template <typename... Args>
    void construct_error(Args&&... args) {
        std::construct_at(&error_, std::forward<Args>(args)...);
    }

    template <typename...>
    void construct_value() {}
    void get_value() const {}
    void destroy_value() {}
};

}

template <typename V>
class AsyncPromise;

/**
 * An `AsyncPromiseFactory` is just a decorator around a Scheduler that
 * adds no state, just a set of helper functions.
 */
class PROMISES_EXPORT AsyncPromiseFactory {
private:
    using job_type = Scheduler::job_type;
    Scheduler* scheduler_;

public:
    AsyncPromiseFactory(Scheduler* scheduler)
    : scheduler_(scheduler)
    {}

    Scheduler* scheduler() const {
        return scheduler_;
    }

    template <typename V>
    auto pending() {
        return std::make_shared<AsyncPromise<V>>(
                scheduler_, detail::construct_callbacks{});
    }

    template <typename V, typename... Args>
    auto fulfilled(Args&&... args) {
        return std::make_shared<AsyncPromise<V>>(
                scheduler_, detail::construct_value{}, std::forward<Args>(args)...);
    }

    template <typename V, typename E>
    auto rejected(E const& error) {
        return std::make_shared<AsyncPromise<V>>(
                scheduler_, detail::construct_error{}, std::make_exception_ptr(error));
    }

    template <typename V>
    auto cast(std::shared_ptr<void> p) {
            return std::static_pointer_cast<AsyncPromise<V>>(p);
    }
};

template <typename V>
class PROMISES_EXPORT AsyncPromise
: public std::enable_shared_from_this<AsyncPromise<V>>
{
public:
    using pointer_type = std::shared_ptr<AsyncPromise<V>>;
    using callback_type = std::function<void(pointer_type)>;
    using storage_type = detail::Storage<callback_type, V>;
    using value_type = typename storage_type::value_type;
    using error_type = typename storage_type::error_type;

private:
    AsyncPromiseFactory factory_;
    std::atomic<State> state_ = PENDING;
    storage_type storage_;

public:
    AsyncPromise() = delete;

    AsyncPromise(Scheduler* scheduler, detail::construct_callbacks)
    : factory_(scheduler)
    {}

    template <typename... Args>
    AsyncPromise(Scheduler* scheduler, detail::construct_value ctor, Args&&... args)
    : factory_(scheduler)
    , state_(FULFILLED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    template <typename... Args>
    AsyncPromise(Scheduler* scheduler, detail::construct_error ctor, Args&&... args)
    : factory_(scheduler)
    , state_(REJECTED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    ~AsyncPromise()
    {
        auto status = state();
        if (status == PENDING)
        {
            std::destroy_at(&storage_.callbacks_);
        }
        else if (status == FULFILLED)
        {
            storage_.destroy_value();
        }
        else
        {
            assert(status == REJECTED);
            std::destroy_at(&storage_.error_);
        }
    }

    State state() const {
        return state_.load(std::memory_order_acquire);
    }

    void subscribe(callback_type&& cb) {
        State expected = PENDING;
        while (!state_.compare_exchange_weak(
                    expected, SUBSCRIBING, std::memory_order_acquire))
        {
            if (expected == SUBSCRIBING) {
                continue;
            }
            if (expected != PENDING) {
                factory_.scheduler()->schedule(
                    [self = this->shared_from_this(), cb = std::move(cb)] ()
                    { cb(std::move(self)); }
                );
                return;
            }
        }
        assert(expected == PENDING);
        storage_.callbacks_.push_back(std::move(cb));
        state_.store(PENDING, std::memory_order_release);
    }

    template <typename F>
    auto then(F&& f) -> typename AsyncPromise<std::invoke_result_t<F, AsyncPromise::pointer_type>>::pointer_type {
        using R = std::invoke_result_t<F, AsyncPromise::pointer_type>;
        auto q = factory_.pending<R>();
        auto cb = [q, f = std::move(f)](pointer_type p) {
            try {
                q->fulfillWith(std::move(f), std::move(p));
            } catch (...) {
                q->reject(std::current_exception());
            }
        };
        subscribe(cb);
        return q;
    }

    auto value() const {
        assert(state() == FULFILLED);
        return storage_.get_value();
    }

    error_type const& error() const {
        assert(state() == REJECTED);
        return storage_.error_;
    }

    template <typename... Args>
    void fulfill(Args&&... args) {
        return settle(
                FULFILLED,
                &storage_type::template construct_value<Args...>,
                std::forward<Args>(args)...);
    }

    template <typename E>
    void reject(E&& error) {
        return reject(std::make_exception_ptr(std::move(error)));
    }

    void reject(std::exception_ptr error) {
        return settle(
                REJECTED,
                &storage_type::template construct_error<std::exception_ptr>,
                std::move(error));
    }

private:
    template <typename W>
    friend class AsyncPromise;

    template <typename M, typename... Args>
    void settle(State status, M method, Args&&... args) {
        State expected = PENDING;
        while (!state_.compare_exchange_weak(
                    expected, SUBSCRIBING, std::memory_order_acquire))
        {
            expected = PENDING;
        }
        decltype(storage_.callbacks_) callbacks(std::move(storage_.callbacks_));
        std::destroy_at(&storage_.callbacks_);
        (storage_.*method)(std::forward<Args>(args)...);
        state_.store(status, std::memory_order_release);
        for (auto& cb : callbacks) {
            factory_.scheduler()->schedule(
                [self = this->shared_from_this(), cb = std::move(cb)] ()
                { cb(std::move(self)); }
            );
        }
    }

    template <typename F, typename... Args>
    std::enable_if_t<!std::is_same_v<
        std::invoke_result_t<F, Args...>,
        void
    >>
    fulfillWith(F&& f, Args&&... args) {
        fulfill(f(std::move(args)...));
    }

    template <typename F, typename... Args>
    std::enable_if_t<std::is_same_v<
        std::invoke_result_t<F, Args...>,
        void
    >>
    fulfillWith(F&& f, Args&&... args) {
        f(std::move(args)...);
        fulfill();
    }
};

}

#endif
