#include "physical_memory_common.h"
#include "physical_memory_ipc_internal.h"

namespace acltest {

bool RunDevicePhysicalIpcTest(const Options& options)
{
    return internal::RunDevicePhysicalIpcEndpointTests(options);
}

bool RunHostPhysicalIpcTest(const Options& options)
{
    return internal::RunHostPhysicalIpcEndpointTests(options);
}

bool RunDeviceHostPhysicalIpcTest(const Options& options)
{
    return internal::RunDeviceHostPhysicalIpcEndpointTest(options);
}

}  // namespace acltest
