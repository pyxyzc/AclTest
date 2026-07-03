#include "physical_memory_common.h"

int main(int argc, char** argv)
{
    const acltest::TestCase tests[] = {
        {"single-process VMM", acltest::RunSingleProcessVmmTest},
        {"device physical memory IPC", acltest::RunDevicePhysicalIpcTest},
        {"host physical memory IPC", acltest::RunHostPhysicalIpcTest},
        {"device-host physical memory IPC", acltest::RunDeviceHostPhysicalIpcTest},
    };
    return acltest::RunTestProgram(argc, argv, "aclrt physical memory IPC probe",
                                   tests, 4);
}
