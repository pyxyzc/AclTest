#include "physical_memory_common.h"
#include "physical_memory_utils.h"

#include <iostream>
#include <string>
#include <vector>

namespace acltest {
using namespace internal;
namespace {

bool RunSingleProcessVaToVaSubtest(const Options& options,
                                   const PhysicalMemoryConfig& config,
                                   size_t aligned_size)
{
    std::cout << "\n[single-process] VA-to-VA physical mapping memcpy\n";

    PhysicalMapping src;
    PhysicalMapping dst;
    bool ok = AllocateAndMapPhysical(config, aligned_size, &src) &&
              AllocateAndMapPhysical(config, aligned_size, &dst);

    const uint32_t seed = 0x77U;
    if (ok) {
        const auto expected = MakePattern(options.requested_size, seed);
        std::vector<uint8_t> actual(options.requested_size);
        ok = CopyHostToMapping(
                 src.virt, src.size, expected, config,
                 "aclrtMemcpy(" + std::string(config.write_tag) +
                     " single VA-to-VA source)") &&
             CopyMappingToMapping(
                 dst.virt, dst.size, src.virt, expected.size(), config,
                 "aclrtMemcpy(" + std::string(config.va_to_va_tag) +
                     " single VA-to-VA)") &&
             CopyMappingToHost(
                 &actual, dst.virt, actual.size(), config,
                 "aclrtMemcpy(" + std::string(config.read_tag) +
                     " single VA-to-VA result)") &&
             VerifyPattern(actual, seed, "single-process VA-to-VA pattern");
    }

    dst.Cleanup();
    src.Cleanup();
    return ok;
}

bool RunSingleProcessHostDeviceVaSubtest(const Options& options)
{
    std::cout << "\n[single-process] host VA <-> device VA physical mapping memcpy\n";

    const auto host_config = MakeHostConfig(options);
    const auto device_config = MakeDeviceConfig(options);

    size_t host_aligned_size = 0;
    bool ok = QueryAlignedSize(host_config, options.requested_size,
                               &host_aligned_size);

    size_t device_aligned_size = 0;
    ok = ok && QueryAlignedSize(device_config, options.requested_size,
                                &device_aligned_size);

    PhysicalMapping host;
    PhysicalMapping device;
    ok = ok && AllocateAndMapPhysical(host_config, host_aligned_size, &host);
    ok = ok && AllocateAndMapPhysical(device_config, device_aligned_size, &device);

    const uint32_t host_to_device_seed = 0x91U;
    if (ok) {
        const auto expected = MakePattern(options.requested_size, host_to_device_seed);
        std::vector<uint8_t> actual(options.requested_size);
        ok = CopyHostToMapping(
                 host.virt, host.size, expected, host_config,
                 "aclrtMemcpy(H2H host VA source)") &&
             CopyWithKind(
                 device.virt, device.size, host.virt, expected.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 "aclrtMemcpy(H2D host VA to device VA)") &&
             CopyMappingToHost(
                 &actual, device.virt, actual.size(), device_config,
                 "aclrtMemcpy(D2H host VA to device VA result)") &&
             VerifyPattern(actual, host_to_device_seed,
                           "single-process host VA to device VA pattern");
    }

    const uint32_t device_to_host_seed = 0xc3U;
    if (ok) {
        const auto expected = MakePattern(options.requested_size, device_to_host_seed);
        std::vector<uint8_t> actual(options.requested_size);
        ok = CopyHostToMapping(
                 device.virt, device.size, expected, device_config,
                 "aclrtMemcpy(H2D device VA source)") &&
             CopyWithKind(
                 host.virt, host.size, device.virt, expected.size(),
                 ACL_MEMCPY_DEVICE_TO_HOST,
                 "aclrtMemcpy(D2H device VA to host VA)") &&
             CopyMappingToHost(
                 &actual, host.virt, actual.size(), host_config,
                 "aclrtMemcpy(H2H device VA to host VA result)") &&
             VerifyPattern(actual, device_to_host_seed,
                           "single-process device VA to host VA pattern");
    }

    device.Cleanup();
    host.Cleanup();
    return ok;
}

}  // namespace

bool RunSingleProcessVmmTest(const Options& options)
{
    std::cout << "\n[single-process] physical allocation + virtual mapping\n";
    AclRuntime runtime;
    if (!runtime.Init() || !runtime.SetDevice(options.device)) {
        return false;
    }

    const auto config = MakeDeviceConfig(options);
    size_t aligned_size = 0;
    if (!QueryAlignedSize(config, options.requested_size, &aligned_size)) {
        return false;
    }

    PhysicalMapping mapping;
    if (!AllocateAndMapPhysical(config, aligned_size, &mapping)) {
        mapping.Cleanup();
        return false;
    }

    const uint32_t seed = 0x31U;
    const auto expected = MakePattern(options.requested_size, seed);
    std::vector<uint8_t> actual(options.requested_size);
    bool ok = CopyHostToMapping(
                  mapping.virt, mapping.size, expected, config,
                  "aclrtMemcpy(" + std::string(config.write_tag) + " single)") &&
              CopyMappingToHost(
                  &actual, mapping.virt, actual.size(), config,
                  "aclrtMemcpy(" + std::string(config.read_tag) + " single)") &&
              VerifyPattern(actual, seed, "single-process pattern");
    mapping.Cleanup();
    ok = RunSingleProcessVaToVaSubtest(options, config, aligned_size) && ok;
    ok = RunSingleProcessHostDeviceVaSubtest(options) && ok;
    return ok;
}

}  // namespace acltest
