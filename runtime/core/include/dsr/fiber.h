#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#if defined(_WIN32)
#include <cfenv>
#endif

#if !defined(_WIN32) && !(defined(__linux__) && defined(__x86_64__))
#error "dsr::Fiber supports Windows or x86-64 Linux only"
#endif

namespace dsr {

// A stackful cooperative context. Fibers are bound to the thread on which
// they are constructed. The thread's existing context is the implicit main
// fiber; yield() from that context is a no-op.
class Fiber final {
public:
    explicit Fiber(std::function<void()> entry);
    ~Fiber();

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // Run or resume fiber until it yields or its entry function returns.
    // Switching to a completed fiber is a no-op.
    static void switch_to(Fiber& fiber);

    // Suspend the current fiber and return to the implicit main fiber.
    static void yield() noexcept;

    // The currently executing fiber, or nullptr on the implicit host context.
    static Fiber* current() noexcept;

    bool finished() const noexcept;

private:
    // Guest stacks live in emulated RAM; every host continuation has at least
    // 256 KiB independently of the guest thread's stack bounds.
    static constexpr std::size_t kStackSize = 256 * 1024;

#if defined(_WIN32)
    static void __stdcall entry_trampoline(void* parameter) noexcept;
#else
    struct Context {
        std::uintptr_t rsp{};
        std::uintptr_t rbx{};
        std::uintptr_t rbp{};
        std::uintptr_t r12{};
        std::uintptr_t r13{};
        std::uintptr_t r14{};
        std::uintptr_t r15{};
        void (*rip)() noexcept{};
        std::uint32_t mxcsr{};
        std::uint16_t x87_control{};
    };

    static void entry_trampoline() noexcept;
    static thread_local Context main_context_;

    void* stack_mapping_{};
    std::size_t stack_mapping_size_{};
    Context context_{};
#endif

    [[noreturn]] void run_entry() noexcept;

    static thread_local Fiber* current_;
    static thread_local unsigned char thread_token_;

#if defined(_WIN32)
    static thread_local void* host_fiber_;
    static thread_local bool owns_host_fiber_;
    static thread_local std::size_t live_fibers_;
    static thread_local std::fenv_t host_floating_point_;
    std::fenv_t floating_point_{};
    void* native_fiber_{};
#endif

    std::function<void()> entry_;
    bool finished_{};
    const unsigned char* owner_thread_{};
};

} // namespace dsr
