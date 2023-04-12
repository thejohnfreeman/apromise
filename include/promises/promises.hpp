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

// TODO: Non-template base type for heterogeneous containers,
// or can we use shared_ptr<void>?
// TODO: `then` returns another promise pointer.

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

template <typename V>
class PROMISES_EXPORT AsyncPromise
: public std::enable_shared_from_this<AsyncPromise<V>>
{
public:
    using value_type = V;
    using error_type = std::exception_ptr;
    using pointer_type = std::shared_ptr<AsyncPromise<V>>;
    using callback_type = std::function<void(pointer_type)>;

private:
    struct construct_pending {};
    struct construct_fulfilled {};
    struct construct_rejected {};

    union Storage
    {
        std::vector<callback_type> callbacks_;
        value_type value_;
        error_type error_;

        Storage() : callbacks_{} {}

        template <typename... Args>
        Storage(construct_fulfilled, Args&&... args)
        : value_(std::forward<Args>(args)...)
        {}

        template <typename... Args>
        Storage(construct_rejected, Args&&... args)
        : error_(std::forward<Args>(args)...)
        {}

        ~Storage() {}
    };

    Scheduler* scheduler_ = SingleThreadedScheduler::dflt();
    std::atomic<State> state_;
    Storage storage_;

public:
    AsyncPromise() = delete;
    AsyncPromise(construct_pending) {}

    template <typename... Args>
    AsyncPromise(construct_fulfilled ctor, Args&&... args)
    : state_(FULFILLED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    template <typename... Args>
    AsyncPromise(construct_rejected ctor, Args&&... args)
    : state_(REJECTED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    static pointer_type pending() {
        return std::make_shared<AsyncPromise<V>>(construct_pending{});
    }

    template <typename... Args>
    static pointer_type fulfilled(Args&&... args) {
        return std::make_shared<AsyncPromise<V>>(
                construct_fulfilled{}, std::forward<Args>(args)...);
    }

    template <typename E>
    static pointer_type rejected(E const& error) {
        return std::make_shared<AsyncPromise<V>>(
                construct_rejected{}, std::make_exception_ptr(error));
    }

    ~AsyncPromise()
    {
        auto status = state();
        if (status == PENDING)
        {
            std::destroy_at(&storage_.callbacks_);
        }
        else if (status == FULFILLED)
        {
            std::destroy_at(&storage_.value_);
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
                scheduler_->schedule(
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

    value_type const& value() const {
        assert(state() == FULFILLED);
        return storage_.value_;
    }

    error_type const& error() const {
        assert(state() == REJECTED);
        return storage_.error_;
    }

    template <typename... Args>
    void fulfill(Args&&... args) {
        return settle(FULFILLED, &Storage::value_, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void reject(Args&&... args) {
        return settle(REJECTED, &Storage::error_, std::forward<Args>(args)...);
    }

private:
    template <typename T, typename... Args>
    void settle(State status, T Storage::*member, Args&&... args) {
        State expected = PENDING;
        while (!state_.compare_exchange_weak(
                    expected, SUBSCRIBING, std::memory_order_acquire))
        {
            expected = PENDING;
        }
        decltype(storage_.callbacks_) callbacks(std::move(storage_.callbacks_));
        std::destroy_at(&storage_.callbacks_);
        std::construct_at(&(storage_.*member), std::forward<Args>(args)...);
        state_.store(status, std::memory_order_release);
        for (auto& cb : callbacks) {
            scheduler_->schedule(
                [self = this->shared_from_this(), cb = std::move(cb)] ()
                { cb(std::move(self)); }
            );
        }
    }

};

}

#endif
