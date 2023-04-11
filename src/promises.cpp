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

enum State { PENDING, SUBSCRIBING, VALUE, ERROR };

template <typename V>
class Promise : public std::enable_shared_from_this<Promise<V>> {
public:
    using value_type = V;
    using error_type = char const*;
    using pointer_type = std::shared_ptr<Promise<V>>;
    using callback_type = std::function<void(pointer_type)>;

private:
    union Storage
    {
        std::vector<callback_type> callbacks_;
        value_type value_;
        error_type error_;

        Storage() : callbacks_{} {}
        ~Storage() {}
    };

    Storage storage_;
    std::atomic<State> state_;

    // This type blocks everyone from directly constructing a promise.
    struct blocker {};

public:
    Promise() = delete;
    Promise(blocker) {}

    static pointer_type make() {
        return std::make_shared<Promise<V>>(blocker{});
    }

    ~Promise()
    {
        auto status = state();
        if (status == PENDING)
        {
            std::destroy_at(&storage_.callbacks_);
        }
        else if (status == VALUE)
        {
            std::destroy_at(&storage_.value_);
        }
        else
        {
            assert(status == ERROR);
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
                // TODO: Schedule immediately.
                return;
            }
        }
        assert(expected == PENDING);
        storage_.callbacks_.push_back(std::move(cb));
        state_.store(PENDING, std::memory_order_release);
    }

    value_type const& value() const {
        assert(state() == VALUE);
        return storage_.value_;
    }

    error_type const& error() const {
        assert(state() == ERROR);
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
        state_.store(VALUE, std::memory_order_release);
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
        state_.store(ERROR, std::memory_order_release);
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

    auto promise = promise_type::make();
    promise->then([](auto p){
        if (p->state() == VALUE) {
            std::printf("value == %d\n", p->value());
        } else {
            std::printf("expected a value");
        }
    });
    promise->fulfill(42);

    return 0;
}
