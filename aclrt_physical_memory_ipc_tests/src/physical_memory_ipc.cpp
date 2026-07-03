#include "physical_memory_common.h"
#include "physical_memory_ipc_internal.h"
#include "physical_memory_utils.h"

namespace acltest {
namespace {

bool RunIpcTest(const Options& options,
                const internal::PhysicalMemoryConfig& config)
{
    bool ok = internal::RunSingleMappingIpcTest(options, config);
    ok = internal::RunVaToVaIpcTest(options, config) && ok;
    return ok;
}

}  // namespace

bool RunDevicePhysicalIpcTest(const Options& options)
{
    return RunIpcTest(options, internal::MakeDeviceConfig(options));
}

bool RunHostPhysicalIpcTest(const Options& options)
{
    return RunIpcTest(options, internal::MakeHostConfig(options));
}

}  // namespace acltest
