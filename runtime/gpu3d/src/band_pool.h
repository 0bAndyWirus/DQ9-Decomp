#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace dsr {

// A blocking, reusable renderer pool. Concurrent callers are serialized. A job
// must not call run() on this same pool; destruction requires no active callers.
// Band numbers are claimed in ascending order, but execution order is unspecified
// with multiple workers. Every band finishes, even if a callback throws.
class BandPool {
public:
    // -1 selects DSR_RENDER_THREADS or the hardware default; 0 runs inline.
    explicit BandPool(int threads = -1)
        : worker_count_(threads < 0 ? configured_workers() : std::clamp(threads, 0, 6)) {}

    BandPool(const BandPool&) = delete;
    BandPool& operator=(const BandPool&) = delete;

    ~BandPool() { stop_workers(); }

    int worker_count() const noexcept { return worker_count_; }

    // Explicit terminal shutdown belongs to the owner, before library unload.
    // It waits for any submitted job, then joins all workers; repeated calls
    // and the later destructor are harmless. No pool is kept in static storage.
    void shutdown() {
        std::lock_guard submission(submission_mutex_);
        stop_workers();
    }

    void run(int bands, const std::function<void(int)>& job) {
        if (bands < 0) throw std::invalid_argument("negative band count");
        if (bands == 0) return;
        if (!job) throw std::invalid_argument("empty band callback");
        std::lock_guard submission(submission_mutex_);
        if (stopping_) throw std::logic_error("renderer pool is shut down");
        if (worker_count_ == 0) {
            std::exception_ptr error;
            for (int band = 0; band < bands; ++band) {
                try { job(band); }
                catch (...) { if (!error) error = std::current_exception(); }
            }
            if (error) std::rethrow_exception(error);
            return;
        }
        start_workers();
        std::unique_lock lock(mutex_);
        job_ = &job; // Borrowed until the blocking completion barrier below.
        next_band_ = 0;
        bands_ = bands;
        remaining_ = bands;
        error_ = nullptr;
        work_ready_.notify_all();
        completed_.wait(lock, [this] { return remaining_ == 0; });
        job_ = nullptr;
        const auto error = error_;
        lock.unlock();
        if (error) std::rethrow_exception(error);
    }

private:
    static int configured_workers() {
        const auto hardware = std::thread::hardware_concurrency();
        const int fallback = static_cast<int>(hardware <= 3U ? 1U : std::min(hardware - 2U, 6U));
        const char* value = std::getenv("DSR_RENDER_THREADS");
        if (value == nullptr) return fallback;
        const std::string_view text(value);
        int count = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || count < 0)
            return fallback;
        return std::clamp(count, 0, 6);
    }

    void start_workers() {
        if (started_ != 0) return;
        try {
            for (; started_ < worker_count_; ++started_)
                workers_[static_cast<std::size_t>(started_)] = std::thread([this] { worker(); });
        } catch (...) {
            // A partial thread-start failure must never destroy a joinable thread.
            stop_workers();
            { std::lock_guard lock(mutex_); stopping_ = false; }
            throw;
        }
    }

    void stop_workers() noexcept {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        work_ready_.notify_all();
        for (int i = 0; i < started_; ++i)
            workers_[static_cast<std::size_t>(i)].join();
        started_ = 0;
    }

    void worker() {
        std::unique_lock lock(mutex_);
        for (;;) {
            work_ready_.wait(lock, [this] { return stopping_ || next_band_ < bands_; });
            if (stopping_) return;
            const int band = next_band_++;
            const auto* job = job_;
            lock.unlock();
            std::exception_ptr error;
            try { (*job)(band); }
            catch (...) { error = std::current_exception(); }
            lock.lock();
            if (error && !error_) error_ = error;
            if (--remaining_ == 0) completed_.notify_one();
        }
    }

    const int worker_count_;
    std::array<std::thread, 6> workers_{};
    int started_ = 0;
    std::mutex submission_mutex_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable completed_;
    const std::function<void(int)>* job_ = nullptr;
    int next_band_ = 0;
    int bands_ = 0;
    int remaining_ = 0;
    bool stopping_ = false;
    std::exception_ptr error_;
};

} // namespace dsr
