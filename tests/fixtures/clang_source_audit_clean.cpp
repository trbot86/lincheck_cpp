#include <lincheck/lincheck.hpp>

struct CleanClangAuditTarget {
    lincheck::atomic<int> value{0};
    lincheck::mutex lock;

    void run() {
        lincheck::thread worker([&] {
            value.fetch_add(1);
        });
        worker.join();
    }
};
