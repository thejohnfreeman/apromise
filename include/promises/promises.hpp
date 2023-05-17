#ifndef PROMISES_HPP
#define PROMISES_HPP

#include <atomic>
#include <cassert>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace promises {

template <std::size_t I, typename... Ts>
struct nth_type : public std::tuple_element<I, std::tuple<Ts...>> {};

template <std::size_t I, typename... Ts>
using nth_type_t = typename nth_type<I, Ts...>::type;

enum State {
    // IDLE STATES
    // In an idle state, the promise is waiting to transition to a terminal
    // state. It holds callbacks in its storage.

    // The initial idle state.
    PENDING,
    // A thread has indicated that it will settle the promise.
    // The promise may never transition back to the `PENDING` state.
    LOCKED,

    // LOCKED STATE
    // In the locked state, a thread is writing the storage.
    // No other thread may read or write the storage.
    WRITING,

    // TERMINAL STATES
    // In a terminal state, the promise will never change states again.
    // Its storage will never be written again, except by the destructor.

    // The promise has been linked to another.
    LINKED,
    // The promise has been settled with a value.
    FULFILLED,
    // The promise has been settled with an error.
    REJECTED,
};

class Scheduler {
public:
    using job_type = std::function<void()>;
    virtual void schedule(job_type&& job) = 0;
    virtual ~Scheduler() {}
};

namespace detail {

struct construct_callbacks {};
struct construct_value {};
struct construct_error {};

template <typename T>
struct Value {
    T value_;

    template <typename... Args>
    void construct(Args&&... args) {
        std::construct_at(&value_, std::forward<Args>(args)...);
    }

    T const& get() const {
        return value_;
    }

    void destroy() {
        std::destroy_at(&value_);
    }
};

template <>
struct Value<void> {
    template <typename...>
    void construct() {}
    void get() const {}
    void destroy() {}
};

template <typename T, typename C, typename V>
union Storage {
    using callback_type = C;
    using value_type = V;
    using error_type = std::exception_ptr;

    std::vector<callback_type> callbacks_;
    std::shared_ptr<T> link_;
    Value<value_type> value_;
    error_type error_;

    Storage() : callbacks_{} {}

    template <typename... Args>
    Storage(construct_value, Args&&... args)
    : value_{std::forward<Args>(args)...}
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
        value_.construct(std::forward<Args>(args)...);
    }

    decltype(auto) get_value() const {
        return value_.get();
    }

    void destroy_value() {
        value_.destroy();
    }
};

}

template <typename V>
class AsyncPromise;

