#include "band_pool.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_ordering_and_reuse(int threads) {
    dsr::BandPool pool(threads);
    const auto caller = std::this_thread::get_id();
    std::vector<int> order;
    std::thread::id first_worker;
    for (int pass = 0; pass < 10; ++pass) {
        order.clear();
        pool.run(8, [&](int band) {
            order.push_back(band);
            if (band == 0) {
                if (pass == 0) first_worker = std::this_thread::get_id();
                require(first_worker == std::this_thread::get_id(), "worker reused");
            }
            require((std::this_thread::get_id() == caller) == (threads == 0), "inline caller affinity");
        });
        require(order == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7}), "single-worker ascending order");
    }
    pool.run(0, {});
}

void test_barrier() {
    dsr::BandPool pool(2);
    std::promise<void> entered;
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::atomic<bool> returned{false};
    std::array<int, 8> results{};
    std::thread caller([&] {
        pool.run(8, [&](int band) {
            if (band == 0) {
                entered.set_value();
                release_future.wait();
            }
            results[static_cast<std::size_t>(band)] = band + 1;
        });
        returned = true;
    });
    entered.get_future().wait();
    const bool blocked = !returned.load();
    release.set_value();
    caller.join();
    require(blocked, "run waits for blocked band");
    require(returned, "run returned after barrier");
    for (int band = 0; band < 8; ++band)
        require(results[static_cast<std::size_t>(band)] == band + 1, "barrier publishes all writes");
}

void test_exceptions(int threads) {
    dsr::BandPool pool(threads);
    std::atomic<int> completed{0};
    bool caught = false;
    try {
        pool.run(24, [&](int band) {
            ++completed;
            if (band == 3) throw std::runtime_error("band failure");
        });
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "band failure";
    }
    require(caught && completed == 24, "exception propagated after all bands finish");
    pool.run(8, [&](int) { ++completed; });
    require(completed == 32, "pool reusable after exception");
    try { pool.run(-1, [](int) {}); require(false, "negative count rejected"); }
    catch (const std::invalid_argument&) {}
    try { pool.run(1, {}); require(false, "empty callback rejected"); }
    catch (const std::invalid_argument&) {}
}

void test_stress(int threads) {
    dsr::BandPool pool(threads);
    std::array<std::atomic<int>, 37> visits{};
    for (int pass = 0; pass < 2000; ++pass) {
        const int bands = pass % 38;
        pool.run(bands, [&](int band) {
            ++visits[static_cast<std::size_t>(band)];
            if ((band & 3) == 0) std::this_thread::yield();
        });
    }
    for (int band = 0; band < 37; ++band) {
        int expected = 0;
        for (int pass = 0; pass < 2000; ++pass) expected += pass % 38 > band;
        require(visits[static_cast<std::size_t>(band)] == expected, "stress exact-once band dispatch");
    }
}

void test_concurrent_callers() {
    dsr::BandPool pool(6);
    std::array<int, 64> a{}, b{};
    auto submit = [&](std::array<int, 64>& result) {
        for (int pass = 0; pass < 100; ++pass)
            pool.run(64, [&](int band) { ++result[static_cast<std::size_t>(band)]; });
    };
    std::thread first([&] { submit(a); });
    std::thread second([&] { submit(b); });
    first.join();
    second.join();
    for (int i = 0; i < 64; ++i)
        require(a[static_cast<std::size_t>(i)] == 100 && b[static_cast<std::size_t>(i)] == 100,
                "concurrent submissions serialized without losing jobs");
}

void test_explicit_shutdown(int threads) {
    dsr::BandPool pool(threads);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::atomic<bool> completed{false};
    bool finished_before_join = false;
    std::thread caller([&] {
        pool.run(1, [&](int) {
            entered.set_value();
            released.wait();
            completed = true;
        });
    });
    entered.get_future().wait();
    std::thread shutdown([&] {
        pool.shutdown();
        finished_before_join = completed.load();
    });
    release.set_value();
    caller.join();
    shutdown.join();
    require(finished_before_join, "shutdown waits for the active job");
    pool.shutdown();
    bool rejected = false;
    try { pool.run(1, [](int) {}); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "terminal shutdown rejects new render jobs");
}

void set_threads_env(const char* value) {
#ifdef _WIN32
    _putenv_s("DSR_RENDER_THREADS", value == nullptr ? "" : value);
#else
    if (value == nullptr) unsetenv("DSR_RENDER_THREADS");
    else setenv("DSR_RENDER_THREADS", value, 1);
#endif
}

void test_environment() {
    const char* old = std::getenv("DSR_RENDER_THREADS");
    const bool had_old = old != nullptr;
    const std::string saved = had_old ? old : "";
    for (const auto* value : {"0", "1", "2", "6", "99"}) {
        set_threads_env(value);
        dsr::BandPool pool;
        require(pool.worker_count() == std::min(std::atoi(value), 6), "environment override");
        pool.run(8, [](int) {});
    }
    set_threads_env(nullptr);
    const int automatic = dsr::BandPool{}.worker_count();
    require(automatic >= 1 && automatic <= 6, "automatic worker clamp");
    for (const auto* value : {"", "-1", "no", "2oops"}) {
        set_threads_env(value);
        require(dsr::BandPool{}.worker_count() == automatic, "malformed environment uses default");
    }
    set_threads_env(had_old ? saved.c_str() : nullptr);
}
} // namespace

int main() {
    try {
        test_environment();
        test_ordering_and_reuse(0);
        test_ordering_and_reuse(1);
        test_barrier();
        for (int threads : {0, 1, 2, 6}) {
            test_exceptions(threads);
            test_stress(threads);
            test_explicit_shutdown(threads);
        }
        test_concurrent_callers();
        std::cout << "band pool tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
