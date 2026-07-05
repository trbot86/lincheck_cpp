#include <atomic>
#include <mutex>
#include <thread>

struct BadAuditTarget {
    std::atomic<int> value{0};
    std::mutex lock;

    void run() {
        std::thread worker([&] {
            value.fetch_add(1);
        });
        worker.join();
    }
};
