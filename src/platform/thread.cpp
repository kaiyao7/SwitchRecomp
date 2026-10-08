// platform thread implementation
#include "switchrecomp/platform/thread.h"
#include <thread>
#include <mutex>

namespace sr::platform {

struct Thread::Impl {
    std::thread handle;
};

Thread::Thread(Entry entry) : impl_(std::make_unique<Impl>()) {
    impl_->handle = std::thread(std::move(entry));
}

Thread::~Thread() {
    if (impl_ && impl_->handle.joinable()) impl_->handle.join();
}

void Thread::Join() {
    if (impl_ && impl_->handle.joinable()) impl_->handle.join();
}

struct Mutex::Impl {
    std::mutex handle;
};

Mutex::Mutex() : impl_(std::make_unique<Impl>()) {}
Mutex::~Mutex() = default;
void Mutex::Lock() { impl_->handle.lock(); }
void Mutex::Unlock() { impl_->handle.unlock(); }

} // namespace sr::platform
