#include "physical_memory_common.h"
#include "console_utils.h"
#include "physical_memory_utils.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace acltest {
using namespace internal;
namespace {

enum class MemorySide {
    Host,
    Device,
};

struct EndpointSpec {
    const char* name = "";
    void* ptr = nullptr;
    size_t size = 0;
    MemorySide side = MemorySide::Host;
    bool is_host_buffer = false;
};

struct Endpoint {
    const char* name = "";
    void* ptr = nullptr;
    size_t size = 0;
    MemorySide side = MemorySide::Host;
};

struct DeviceAllocation {
    void* ptr = nullptr;
    size_t size = 0;
    bool allocated = false;

    bool Allocate(size_t bytes)
    {
        size = bytes;
        const aclError ret = aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_FIRST);
        allocated = (ret == ACL_SUCCESS);
        return LogAcl("aclrtMalloc(device buffer)", ret);
    }

    void Cleanup()
    {
        if (allocated && ptr != nullptr) {
            (void)LogAcl("cleanup aclrtFree(device buffer)", aclrtFree(ptr));
            ptr = nullptr;
            allocated = false;
        }
    }
};

EndpointSpec HostBufferSpec()
{
    EndpointSpec spec;
    spec.name = "host buffer";
    spec.side = MemorySide::Host;
    spec.is_host_buffer = true;
    return spec;
}

EndpointSpec DeviceBufferSpec(const DeviceAllocation& allocation)
{
    EndpointSpec spec;
    spec.name = "device buffer";
    spec.ptr = allocation.ptr;
    spec.size = allocation.size;
    spec.side = MemorySide::Device;
    return spec;
}

EndpointSpec DeviceVaSpec(const PhysicalMapping& mapping)
{
    EndpointSpec spec;
    spec.name = "device VA";
    spec.ptr = mapping.virt;
    spec.size = mapping.size;
    spec.side = MemorySide::Device;
    return spec;
}

EndpointSpec HostVaSpec(const PhysicalMapping& mapping)
{
    EndpointSpec spec;
    spec.name = "host VA";
    spec.ptr = mapping.virt;
    spec.size = mapping.size;
    spec.side = MemorySide::Host;
    return spec;
}

Endpoint ResolveEndpoint(const EndpointSpec& spec, std::vector<uint8_t>* host_buffer)
{
    Endpoint endpoint;
    endpoint.name = spec.name;
    endpoint.ptr = spec.is_host_buffer ? host_buffer->data() : spec.ptr;
    endpoint.size = spec.is_host_buffer ? host_buffer->size() : spec.size;
    endpoint.side = spec.side;
    return endpoint;
}

aclrtMemcpyKind CopyKind(const Endpoint& dst, const Endpoint& src)
{
    if (src.side == MemorySide::Host && dst.side == MemorySide::Host) {
        return ACL_MEMCPY_HOST_TO_HOST;
    }
    if (src.side == MemorySide::Host && dst.side == MemorySide::Device) {
        return ACL_MEMCPY_HOST_TO_DEVICE;
    }
    if (src.side == MemorySide::Device && dst.side == MemorySide::Host) {
        return ACL_MEMCPY_DEVICE_TO_HOST;
    }
    return ACL_MEMCPY_DEVICE_TO_DEVICE;
}

bool CopyEndpoint(const Endpoint& dst, const Endpoint& src, size_t copy_size,
                  const std::string& phase)
{
    const std::string label = "aclrtMemcpy(" + phase + " " +
                              std::string(src.name) + " -> " + dst.name + ")";
    return CopyWithKind(dst.ptr, dst.size, src.ptr, copy_size, CopyKind(dst, src),
                        label);
}

bool VerifyDirection(const std::vector<uint8_t>& data, uint32_t seed,
                     const std::string& direction)
{
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t expected =
            static_cast<uint8_t>((seed + (i * 131U) + (i >> 3U)) & 0xffU);
        if (data[i] != expected) {
            std::cerr << "  [single-process] " << direction
                      << " mismatch at offset=" << i
                      << ", expected=" << static_cast<int>(expected)
                      << ", actual=" << static_cast<int>(data[i]) << "\n";
            return false;
        }
    }
    PrintGreen("  " + direction + " √");
    return true;
}