/** Shared state collecting arguments for a function application. */
template <typename F, typename... Args>
struct ApplyState
: public std::enable_shared_from_this<ApplyState<F, Args...>>
{
    using R = std::invoke_result_t<F, Args...>;
    using output_type = typename AsyncPromise<R>::pointer_type;

    output_type output_;
    F function_;
    std::tuple<std::shared_ptr<const Args>...> arguments_;
    std::atomic<unsigned int> count_ = 0;
    std::atomic<bool> valid_ = true;

    ApplyState(output_type output, F&& function)
    : output_(std::move(output))
    , function_(std::move(function))
    {}

    template <std::size_t I>
    void addCallback() {}

    template <std::size_t I, typename Arg, typename... Rest>
    void addCallback(
            std::shared_ptr<AsyncPromise<Arg>> arg,
            Rest&&... rest)
    {
        arg->subscribe([self = this->shared_from_this()](auto p) {
            self->template setArgument<I>(p);
        });
        addCallback<I+1>(std::forward<Rest>(rest)...);
    }

    template <typename... Rest>
    void addCallbacks(Rest&&... rest) {
        addCallback<0>(std::forward<Rest>(rest)...);
    }

    template <std::size_t... I>
    R invoke(std::index_sequence<I...>) {
        return std::invoke(
                std::move(function_), std::move(*std::get<I>(arguments_))...);
    }

    R invoke() {
        return invoke(std::make_index_sequence<sizeof...(Args)>());
    }

    template <std::size_t I>
    void setArgument(std::shared_ptr<void> arg) {
        using Arg = nth_type_t<I, Args...>;
        auto p = std::static_pointer_cast<AsyncPromise<Arg>>(std::move(arg));
        auto state = p->state();
        if (state == REJECTED) {
            bool valid = true;
            if (valid_.compare_exchange_strong(valid, false, std::memory_order_relaxed))
            {
                // We are the only writer who invalidated this state.
                output_->reject(p->error());
                output_.reset();
            }
        } else {
            assert(state == FULFILLED);
            std::get<I>(arguments_) = p->value_ptr();
        }
        auto count = 1 + count_.fetch_add(1, std::memory_order_acq_rel);
        if (count != sizeof...(Args)) {
            return;
        }
        // We are the argument writer who wrote the final argument.
        // Every other argument writer has already passed the call to
        // `count_.fetch_add`, and the effect of their write to `valid_`, if
        // any, is visible in this thread because of the acquire-release
        // synchronization on `count_`.
        if (!valid_.load(std::memory_order_relaxed)) {
            return;
        }
        output_->factory_.scheduler().schedule(
        [self = this->shared_from_this()]() {
            try {
                self->output_->fulfill(self->invoke());
            } catch (...) {
                self->output_->reject(std::current_exception());
            }
            // Normally, this lambda would hold the last reference to the
            // shared state, and the state would be destroyed as the lambda
            // exits, but in case someone else is holding a pointer to the
            // state, we'll release its hold on the output, because it can and
            // will no longer modify it.
            self->output_.reset();
        });
    }
};

/**
 * An `AsyncPromiseFactory` is just a decorator around a Scheduler that
 * adds no state, just a set of helper functions.
 */
class AsyncPromiseFactory {
private:
    using job_type = Scheduler::job_type;
    Scheduler& scheduler_;

public:
    AsyncPromiseFactory(Scheduler& scheduler)
    : scheduler_(scheduler)
    {}

