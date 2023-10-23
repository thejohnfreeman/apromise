#include <cstdio>
#include <functional>
#include <vector>

#include <apromise/apromise.hpp>
#include <apromise/schedulers.hpp>

int main(int argc, const char** argv) {
    using namespace apromise;

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

    decltype(auto) sch = SingleThreadedScheduler::dflt();

    {
        auto p1 = sch.pending<int>();
        auto p2 = sch.pending<int>();
        auto p3 = sch.apply([](int a, int b){ return a + b; }, p1, p2);
        auto p4 = p3->then([](auto p){
            std::printf("value == %d\n", p->value());
        });
        sch.schedule([&](){ p1->fulfill(1); });
        sch.run();
        std::printf("not yet...\n");
        sch.schedule([&](){ p2->fulfill(2); });
        sch.run();
    }

    return 0;
}
