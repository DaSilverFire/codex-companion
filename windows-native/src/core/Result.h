#pragma once

#include "core/CompanionError.h"

#include <utility>
#include <variant>

namespace companion {

template <typename T>
class Result final {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(CompanionError error) { return Result(std::move(error)); }

    bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }
    T& value() { return std::get<T>(storage_); }
    const CompanionError& error() const { return std::get<CompanionError>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(CompanionError error) : storage_(std::move(error)) {}
    std::variant<T, CompanionError> storage_;
};

template <>
class Result<void> final {
public:
    static Result success() { return Result(true, {}); }
    static Result failure(CompanionError error) { return Result(false, std::move(error)); }

    bool hasValue() const noexcept { return ok_; }
    const CompanionError& error() const { return error_; }

private:
    Result(bool ok, CompanionError error) : ok_(ok), error_(std::move(error)) {}
    bool ok_;
    CompanionError error_;
};

} // namespace companion
