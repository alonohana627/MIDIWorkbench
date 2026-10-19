#pragma once

#include <utility>

namespace mwb {

/// Visitor helper for std::visit over several lambdas (section 19.3: one shared helper).
template <class... Functions>
struct Overloaded : Functions... {
    using Functions::operator()...;
};
template <class... Functions>
Overloaded(Functions...) -> Overloaded<Functions...>;

/// Shared RAII owner for C handles; `Deleter` releases a valid handle.
template <class Handle, class Deleter, Handle Invalid = Handle{}>
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(Handle handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, Invalid)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, Invalid);
        }
        return *this;
    }

    [[nodiscard]] Handle get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const { return handle_ != Invalid; }
    [[nodiscard]] Handle release() { return std::exchange(handle_, Invalid); }
    void reset(Handle handle = Invalid) {
        if (handle_ != Invalid)
            Deleter{}(handle_);
        handle_ = handle;
    }

private:
    Handle handle_ = Invalid;
};

} // namespace mwb