    Scheduler& scheduler() const {
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

    template <typename F, typename... Args>
    auto apply(F&& function, std::shared_ptr<AsyncPromise<Args>>... args)
    -> typename AsyncPromise<std::invoke_result_t<F, Args...>>::pointer_type
    {
        using R = std::invoke_result_t<F, Args...>;
        auto output = pending<R>();
        auto state = std::make_shared<ApplyState<F, Args...>>(
                output, std::move(function));
        state->addCallbacks(args...);
        return output;
        // All of the inputs now hold callbacks that hold a shared pointer to
        // the shared state.
        // We will now release our shared pointer to the shared state.
        // The last input that destroys its calllback will destroy the shared
        // state.
    }
};

template <typename V>
class AsyncPromise
: public std::enable_shared_from_this<AsyncPromise<V>>
{
public:
    using pointer_type = std::shared_ptr<AsyncPromise<V>>;
    using callback_type = std::function<void(pointer_type const&)>;
    using storage_type = detail::Storage<AsyncPromise<V>, callback_type, V>;
    using value_type = typename storage_type::value_type;
    using error_type = typename storage_type::error_type;

private:
    AsyncPromiseFactory factory_;
    std::atomic<State> state_ = PENDING;
    storage_type storage_;

public:
    AsyncPromise() = delete;

    AsyncPromise(Scheduler& scheduler, detail::construct_callbacks)
    : factory_(scheduler)
    {}

    template <typename... Args>
    AsyncPromise(Scheduler& scheduler, detail::construct_value ctor, Args&&... args)
    : factory_(scheduler)
    , state_(FULFILLED)
    , storage_(ctor, std::forward<Args>(args)...)
    {}

    template <typename... Args>
    AsyncPromise(Scheduler& scheduler, detail::construct_error ctor, Args&&... args)
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
        else if (status == LINKED) {
            std::destroy_at(&storage_.link_);
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

    bool settled() const {
        auto self = follow();
        auto status = self->state();
        return status == FULFILLED || status == REJECTED;
    }

    bool lock() {
        auto self = this;
        State previous = transition_(self, LOCKED);
        return previous == PENDING;
    }

    void link(pointer_type& rhs) {
        return link(rhs.get());
    }

    /**
     * For this explanation, consider only the observable states
     * (i.e. those returned by `transition_`):
     * `PENDING`, `LOCKED`, `FULFILLED`, and `REJECTED`.
     * One of the two halves of a link _must_ be in the `PENDING` state and
     * _must never_ transition to a non-`PENDING` state.
     * The other observable states (`LOCKED`, `FULFILLED`, and `REJECTED`)
     * indicate that a thread will settle or has settled the promise,
     * and it is impossible to link two promises that are both non-`PENDING`.
     */
    void link(AsyncPromise* rhs) {
        auto lhs = this;
        // Typically, we expect `rhs` to be (a) fresh with no callbacks and
        // (b) the one that will be settled.
        // Thus, we first try to link `rhs` to `lhs`.
        auto rprev = transition_(rhs, WRITING);
        auto lprev = transition_(lhs, WRITING);
        if (rprev != PENDING) {
            // `rhs` is settled or locked.
            // `lhs` must be pending.
            assert(lprev == PENDING);
            std::swap(lhs, rhs);
            std::swap(lprev, rprev);
        }
        // For the rest of the function,
        // we can assume that `rhs` was `PENDING` and is now `WRITING`.
        // We need to reap callbacks from `rhs`, link it to `lhs`,
        // and change its state to `LINKED`.
        // `lhs` may be `WRITING` and its previous state is in `lprev`.
        // If it was not settled (i.e. if it was pending or locked),
        // then its state is now `WRITING`,
        // and we need to add callbacks from `rhs`
        // and restore its previous state.
        // If it was settled,
        // then its state is unchanged,
        // and we need to schedule callbacks from `rhs`,
        // passing to them `lhs`.

        // Reap the callbacks from `rhs`.
        decltype(storage_.callbacks_) callbacks(std::move(rhs->storage_.callbacks_));
        std::destroy_at(&rhs->storage_.callbacks_);
        // Make `rhs` link to `lhs`.
        std::construct_at(&rhs->storage_.link_, lhs->shared_from_this());
        rhs->state_.store(LINKED, std::memory_order_release);

        if (lprev == PENDING || lprev == LOCKED) {
            std::move(callbacks.begin(), callbacks.end(), std::back_inserter(lhs->storage_.callbacks_));
            lhs->state_.store(lprev, std::memory_order_release);
        } else {
            for (auto& cb : callbacks) {
                lhs->factory_.scheduler().schedule(
                    [self = lhs->shared_from_this(), cb = std::move(cb)] () {
                        cb(std::move(self));
                    }
                );
            }
        }
    }

    void subscribe(callback_type&& cb) {
        auto self = this;
        State previous = transition_(self, WRITING);
        if (previous != PENDING && previous != LOCKED) {
            // The promise is settled. No longer taking subscribers.
            self->factory_.scheduler().schedule(
                [self = self->shared_from_this(), cb = std::move(cb)] () {
                    cb(std::move(self));
                }
            );
            return;
        }
        self->storage_.callbacks_.push_back(std::move(cb));
        self->state_.store(previous, std::memory_order_release);
    }

    template <typename F>
    auto then(F&& f)
    -> typename AsyncPromise<std::invoke_result_t<F, pointer_type>>::pointer_type {
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

    decltype(auto) get() const {
        auto self = follow();
        auto status = self->state();
        if (status == REJECTED) {
            std::rethrow_exception(self->error_());
        }
        if (status != FULFILLED) {
            throw std::runtime_error("promise not settled");
        }
        return self->value_();
    }

    decltype(auto) value() const {
        auto self = follow();
        assert(self->state() == FULFILLED);
        return self->value_();
    }

    auto value_ptr() const {
        auto self = follow();
        return std::shared_ptr<const value_type>(
                self->shared_from_this(), &self->value_());
    }

    error_type const& error() const {
        auto self = follow();
        assert(self->state() == REJECTED);
        return self->error_();
    }

    template <typename... Args>
    bool fulfill(Args&&... args) {
        return settle(
                FULFILLED,
                &storage_type::template construct_value<Args...>,
                std::forward<Args>(args)...);
    }

    template <typename E>
    bool reject(E&& error) {
        return reject(std::make_exception_ptr(std::move(error)));
    }

    bool reject(std::exception_ptr error) {
        return settle(
                REJECTED,
                &storage_type::template construct_error<std::exception_ptr>,
                std::move(error));
    }

private:
    template <typename W>
    friend class AsyncPromise;
    template <typename F, typename... Args>
    friend struct ApplyState;

    AsyncPromise const* follow() const {
        auto p = this;
        while (p->state() == LINKED) {
            p = p->storage_.link_.get();
        }
        return p;
    }

    AsyncPromise* follow() {
        auto p = this;
        while (p->state() == LINKED) {
            p = p->storage_.link_.get();
        }
        return p;
    }

    decltype(auto) value_() const {
        return storage_.get_value();
    }

    error_type const& error_() const {
        return storage_.error_;
    }

    /**
     * @return `PENDING` or `LOCKED` if the transition succeeded.
     * Otherwise, whatever state prevented the transition
     * from ever succeeding, one of `FULFILLED` or `REJECTED`
     * (or `LOCKED` only if `desired == LOCKED`).
     * Will never be `WRITING` or `LINKED`.
     */
    friend
    State transition_(AsyncPromise*& self, State desired) {
        State idle = PENDING;
        State expected = idle;
        while (!self->state_.compare_exchange_weak(
                    expected, desired, std::memory_order_acquire))
        {
            if (expected == WRITING) {
                // Another thread is writing. Try again.
                expected = idle;
                continue;
            }
            if (expected == LINKED) {
                // Follow the link and try again.
                self = self->storage_.link_.get();
                expected = idle;
                continue;
            }
            if (expected == LOCKED && desired != LOCKED) {
                // `LOCKED` is the idle state,
                // and this should be the thread that put it there.
                idle = LOCKED;
                // One more time through the loop.
                continue;
            }
            break;
        }
        return expected;
    }

    template <typename M, typename... Args>
    bool settle(State status, M method, Args&&... args) {
        auto self = this;
        // We cannot transition directly to `status`
        // because we do not want any threads reading the value or error
        // until it is constructed.
        State previous = transition_(self, WRITING);
        if (previous != PENDING && previous != LOCKED) {
            // This should be unreachable.
            // Only one thread should ever call `settle`.
            return false;
        }
        decltype(storage_.callbacks_) callbacks(std::move(self->storage_.callbacks_));
        std::destroy_at(&self->storage_.callbacks_);
        (self->storage_.*method)(std::forward<Args>(args)...);
        self->state_.store(status, std::memory_order_release);
        for (auto& cb : callbacks) {
            self->factory_.scheduler().schedule(
                [self = self->shared_from_this(), cb = std::move(cb)] ()
                { cb(std::move(self)); }
            );
        }
        return true;
    }

    template <typename F, typename... Args>
    std::enable_if_t<!std::is_same_v<
        std::invoke_result_t<F, Args...>,
        void
    >, bool>
    fulfillWith(F&& f, Args&&... args) {
        return fulfill(f(std::move(args)...));
    }

    template <typename F, typename... Args>
    std::enable_if_t<std::is_same_v<
        std::invoke_result_t<F, Args...>,
        void
    >, bool>
    fulfillWith(F&& f, Args&&... args) {
        f(std::move(args)...);
        return fulfill();
    }
};

}

#endif
