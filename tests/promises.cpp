#include <exception>
#include <memory>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <promises/promises.hpp>
#include <promises/schedulers.hpp>

TEST_CASE("promises") {
    using namespace promises;

    auto sch = SingleThreadedScheduler::dflt();
    auto factory = AsyncPromiseFactory(sch);

    SUBCASE("pending -> fulfilled")
    {
        auto p1 = factory.pending<int>();
        p1->subscribe([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 42);
        });
        sch.schedule([&](){ p1->fulfill(42); });
        sch.run();
        // https://github.com/doctest/doctest/discussions/769
    }

    SUBCASE("pending -> locked -> fulfilled")
    {
        auto p1 = factory.pending<int>();
        p1->subscribe([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 42);
        });
        sch.schedule([&](){
            REQUIRE(p1->lock());
            p1->fulfill(42);
        });
        sch.run();
    }

    SUBCASE("immediately fulfilled")
    {
        auto p1 = factory.fulfilled<int>('c');
        p1->subscribe([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 'c');
        });
        sch.run();
    }

    SUBCASE("immediately rejected")
    {
        auto e = std::runtime_error("hello, world!");
        auto p1 = factory.rejected<int>(e);
        p1->subscribe([](auto const& p){
            REQUIRE(p->state() == REJECTED);
            try {
                std::rethrow_exception(p->error());
            } catch (std::exception const& error) {
                CHECK(error.what() == std::string("hello, world!"));
            }
        });
        sch.run();
    }

    SUBCASE("then")
    {
        auto p1 = factory.pending<int>();
        auto p2 = p1->then([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            return p->value() + 1;
        });
        auto p3 = p2->then([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 43);
        });
        sch.schedule([&](){ p1->fulfill(42); });
        sch.run();
    }

    SUBCASE("homogeneous container + cast")
    {
        std::vector<std::shared_ptr<void>> ptrs;
        {
            auto p1 = factory.fulfilled<int>(42);
            ptrs.push_back(p1);
        }
        double d = 3.14;
        {
            auto p2 = factory.fulfilled<double>(d);
            ptrs.push_back(p2);
        }
        auto p3 = factory.cast<int>(ptrs[0]);
        CHECK(p3->value() == 42);
        auto p4 = factory.cast<double>(ptrs[1]);
        CHECK(p4->value() == d);
    }

    SUBCASE("apply")
    {
        auto p1 = factory.pending<int>();
        auto p2 = factory.pending<int>();
        auto p3 = factory.apply([](int a, int b){return a + b; }, p1, p2);
        auto p4 = p3->then([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 3);
        });
        sch.schedule([&](){ p1->fulfill(1); });
        sch.schedule([&](){ p2->fulfill(2); });
        sch.run();
    }

    SUBCASE("subscribe head -> link -> settle tail")
    {
        bool called = false;
        auto head = factory.pending<int>();
        auto tail = factory.pending<int>();
        head->subscribe([&called](auto const& p) {
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 42);
            called = true;
        });
        tail->link(head);
        REQUIRE(head->state() == PENDING);
        sch.schedule([&](){ tail->fulfill(42); });
        sch.run();
        REQUIRE(head->value() == 42);
        REQUIRE(called);
    }

    SUBCASE("link -> subscribe head -> settle tail")
    {
        bool called = false;
        auto head = factory.pending<int>();
        auto tail = factory.pending<int>();
        tail->link(head);
        REQUIRE(head->state() == PENDING);
        head->subscribe([&called](auto const& p) {
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 42);
            called = true;
        });
        sch.schedule([&](){ tail->fulfill(42); });
        sch.run();
        REQUIRE(head->value() == 42);
        REQUIRE(called);
    }

    SUBCASE("subscribe tail -> link -> settle head")
    {
        bool called = false;
        auto head = factory.pending<int>();
        auto tail = factory.pending<int>();
        tail->subscribe([&called](auto const& p) {
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 42);
            called = true;
        });
        tail->link(head);
        REQUIRE(head->state() == PENDING);
        sch.schedule([&](){ head->fulfill(42); });
        sch.run();
        REQUIRE(tail->value() == 42);
        REQUIRE(called);
    }

    SUBCASE("link -> subscribe tail -> settle head")
    {
        bool called = false;
        auto head = factory.pending<int>();
        auto tail = factory.pending<int>();
        tail->link(head);
        REQUIRE(head->state() == PENDING);
        tail->subscribe([&called](auto const& p) {
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 42);
            called = true;
        });
        sch.schedule([&](){ head->fulfill(42); });
        sch.run();
        REQUIRE(tail->value() == 42);
        REQUIRE(called);
    }

    SUBCASE("take non-const reference to value")
    {
        auto p1 = factory.fulfilled<int>(42);
        auto const& p2 = p1;
        auto& i = p2->value();
        ++i;
        CHECK(p1->value() == 43);
    }

    SUBCASE("take non-const reference to error")
    {
        auto e1 = std::runtime_error("hello, world!");
        auto p1 = factory.rejected<int>(e1);
        auto const& p2 = p1;
        auto& e = p2->error();
        e = nullptr;
        CHECK(p1->error() == nullptr);
    }

    SUBCASE("then() a callback that returns a promise")
    {
        auto p1 = factory.pending<int>();
        auto p2 = p1->then([&factory](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            return factory.fulfilled<int>(p->value() + 1);
        });
        auto p3 = p2->then([](auto const& p){
            REQUIRE(p->state() == FULFILLED);
            CHECK(p->value() == 43);
        });
        sch.schedule([&](){ p1->fulfill(42); });
        sch.run();
        CHECK(p2->value() == 43);
    }

    SUBCASE("thenv")
    {
        auto p1 = factory.pending<int>();
        auto p2 = p1->thenv([](auto i){
            return i + 1;
        });
        auto p3 = p2->thenv([](auto i){
            CHECK(i == 43);
        });
        sch.schedule([&](){ p1->fulfill(42); });
        sch.run();
    }

}