bool RunCopyDirection(const EndpointSpec& src_spec, const EndpointSpec& dst_spec,
                      size_t requested_size, uint32_t seed)
{
    std::vector<uint8_t> expected = MakePattern(requested_size, seed);
    std::vector<uint8_t> actual(requested_size, 0);

    Endpoint src = ResolveEndpoint(src_spec, &expected);
    Endpoint dst = ResolveEndpoint(dst_spec, &actual);

    bool ok = true;
    if (!src_spec.is_host_buffer) {
        Endpoint setup_src;
        setup_src.name = "host buffer";
        setup_src.ptr = expected.data();
        setup_src.size = expected.size();
        setup_src.side = MemorySide::Host;
        ok = CopyEndpoint(src, setup_src, expected.size(), "setup");
    }

    ok = ok && CopyEndpoint(dst, src, expected.size(), "copy");

    if (ok && !dst_spec.is_host_buffer) {
        Endpoint readback_dst;
        readback_dst.name = "host buffer";
        readback_dst.ptr = actual.data();
        readback_dst.size = actual.size();
        readback_dst.side = MemorySide::Host;
        ok = CopyEndpoint(readback_dst, dst, actual.size(), "readback");
    }

    const std::string direction =
        std::string(src_spec.name) + " -> " + dst_spec.name;
    return ok && VerifyDirection(actual, seed, direction);
}

void PrintSubtestSection(const std::string& title)
{
    std::cout << "\n";
    PrintRed(title);
}

bool AllocatePhysicalMapping(const PhysicalMemoryConfig& config, size_t requested_size,
                             PhysicalMapping* mapping)
{
    size_t aligned_size = 0;
    return QueryAlignedSize(config, requested_size, &aligned_size) &&
           AllocateAndMapPhysical(config, aligned_size, mapping);
}

bool RunDeviceVaDeviceBufferSubtest(const Options& options,
                                    const PhysicalMemoryConfig& device_config)
{
    PrintSubtestSection("[single-process] device VA <-> device buffer");

    PhysicalMapping device_va;
    DeviceAllocation device_buffer;
    bool ok = AllocatePhysicalMapping(device_config, options.requested_size,
                                      &device_va) &&
              device_buffer.Allocate(options.requested_size);
    if (ok) {
        const EndpointSpec va = DeviceVaSpec(device_va);
        const EndpointSpec buffer = DeviceBufferSpec(device_buffer);
        ok = RunCopyDirection(va, buffer, options.requested_size, 0x1101U) &&
             RunCopyDirection(buffer, va, options.requested_size, 0x1102U);
    }

    device_buffer.Cleanup();
    device_va.Cleanup();
    return ok;
}

bool RunDeviceVaHostBufferSubtest(const Options& options,
                                  const PhysicalMemoryConfig& device_config)
{
    PrintSubtestSection("[single-process] device VA <-> host buffer");

    PhysicalMapping device_va;
    bool ok = AllocatePhysicalMapping(device_config, options.requested_size,
                                      &device_va);
    if (ok) {
        const EndpointSpec va = DeviceVaSpec(device_va);
        const EndpointSpec buffer = HostBufferSpec();
        ok = RunCopyDirection(va, buffer, options.requested_size, 0x1201U) &&
             RunCopyDirection(buffer, va, options.requested_size, 0x1202U);
    }

    device_va.Cleanup();
    return ok;
}

bool RunDeviceVaDeviceVaSubtest(const Options& options,
                                const PhysicalMemoryConfig& device_config)
{
    PrintSubtestSection("[single-process] device VA <-> device VA");

    PhysicalMapping left;
    PhysicalMapping right;
    bool ok = AllocatePhysicalMapping(device_config, options.requested_size, &left) &&
              AllocatePhysicalMapping(device_config, options.requested_size, &right);
    if (ok) {
        const EndpointSpec left_va = DeviceVaSpec(left);
        const EndpointSpec right_va = DeviceVaSpec(right);
        ok = RunCopyDirection(left_va, right_va, options.requested_size, 0x1301U) &&
             RunCopyDirection(right_va, left_va, options.requested_size, 0x1302U);
    }

    right.Cleanup();
    left.Cleanup();
    return ok;
}

