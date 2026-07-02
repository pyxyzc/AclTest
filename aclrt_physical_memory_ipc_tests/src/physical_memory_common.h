#pragma once

#include <acl/acl.h>

#include <cstddef>

namespace acltest {

enum class ShareKind {
    Default,
    Fabric,
};

struct Options {
    int device = 0;
    int host_numa = 0;
    size_t requested_size = 4096;
    bool disable_pid_validation = false;
    bool force_v1 = false;
    ShareKind share_kind = ShareKind::Default;
};

using TestFn = bool (*)(const Options&);

struct TestCase {
    const char* name;
    TestFn run;
};

int RunTestProgram(int argc, char** argv, const char* title,
                   const TestCase* tests, size_t test_count);

bool RunSingleProcessVmmTest(const Options& options);
bool RunDevicePhysicalIpcTest(const Options& options);
bool RunHostPhysicalIpcTest(const Options& options);

}  // namespace acltest
