#include "physical_memory_common.h"

int main(int argc, char** argv)
{
    const acltest::TestCase tests[] = {
        {"single-process VMM", acltest::RunSingleProcessVmmTest},
    };
    return acltest::RunTestProgram(argc, argv, "aclrt single-process VMM probe",
                                   tests, 1);
}
