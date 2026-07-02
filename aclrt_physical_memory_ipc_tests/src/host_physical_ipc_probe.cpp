#include "physical_memory_common.h"

int main(int argc, char** argv)
{
    const acltest::TestCase tests[] = {
        {"host physical memory IPC", acltest::RunHostPhysicalIpcTest},
    };
    return acltest::RunTestProgram(argc, argv, "aclrt host physical memory IPC probe",
                                   tests, 1);
}
