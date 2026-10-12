#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif


#include "dsr/fiber.h"

#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace dsr {

thread_local Fiber* Fiber::current_ = nullptr;
thread_local unsigned char Fiber::thread_token_ = 0;
Fiber* Fiber::current() noexcept {
    return current_;
}


#if defined(_WIN32)

thread_local void* Fiber::host_fiber_ = nullptr;
thread_local bool Fiber::owns_host_fiber_ = false;
thread_local std::size_t Fiber::live_fibers_ = 0;
thread_local std::fenv_t Fiber::host_floating_point_{};

Fiber::Fiber(std::function<void()> entry)
    : entry_(std::move(entry)), owner_thread_(&thread_token_) {
    if (!entry_) {
        throw std::invalid_argument("Fiber entry function must not be empty");
    }

    std::fegetenv(&floating_point_);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    // Windows keeps a moving guard page inside the reservation. Leave space
    // for it in addition to the same usable minimum as the Linux mapping.
    native_fiber_ = CreateFiberEx(system.dwPageSize, kStackSize + system.dwPageSize,
                                  FIBER_FLAG_FLOAT_SWITCH,
                                  &Fiber::entry_trampoline, this);
    if (native_fiber_ == nullptr) {
        const DWORD error = GetLastError();
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "CreateFiberEx");
    }
    ++live_fibers_;
}

Fiber::~Fiber() {
    if (current_ == this || owner_thread_ != &thread_token_) {
        std::terminate();
    }
    if (native_fiber_ != nullptr) {
        DeleteFiber(native_fiber_);
    }
    if (--live_fibers_ == 0) {
        // Do not leave an SDL/Godot worker converted after its last session,
        // and never convert a fiber owned by an embedding host back to a thread.
        if (owns_host_fiber_ && !ConvertFiberToThread()) std::terminate();
        host_fiber_ = nullptr;
        owns_host_fiber_ = false;
    }
}

void Fiber::switch_to(Fiber& fiber) {
    if (fiber.owner_thread_ != &thread_token_) {
        throw std::logic_error("Fiber switched from a different thread");
    }
    if (fiber.finished_ || current_ == &fiber) {
        return;
    }

    Fiber* caller = current_;
    auto& caller_floating_point = caller ? caller->floating_point_ : host_floating_point_;
    std::fegetenv(&caller_floating_point);

    if (host_fiber_ == nullptr) {
        void* host = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (host == nullptr) {
            const DWORD error = GetLastError();
            if (error == ERROR_ALREADY_FIBER) {
                host = GetCurrentFiber();
            } else {
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "ConvertThreadToFiberEx");
            }
        }
        else {
            owns_host_fiber_ = true;
        }
        host_fiber_ = host;
    }

    current_ = &fiber;
    SwitchToFiber(fiber.native_fiber_);
    current_ = caller;
    // The host CRT's rounding/exception controls must follow the continuation,
    // even on Fiber API implementations which do not preserve those controls.
    std::fesetenv(&caller_floating_point);
}

void Fiber::yield() noexcept {
    Fiber* fiber = current_;
    if (fiber == nullptr) {
        return;
    }

    std::fegetenv(&fiber->floating_point_);
    current_ = nullptr;
    SwitchToFiber(host_fiber_);
    current_ = fiber;
    std::fesetenv(&fiber->floating_point_);
}

void __stdcall Fiber::entry_trampoline(void* parameter) noexcept {
    static_cast<Fiber*>(parameter)->run_entry();
}

[[noreturn]] void Fiber::run_entry() noexcept {
    std::fesetenv(&floating_point_);
    entry_();
    finished_ = true;
    yield();
    std::terminate();
}

bool Fiber::finished() const noexcept {
    return finished_;
}

#else

thread_local Fiber::Context Fiber::main_context_{};

extern "C" void dsr_fiber_context_switch(void* from, void* to) noexcept;

Fiber::Fiber(std::function<void()> entry)
    : entry_(std::move(entry)), owner_thread_(&thread_token_) {
    if (!entry_) {
        throw std::invalid_argument("Fiber entry function must not be empty");
    }

    const long queried_page_size = sysconf(_SC_PAGESIZE);
    if (queried_page_size <= 0) {
        throw std::runtime_error("Unable to determine system page size for Fiber stack");
    }
    const std::size_t page_size = static_cast<std::size_t>(queried_page_size);
    const std::size_t usable_stack_size =
        kStackSize + (page_size - kStackSize % page_size) % page_size;
    if (page_size > (std::numeric_limits<std::size_t>::max() - usable_stack_size) / 2) {
        throw std::length_error("Fiber stack mapping size overflow");
    }
    const std::size_t mapping_size = usable_stack_size + 2 * page_size;

    void* mapping = mmap(nullptr, mapping_size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap Fiber stack");
    }

    auto* stack_start = static_cast<unsigned char*>(mapping) + page_size;
    if (mprotect(stack_start, usable_stack_size, PROT_READ | PROT_WRITE) != 0) {
        const int error = errno;
        munmap(mapping, mapping_size);
        throw std::system_error(error, std::generic_category(), "mprotect Fiber stack");
    }

    stack_mapping_ = mapping;
    stack_mapping_size_ = mapping_size;

    auto* stack_top = stack_start + usable_stack_size;
    context_.rsp = reinterpret_cast<std::uintptr_t>(stack_top) - sizeof(std::uintptr_t);
    *reinterpret_cast<std::uintptr_t*>(context_.rsp) = 0;
    context_.rip = &Fiber::entry_trampoline;
    asm volatile("stmxcsr %0" : "=m"(context_.mxcsr));
    asm volatile("fnstcw %0" : "=m"(context_.x87_control));
}

Fiber::~Fiber() {
    if (current_ == this) {
        std::terminate();
    }
    if (stack_mapping_ != nullptr) {
        munmap(stack_mapping_, stack_mapping_size_);
    }
}

void Fiber::switch_to(Fiber& fiber) {
    if (fiber.owner_thread_ != &thread_token_) {
        throw std::logic_error("Fiber switched from a different thread");
    }
    if (fiber.finished_ || current_ == &fiber) {
        return;
    }

    Fiber* caller = current_;
    Context& caller_context = caller == nullptr ? main_context_ : caller->context_;
    current_ = &fiber;
    dsr_fiber_context_switch(&caller_context, &fiber.context_);
    current_ = caller;
}

void Fiber::yield() noexcept {
    Fiber* fiber = current_;
    if (fiber == nullptr) {
        return;
    }

    current_ = nullptr;
    dsr_fiber_context_switch(&fiber->context_, &main_context_);
    current_ = fiber;
}

void Fiber::entry_trampoline() noexcept {
    Fiber* fiber = current_;
    if (fiber == nullptr) {
        std::terminate();
    }
    fiber->run_entry();
}

[[noreturn]] void Fiber::run_entry() noexcept {
    entry_();
    finished_ = true;
    yield();
    std::terminate();
}

bool Fiber::finished() const noexcept {
    return finished_;
}

#endif

} // namespace dsr
