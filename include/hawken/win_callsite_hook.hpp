#pragma once

#if !defined(_WIN64)
#error The callsite hook is supported on Windows x64 only.
#endif

#include <windows.h>

namespace hawken::client::diagnostics {

enum class CallArgumentMode {
    unchanged,
    prepend_rdi
};

class RelativeCallHook final {
public:
    constexpr RelativeCallHook() noexcept = default;
    RelativeCallHook(const RelativeCallHook&) = delete;
    RelativeCallHook& operator=(const RelativeCallHook&) = delete;

    bool install(void* call_site, void* expected_target, void* replacement,
        void** original_target,
        CallArgumentMode argument_mode = CallArgumentMode::unchanged) noexcept;
    bool restore() noexcept;
    bool installed() const noexcept { return call_site_ != nullptr; }

private:
    unsigned char* call_site_ = nullptr;
    void* relay_ = nullptr;
    unsigned char original_[5]{};
};

}