bool RunHostVaDeviceBufferSubtest(const Options& options,
                                  const PhysicalMemoryConfig& host_config)
{
    PrintSubtestSection("[single-process] host VA <-> device buffer");

    PhysicalMapping host_va;
    DeviceAllocation device_buffer;
    bool ok = AllocatePhysicalMapping(host_config, options.requested_size, &host_va) &&
              device_buffer.Allocate(options.requested_size);
    if (ok) {
        const EndpointSpec va = HostVaSpec(host_va);
        const EndpointSpec buffer = DeviceBufferSpec(device_buffer);
        ok = RunCopyDirection(va, buffer, options.requested_size, 0x1401U) &&
             RunCopyDirection(buffer, va, options.requested_size, 0x1402U);
    }

    device_buffer.Cleanup();
    host_va.Cleanup();
    return ok;
}

bool RunHostVaHostBufferSubtest(const Options& options,
                                const PhysicalMemoryConfig& host_config)
{
    PrintSubtestSection("[single-process] host VA <-> host buffer");

    PhysicalMapping host_va;
    bool ok = AllocatePhysicalMapping(host_config, options.requested_size, &host_va);
    if (ok) {
        const EndpointSpec va = HostVaSpec(host_va);
        const EndpointSpec buffer = HostBufferSpec();
        ok = RunCopyDirection(va, buffer, options.requested_size, 0x1501U) &&
             RunCopyDirection(buffer, va, options.requested_size, 0x1502U);
    }

    host_va.Cleanup();
    return ok;
}

bool RunHostVaHostVaSubtest(const Options& options,
                            const PhysicalMemoryConfig& host_config)
{
    PrintSubtestSection("[single-process] host VA <-> host VA");

    PhysicalMapping left;
    PhysicalMapping right;
    bool ok = AllocatePhysicalMapping(host_config, options.requested_size, &left) &&
              AllocatePhysicalMapping(host_config, options.requested_size, &right);
    if (ok) {
        const EndpointSpec left_va = HostVaSpec(left);
        const EndpointSpec right_va = HostVaSpec(right);
        ok = RunCopyDirection(left_va, right_va, options.requested_size, 0x1601U) &&
             RunCopyDirection(right_va, left_va, options.requested_size, 0x1602U);
    }

    right.Cleanup();
    left.Cleanup();
    return ok;
}

bool RunDeviceVaHostVaSubtest(const Options& options,
                              const PhysicalMemoryConfig& device_config,
                              const PhysicalMemoryConfig& host_config)
{
    PrintSubtestSection("[single-process] device VA <-> host VA");

    PhysicalMapping device_va;
    PhysicalMapping host_va;
    bool ok = AllocatePhysicalMapping(device_config, options.requested_size,
                                      &device_va) &&
              AllocatePhysicalMapping(host_config, options.requested_size, &host_va);
    if (ok) {
        const EndpointSpec device = DeviceVaSpec(device_va);
        const EndpointSpec host = HostVaSpec(host_va);
        ok = RunCopyDirection(device, host, options.requested_size, 0x1701U) &&
             RunCopyDirection(host, device, options.requested_size, 0x1702U);
    }

    host_va.Cleanup();
    device_va.Cleanup();
    return ok;
}

}  // namespace

bool RunSingleProcessVmmTest(const Options& options)
{
    AclRuntime runtime;
    if (!runtime.Init() || !runtime.SetDevice(options.device)) {
        return false;
    }

    const auto device_config = MakeDeviceConfig(options);
    const auto host_config = MakeHostConfig(options);

    bool ok = true;
    ok = RunDeviceVaDeviceBufferSubtest(options, device_config) && ok;
    ok = RunDeviceVaHostBufferSubtest(options, device_config) && ok;
    ok = RunDeviceVaDeviceVaSubtest(options, device_config) && ok;
    ok = RunHostVaDeviceBufferSubtest(options, host_config) && ok;
    ok = RunHostVaHostBufferSubtest(options, host_config) && ok;
    ok = RunHostVaHostVaSubtest(options, host_config) && ok;
    ok = RunDeviceVaHostVaSubtest(options, device_config, host_config) && ok;
    return ok;
}

}  // namespace acltest
