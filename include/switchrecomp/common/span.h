#pragma once

#include "switchrecomp/common/types.h"

#include <cassert>

namespace sr::common {

// Minimal span replacement (C++17 has no std::span).
template <typename T>
class span {
public:
    span() noexcept : data_(nullptr), size_(0) {}
    span(T* data, usize size) noexcept : data_(data), size_(size) {}

    template <usize N>
    span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}

    T* data() const noexcept { return data_; }
    usize size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    T& operator[](usize i) noexcept {
        assert(i < size_);
        return data_[i];
    }
    const T& operator[](usize i) const noexcept {
        assert(i < size_);
        return data_[i];
    }

    T* begin() const noexcept { return data_; }
    T* end() const noexcept { return data_ + size_; }

private:
    T* data_;
    usize size_;
};

} // namespace sr::common
