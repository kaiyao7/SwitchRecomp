#pragma once

#include <string>
#include <utility>
#include <variant>

namespace sr::common {

struct Error {
    std::string message;
};

// Lightweight Result<T> (either T or Error).
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error err) : data_(std::move(err)) {}

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }

    const Error& error() const { return std::get<Error>(data_); }

private:
    std::variant<T, Error> data_;
};

} // namespace sr::common
