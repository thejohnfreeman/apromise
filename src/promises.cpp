#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

// TODO: Scheduler.
// TODO: Test with single-thread loop scheduler.
// TODO: Non-template base type for heterogeneous containers,
// or can we use shared_ptr<void>?
// TODO: Factor out `settle` method.
// TODO: `then` returns another promise pointer.

enum State { PENDING, SUBSCRIBING, FULFILLED, REJECTED };

template <typename V>
class Promise : public std::enable_shared_from_this<Promise<V>> {
public:
    using value_type = V;
    using error_type = char const*;
    using pointer_type = std::shared_ptr<Promise<V>>;
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

    Storage storage_;
    std::atomic<State> state_;

public:
    Promise() = delete;
    Promise(construct_pending) {}

    template <typename... Args>
    Promise(construct_fulfilled ctor, Args&&... args)
    : state_(FULFILLED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    template <typename... Args>
    Promise(construct_rejected ctor, Args&&... args)
    : state_(REJECTED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    static pointer_type pending() {
        return std::make_shared<Promise<V>>(construct_pending{});
    }

    template <typename... Args>
    static pointer_type fulfilled(Args&&... args) {
        return std::make_shared<Promise<V>>(
                construct_fulfilled{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static pointer_type rejected(Args&&... args) {
        return std::make_shared<Promise<V>>(
                construct_rejected{}, std::forward<Args>(args)...);
    }

    ~Promise()
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

    void then(callback_type&& cb) {
        State expected = PENDING;
        while (!state_.compare_exchange_weak(
                    expected, SUBSCRIBING, std::memory_order_acquire))
        {
            if (expected == SUBSCRIBING) {
                continue;
            }
            if (expected != PENDING) {
                // TODO: Schedule this call.
                cb(this->shared_from_this());
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
        State expected = PENDING;
        while (!state_.compare_exchange_weak(
                    expected, SUBSCRIBING, std::memory_order_acquire))
        {
            expected = PENDING;
        }
        decltype(storage_.callbacks_) callbacks(std::move(storage_.callbacks_));
        std::destroy_at(&storage_.callbacks_);
        std::construct_at(&storage_.value_, std::forward<Args>(args)...);
        state_.store(FULFILLED, std::memory_order_release);
        for (auto& cb : callbacks) {
            cb(this->shared_from_this());
        }
    }

    template <typename... Args>
    void reject(Args&&... args) {
        State expected = PENDING;
        while (!state_.compare_exchange_weak(
                    expected, SUBSCRIBING, std::memory_order_acquire))
        {
            expected = PENDING;
        }
        decltype(storage_.callbacks_) callbacks(std::move(storage_.callbacks_));
        std::destroy_at(&storage_.callbacks_);
        std::construct_at(&storage_.error_, std::forward<Args>(args)...);
        state_.store(REJECTED, std::memory_order_release);
        for (auto& cb : callbacks) {
            cb(this->shared_from_this());
        }
    }
};

int main(int argc, const char** argv) {
    using promise_type = Promise<int>;
    using value_type = promise_type::value_type;
    using error_type = promise_type::error_type;

    std::printf("sizeof(callbacks_) == %lu\n",
            sizeof(std::vector<std::function<void()>>));
    std::printf("sizeof(value) == %lu\n", sizeof(value_type));
    std::printf("sizeof(error) == %lu\n", sizeof(error_type));
    std::printf("sizeof(state) == %lu\n", sizeof(std::atomic<State>));
    std::printf("sizeof(enable_shared_from_this) == %lu\n",
            sizeof(std::enable_shared_from_this<void>));
    std::printf("sizeof(promise) == %lu\n", sizeof(promise_type));

    {
        auto p1 = promise_type::pending();
        p1->then([](auto p){
            if (p->state() == FULFILLED) {
                std::printf("value == %d\n", p->value());
            } else {
                std::printf("expected a value");
            }
        });
        p1->fulfill(42);
    }

    {
        auto p1 = promise_type::fulfilled('c');
        p1->then([](auto p){
            if (p->state() == FULFILLED) {
                std::printf("value == %d\n", p->value());
            } else {
                std::printf("expected a value");
            }
        });
    }

    {
        auto p1 = promise_type::rejected("hello, world!");
        p1->then([](auto p){
            if (p->state() == REJECTED) {
                std::printf("error == %s\n", p->error());
            } else {
                std::printf("expected an error");
            }
        });
    }

    return 0;
}
