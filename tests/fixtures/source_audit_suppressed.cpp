#include <atomic>
#include <mutex>

struct SuppressedAuditTarget {
    std::atomic<int> value{0}; // NOLINT(lincheck-raw-sync)

    // NOLINTNEXTLINE(lincheck-raw-sync)
    std::mutex lock;

    const char* text = "std::thread in a string literal is ignored";
    // std::condition_variable in a comment is ignored
};
