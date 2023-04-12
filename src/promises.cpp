#include <exception>
#include <promises/promises.hpp>

#include <cstdio>
#include <vector>

int main(int argc, const char** argv) {
    using namespace promises;

    using promise_type = AsyncPromise<int>;
    using value_type = promise_type::value_type;
    using error_type = promise_type::error_type;

    std::printf("sizeof(callback) == %lu\n",
            sizeof(promise_type::callback_type));
    std::printf("sizeof(callbacks_) == %lu\n",
            sizeof(std::vector<std::function<void()>>));
    std::printf("sizeof(value_) == %lu\n", sizeof(value_type));
    std::printf("sizeof(error_) == %lu\n", sizeof(error_type));
    std::printf("sizeof(state_) == %lu\n", sizeof(std::atomic<State>));
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
        auto sch = SingleThreadedScheduler::dflt();
        sch->schedule([&](){ p1->fulfill(42); });
        sch->run();
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
        auto sch = SingleThreadedScheduler::dflt();
        sch->run();
    }

    {
        auto e = std::runtime_error("hello, world!");
        std::printf("error == %s\n", e.what());
        auto p1 = promise_type::rejected(e);
        p1->then([](auto p){
            if (p->state() == REJECTED) {
                try {
                    std::rethrow_exception(p->error());
                } catch (std::exception const& error) {
                    std::printf("error == %s\n", error.what());
                }
            } else {
                std::printf("expected an error");
            }
        });
        auto sch = SingleThreadedScheduler::dflt();
        sch->run();
    }

    return 0;
}
