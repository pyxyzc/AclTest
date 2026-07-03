#include "physical_memory_common.h"

int main(int argc, char** argv)
{
    const acltest::TestCase tests[] = {
        {"single-process VMM", acltest::RunSingleProcessVmmTest},
    };
    return acltest::RunTestProgram(
        argc, argv, "single-process VMM", tests, 1,
        acltest::StartupDisplayMode::SingleProcessBanner);
}
