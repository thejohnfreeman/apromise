#ifndef APROMISE_SCHEDULERS_HPP
#define APROMISE_SCHEDULERS_HPP

#include <apromise/apromise.hpp>

#include <list>

namespace apromise {

class SingleThreadedScheduler : public Scheduler {
private:
    std::list<job_type> jobs_;
public:
    static SingleThreadedScheduler& dflt() {
        thread_local SingleThreadedScheduler scheduler;
        return scheduler;
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

}

#endif
