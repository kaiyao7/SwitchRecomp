#pragma once

#include <functional>
#include <memory>

namespace sr::platform {

// RAII thread. Joins on destruction if not already joined.
class Thread {
public:
    using Entry = std::function<void()>;

    explicit Thread(Entry entry);
    ~Thread();

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void Join();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Mutex {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock();
    void Unlock();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MutexGuard {
public:
    explicit MutexGuard(Mutex& m) : m_(m) { m_.Lock(); }
    ~MutexGuard() { m_.Unlock(); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& m_;
};

} // namespace sr::platform
