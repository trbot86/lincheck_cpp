#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <latch>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <stop_token>
#include <thread>

struct RewriteCompileTarget {
    std::atomic<int> value{0};
    int mirrored = 0;
    std::mutex lock;
    std::recursive_mutex recursive_lock;
    std::timed_mutex timed_lock;
    std::recursive_timed_mutex recursive_timed_lock;
    std::shared_mutex shared_lock;
    std::shared_timed_mutex shared_timed_lock;
    std::condition_variable condition;
    std::barrier<> phase{2};
    std::binary_semaphore gate{0};
    std::counting_semaphore<3> permits{1};
    std::latch done{1};
    bool ready = false;

    int run() {
        std::jthread observer([&](std::stop_token token) {
            if (!token.stop_requested()) {
                std::this_thread::yield();
            }
        });

        std::thread worker([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(0));
            {
                std::lock_guard<std::mutex> guard(lock);
                std::atomic_ref<int> mirrored_ref(mirrored);
                mirrored_ref.fetch_add(1);
                value.fetch_add(1);
                ready = true;
                gate.release();
                done.count_down();
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            condition.notify_one();
            phase.arrive_and_wait();
        });

        {
            std::unique_lock<std::mutex> guard(lock);
            condition.wait(guard, [&] {
                return ready;
            });
        }
        {
            std::lock_guard<std::recursive_mutex> guard(recursive_lock);
            std::lock_guard<std::recursive_mutex> nested_guard(recursive_lock);
        }
        if (timed_lock.try_lock_for(std::chrono::milliseconds(0))) {
            timed_lock.unlock();
        }
        {
            std::unique_lock<std::timed_mutex> guard(timed_lock, std::defer_lock);
            if (guard.try_lock_until(std::chrono::steady_clock::now())) {
                guard.unlock();
            }
        }
        if (recursive_timed_lock.try_lock_for(std::chrono::milliseconds(0))) {
            recursive_timed_lock.unlock();
        }
        {
            std::shared_lock<std::shared_mutex> guard(shared_lock);
        }
        if (shared_lock.try_lock_shared()) {
            shared_lock.unlock_shared();
        }
        {
            std::shared_lock<std::shared_timed_mutex> guard(shared_timed_lock, std::defer_lock);
            if (guard.try_lock_for(std::chrono::milliseconds(0))) {
                guard.unlock();
            }
        }
        if (shared_timed_lock.try_lock_shared_for(std::chrono::milliseconds(0))) {
            shared_timed_lock.unlock_shared();
        }
        gate.acquire();
        permits.acquire();
        permits.release();
        done.wait();
        phase.arrive_and_wait();

        {
            std::scoped_lock<std::mutex> guard(lock);
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }

        worker.join();
        observer.request_stop();
        return value.load() + mirrored;
    }
};

int main() {
    RewriteCompileTarget target;
    return target.run() == 2 ? 0 : 1;
}
