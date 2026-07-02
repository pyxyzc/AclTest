#include "physical_memory_common.h"

int main(int argc, char** argv)
{
    const acltest::TestCase tests[] = {
        {"device physical memory IPC", acltest::RunDevicePhysicalIpcTest},
    };
    return acltest::RunTestProgram(argc, argv, "aclrt device physical memory IPC probe",
                                   tests, 1);
}
