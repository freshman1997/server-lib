#include "native_platform.h"

#include <cstdlib>
#include <iostream>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    using yuan::app::ClassifyNativeError;
    using yuan::app::DescribeNativeError;
    using yuan::app::GetLastNativeError;
    using yuan::app::IsNativeRetryableError;
    using yuan::app::NativeError;

    const int no_err = 0;
    if (!require(ClassifyNativeError(no_err) == NativeError::none, "zero should classify to none")) {
        return 10;
    }

#ifdef _WIN32
    const int retry_err = WSAEWOULDBLOCK;
    const int timeout_err = WSAETIMEDOUT;
#else
    const int retry_err = EAGAIN;
    const int timeout_err = ETIMEDOUT;
#endif

    if (!require(IsNativeRetryableError(retry_err), "retryable error classification mismatch")) {
        return 11;
    }
    if (!require(!IsNativeRetryableError(timeout_err), "timeout should not be retryable")) {
        return 12;
    }

    const auto timeout_kind = ClassifyNativeError(timeout_err);
    if (!require(timeout_kind == NativeError::timed_out, "timeout should classify to timed_out")) {
        return 13;
    }

    const std::string timeout_text = DescribeNativeError(timeout_err);
    if (!require(!timeout_text.empty(), "error description should not be empty")) {
        return 14;
    }

    const int last = GetLastNativeError();
    (void)last;

    std::cout << "native platform tests passed\n";
    return EXIT_SUCCESS;
}
