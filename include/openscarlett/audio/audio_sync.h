#pragma once

// Native Win32 SRWLOCK / CONDITION_VARIABLE synchronization primitives for OpenScarlett audio shims.
// Guest titles may ship an older msvcp140.dll (e.g. 14.29) whose std::mutex / std::condition_variable
// ABI is incompatible with MSVC 19.44-compiled shims; using kernel32 SRWLOCK avoids deadlocks/crashes.

#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <chrono>
#  include <condition_variable>
#  include <mutex>
#endif

namespace openscarlett::audio {

class SrwLock {
public:
#if defined(_WIN32)
    SrwLock() noexcept = default;
#else
    SrwLock() = default;
#endif
    SrwLock(const SrwLock&) = delete;
    SrwLock& operator=(const SrwLock&) = delete;

    void Lock() noexcept {
#if defined(_WIN32)
        ::AcquireSRWLockExclusive(&lock_);
#else
        mutex_.lock();
#endif
    }

    void Unlock() noexcept {
#if defined(_WIN32)
        ::ReleaseSRWLockExclusive(&lock_);
#else
        mutex_.unlock();
#endif
    }

    void LockShared() noexcept {
#if defined(_WIN32)
        ::AcquireSRWLockShared(&lock_);
#else
        mutex_.lock();
#endif
    }

    void UnlockShared() noexcept {
#if defined(_WIN32)
        ::ReleaseSRWLockShared(&lock_);
#else
        mutex_.unlock();
#endif
    }

#if defined(_WIN32)
    SRWLOCK* NativeHandle() noexcept { return &lock_; }
#else
    std::mutex& NativeMutex() noexcept { return mutex_; }
#endif

private:
#if defined(_WIN32)
    SRWLOCK lock_ = SRWLOCK_INIT;
#else
    std::mutex mutex_;
#endif
};

class SrwLockGuard {
public:
    explicit SrwLockGuard(SrwLock& lock) noexcept : lock_(lock) { lock_.Lock(); }
    ~SrwLockGuard() noexcept { lock_.Unlock(); }

    SrwLockGuard(const SrwLockGuard&) = delete;
    SrwLockGuard& operator=(const SrwLockGuard&) = delete;

private:
    SrwLock& lock_;
};

class SrwSharedGuard {
public:
    explicit SrwSharedGuard(SrwLock& lock) noexcept : lock_(lock) { lock_.LockShared(); }
    ~SrwSharedGuard() noexcept { lock_.UnlockShared(); }

    SrwSharedGuard(const SrwSharedGuard&) = delete;
    SrwSharedGuard& operator=(const SrwSharedGuard&) = delete;

private:
    SrwLock& lock_;
};

class SrwCondVar {
public:
    SrwCondVar() noexcept = default;
    SrwCondVar(const SrwCondVar&) = delete;
    SrwCondVar& operator=(const SrwCondVar&) = delete;

    void NotifyOne() noexcept {
#if defined(_WIN32)
        ::WakeConditionVariable(&cv_);
#else
        cv_.notify_one();
#endif
    }

    void NotifyAll() noexcept {
#if defined(_WIN32)
        ::WakeAllConditionVariable(&cv_);
#else
        cv_.notify_all();
#endif
    }

    // Must be called with `lock` held exclusively. Returns true if signaled, false on timeout.
    bool WaitForMs(SrwLock& lock, uint32_t timeout_ms) noexcept {
#if defined(_WIN32)
        return ::SleepConditionVariableSRW(&cv_, lock.NativeHandle(), timeout_ms, 0u) != FALSE;
#else
        std::unique_lock<std::mutex> ul(lock.NativeMutex(), std::adopt_lock);
        const bool ok = (cv_.wait_for(ul, std::chrono::milliseconds(timeout_ms)) == std::cv_status::no_timeout);
        ul.release();
        return ok;
#endif
    }

private:
#if defined(_WIN32)
    CONDITION_VARIABLE cv_ = CONDITION_VARIABLE_INIT;
#else
    std::condition_variable cv_;
#endif
};

}  // namespace openscarlett::audio
