#include "console_utils.h"
#include "physical_memory_ipc_internal.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace acltest::internal {
namespace {

enum class MemorySide {
    Host,
    Device,
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

struct ParentHandle {
    IpcMemoryKind memory_kind = IpcMemoryKind::DevicePhysical;
    PhysicalMemoryConfig config = {};
    PhysicalMapping mapping = {};
    size_t aligned_size = 0;
};

struct IpcDirection {
    IpcEndpointKind src = IpcEndpointKind::HostBuffer;
    IpcEndpointKind dst = IpcEndpointKind::HostBuffer;
    uint32_t seed = 0;
};

IpcEndpointKind EndpointKindFromWire(uint32_t value)
{
    switch (static_cast<IpcEndpointKind>(value)) {
        case IpcEndpointKind::ImportedDeviceVa:
        case IpcEndpointKind::ImportedHostVa:
        case IpcEndpointKind::DeviceBuffer:
        case IpcEndpointKind::HostBuffer:
            return static_cast<IpcEndpointKind>(value);
    }
    return IpcEndpointKind::HostBuffer;
}

IpcMemoryKind MemoryKindFromWire(uint32_t value)
{
    switch (static_cast<IpcMemoryKind>(value)) {
        case IpcMemoryKind::DevicePhysical:
        case IpcMemoryKind::HostPhysical:
            return static_cast<IpcMemoryKind>(value);
    }
    return IpcMemoryKind::DevicePhysical;
}

const char* EndpointName(IpcEndpointKind endpoint)
{
    switch (endpoint) {
        case IpcEndpointKind::ImportedDeviceVa:
            return "imported device VA";
        case IpcEndpointKind::ImportedHostVa:
            return "imported host VA";
        case IpcEndpointKind::DeviceBuffer:
            return "device buffer";
        case IpcEndpointKind::HostBuffer:
            return "host buffer";
    }
    return "unknown";
}

bool IsImportedEndpoint(IpcEndpointKind endpoint)
{
    return endpoint == IpcEndpointKind::ImportedDeviceVa ||
           endpoint == IpcEndpointKind::ImportedHostVa;
}

IpcMemoryKind ImportedMemoryKind(IpcEndpointKind endpoint)
{
    return endpoint == IpcEndpointKind::ImportedHostVa
               ? IpcMemoryKind::HostPhysical
               : IpcMemoryKind::DevicePhysical;
}

MemorySide EndpointSide(IpcEndpointKind endpoint)
{
    return endpoint == IpcEndpointKind::ImportedDeviceVa ||
                   endpoint == IpcEndpointKind::DeviceBuffer
               ? MemorySide::Device
               : MemorySide::Host;
}

PhysicalMemoryConfig MakeConfigForKind(const Options& options,
                                       IpcMemoryKind memory_kind)
{
    if (memory_kind == IpcMemoryKind::HostPhysical) {
        return MakeHostConfig(options);
    }
    return MakeDeviceConfig(options);
}

PhysicalMemoryConfig MakeConfigForKind(const ShareMsg& share_msg,
                                       IpcMemoryKind memory_kind)
{
    Options options;
    options.device = share_msg.device;
    options.host_numa = share_msg.host_numa;
    options.requested_size = static_cast<size_t>(share_msg.test_size);
    return MakeConfigForKind(options, memory_kind);
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
                  const std::string& label_prefix)
{
    const std::string label = label_prefix + " aclrtMemcpy(" +
                              std::string(src.name) + " -> " + dst.name + ")";
    return CopyWithKind(dst.ptr, dst.size, src.ptr, copy_size, CopyKind(dst, src),
                        label);
}

bool VerifyIpcDirection(const std::vector<uint8_t>& data, uint32_t seed,
                        const std::string& direction)
{
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t expected =
            static_cast<uint8_t>((seed + (i * 131U) + (i >> 3U)) & 0xffU);
        if (data[i] != expected) {
            std::cerr << "  " << direction << " mismatch at offset=" << i
                      << ", expected=" << static_cast<int>(expected)
                      << ", actual=" << static_cast<int>(data[i]) << "\n";
            PrintRed("  " + direction + " ×");
            return false;
        }
    }
    PrintGreen("  " + direction + " √");
    return true;
}

std::string DirectionName(IpcEndpointKind src, IpcEndpointKind dst)
{
    return std::string(EndpointName(src)) + " -> " + EndpointName(dst);
}

bool EndpointUsesDeviceBuffer(IpcEndpointKind src, IpcEndpointKind dst)
{
    return src == IpcEndpointKind::DeviceBuffer ||
           dst == IpcEndpointKind::DeviceBuffer;
}

bool ValidImportedIndex(uint32_t index, const ShareMsg& share_msg)
{
    return index < share_msg.handle_count && index < kMaxSharedHandleCount;
}

uint32_t HandleIndexForEndpoint(const ShareMsg& share_msg,
                                IpcEndpointKind endpoint, bool is_src)
{
    if (!IsImportedEndpoint(endpoint)) {
        return kInvalidHandleIndex;
    }
    return is_src ? share_msg.src_handle_index : share_msg.dst_handle_index;
}

Endpoint ResolveChildEndpoint(IpcEndpointKind endpoint, uint32_t handle_index,
                              std::array<PhysicalMapping, kMaxSharedHandleCount>* mappings,
                              DeviceAllocation* device_buffer,
                              std::vector<uint8_t>* host_buffer)
{
    Endpoint resolved;
    resolved.name = EndpointName(endpoint);
    resolved.side = EndpointSide(endpoint);
    if (endpoint == IpcEndpointKind::DeviceBuffer) {
        resolved.ptr = device_buffer->ptr;
        resolved.size = device_buffer->size;
    } else if (endpoint == IpcEndpointKind::HostBuffer) {
        resolved.ptr = host_buffer->data();
        resolved.size = host_buffer->size();
    } else if (handle_index < kMaxSharedHandleCount) {
        resolved.ptr = (*mappings)[handle_index].virt;
        resolved.size = (*mappings)[handle_index].size;
    }
    return resolved;
}

bool AddImportedHandle(const Options& options, IpcEndpointKind endpoint,
                       ShareMsg* share_msg,
                       std::array<ParentHandle, kMaxSharedHandleCount>* handles,
                       uint32_t* handle_index)
{
    if (!IsImportedEndpoint(endpoint)) {
        *handle_index = kInvalidHandleIndex;
        return true;
    }
    if (share_msg->handle_count >= kMaxSharedHandleCount) {
        std::cerr << "  too many imported endpoints in IPC direction\n";
        return false;
    }

    const uint32_t index = share_msg->handle_count++;
    *handle_index = index;

    ParentHandle& handle = (*handles)[index];
    handle.memory_kind = ImportedMemoryKind(endpoint);
    handle.config = MakeConfigForKind(options, handle.memory_kind);
    if (!QueryAlignedSize(handle.config, options.requested_size,
                          &handle.aligned_size) ||
        !AllocateAndMapPhysical(handle.config, handle.aligned_size,
                                &handle.mapping)) {
        return false;
    }

    share_msg->handle_memory_kinds[index] = static_cast<uint32_t>(handle.memory_kind);
    share_msg->handle_aligned_sizes[index] = handle.aligned_size;
    if (share_msg->aligned_size == 0U) {
        share_msg->aligned_size = handle.aligned_size;
    }
    return true;
}

bool SetupImportedSource(const Options& options, const IpcDirection& direction,
                         ShareMsg* share_msg,
                         std::array<ParentHandle, kMaxSharedHandleCount>* handles)
{
    if (!IsImportedEndpoint(direction.src)) {
        return true;
    }
    if (!ValidImportedIndex(share_msg->src_handle_index, *share_msg)) {
        std::cerr << "  invalid source handle index for parent setup\n";
        return false;
    }

    ParentHandle& src = (*handles)[share_msg->src_handle_index];
    const auto pattern = MakePattern(options.requested_size, direction.seed);
    return CopyHostToMapping(
        src.mapping.virt, src.mapping.size, pattern, src.config,
        "parent setup aclrtMemcpy(host buffer -> " +
            std::string(EndpointName(direction.src)) + ")");
}

bool ExportParentHandles(const Options& options, int32_t child_bare_tgid,
                         ShareMsg* share_msg,
                         std::array<ParentHandle, kMaxSharedHandleCount>* handles)
{
    for (uint32_t i = 0; i < share_msg->handle_count; ++i) {
        if (!ExportShareableHandle(options, (*handles)[i].mapping.handle,
                                   child_bare_tgid, &share_msg->handles[i])) {
            return false;
        }
    }
    return true;
}

bool VerifyImportedDestination(const Options& options, const IpcDirection& direction,
                               const ShareMsg& share_msg,
                               std::array<ParentHandle, kMaxSharedHandleCount>* handles)
{
    if (!IsImportedEndpoint(direction.dst)) {
        return true;
    }
    if (!ValidImportedIndex(share_msg.dst_handle_index, share_msg)) {
        std::cerr << "  invalid destination handle index for parent readback\n";
        return false;
    }

    ParentHandle& dst = (*handles)[share_msg.dst_handle_index];
    std::vector<uint8_t> actual(options.requested_size);
    return CopyMappingToHost(
               &actual, dst.mapping.virt, actual.size(), dst.config,
               "parent readback aclrtMemcpy(" +
                   std::string(EndpointName(direction.dst)) + " -> host buffer)") &&
           VerifyIpcDirection(actual, direction.seed,
                              DirectionName(direction.src, direction.dst));
}

void CleanupParentHandles(std::array<ParentHandle, kMaxSharedHandleCount>* handles,
                          uint32_t handle_count)
{
    for (uint32_t i = 0; i < handle_count && i < kMaxSharedHandleCount; ++i) {
        (*handles)[i].mapping.Cleanup();
    }
}

bool WaitForChild(pid_t pid, bool ok)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << "  waitpid failed, errno=" << errno << "\n";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (ok) {
            std::cerr << "  child exited abnormally, status=" << status << "\n";
        } else {
            std::cerr << "  child exited with status=" << status << "\n";
        }
        return false;
    }
    return ok;
}

bool RunIpcCopyDirection(const Options& options, const IpcDirection& direction)
{
    (void)std::signal(SIGPIPE, SIG_IGN);
    const std::string direction_name =
        DirectionName(direction.src, direction.dst);

    int parent_to_child[2] = {-1, -1};
    int child_to_parent[2] = {-1, -1};
    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        std::cerr << "  pipe failed, errno=" << errno << "\n";
        CloseFd(&parent_to_child[0]);
        CloseFd(&parent_to_child[1]);
        CloseFd(&child_to_parent[0]);
        CloseFd(&child_to_parent[1]);
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "  fork failed, errno=" << errno << "\n";
        CloseFd(&parent_to_child[0]);
        CloseFd(&parent_to_child[1]);
        CloseFd(&child_to_parent[0]);
        CloseFd(&child_to_parent[1]);
        return false;
    }

    if (pid == 0) {
        CloseFd(&parent_to_child[1]);
        CloseFd(&child_to_parent[0]);
        const int child_ret = RunIpcChild(parent_to_child[0], child_to_parent[1]);
        CloseFd(&parent_to_child[0]);
        CloseFd(&child_to_parent[1]);
        std::_Exit(child_ret);
    }

    CloseFd(&parent_to_child[0]);
    CloseFd(&child_to_parent[1]);

    bool ok = true;
    ChildPidMsg pid_msg;
    if (!ReadFull(child_to_parent[0], &pid_msg, sizeof(pid_msg)) ||
        pid_msg.magic != kPidMagic) {
        std::cerr << "  parent failed to read child bare tgid\n";
        ok = false;
    } else {
        std::cout << "  child_os_pid=" << pid_msg.os_pid
                  << ", child_bare_tgid=" << pid_msg.bare_tgid << "\n";
    }

    AclRuntime runtime;
    ok = ok && runtime.Init() && runtime.SetDevice(options.device);

    ShareMsg share_msg;
    share_msg.device = options.device;
    share_msg.host_numa = options.host_numa;
    share_msg.test_size = options.requested_size;
    share_msg.parent_seed = direction.seed;
    share_msg.test_kind = static_cast<uint32_t>(IpcTestKind::CopyDirection);
    share_msg.src_endpoint = static_cast<uint32_t>(direction.src);
    share_msg.dst_endpoint = static_cast<uint32_t>(direction.dst);

    std::array<ParentHandle, kMaxSharedHandleCount> handles;
    if (ok) {
        ok = AddImportedHandle(options, direction.src, &share_msg, &handles,
                               &share_msg.src_handle_index) &&
             AddImportedHandle(options, direction.dst, &share_msg, &handles,
                               &share_msg.dst_handle_index) &&
             SetupImportedSource(options, direction, &share_msg, &handles) &&
             ExportParentHandles(options, pid_msg.bare_tgid, &share_msg, &handles);
    }

    if (ok) {
        ok = WriteFull(parent_to_child[1], &share_msg, sizeof(share_msg));
        if (!ok) {
            std::cerr << "  parent failed to send copy-direction share msg\n";
        }
    } else if (parent_to_child[1] >= 0) {
        SendStopMessage(parent_to_child[1]);
    }
    CloseFd(&parent_to_child[1]);

    ChildResult result;
    if (ReadFull(child_to_parent[0], &result, sizeof(result)) &&
        result.magic == kResultMagic) {
        std::cout << "  child_result ok=" << result.ok
                  << ", ret=" << result.ret
                  << ", message=\"" << result.message << "\"\n";
        ok = ok && result.ok == 1;
    } else {
        std::cerr << "  parent failed to read child result\n";
        ok = false;
    }
    CloseFd(&child_to_parent[0]);

    const bool verify_in_parent =
        ok && IsImportedEndpoint(direction.dst);
    ok = ok && VerifyImportedDestination(options, direction, share_msg, &handles);
    if (!ok && !verify_in_parent) {
        PrintRed("  " + direction_name + " ×");
    }

    CleanupParentHandles(&handles, share_msg.handle_count);
    return WaitForChild(pid, ok);
}

bool RunIpcEndpointPair(const Options& options, const std::string& section,
                        IpcEndpointKind left, IpcEndpointKind right,
                        uint32_t seed_base)
{
    std::cout << "\n";
    PrintRed(section);

    bool ok = RunIpcCopyDirection(options, {left, right, seed_base + 1U});
    ok = RunIpcCopyDirection(options, {right, left, seed_base + 2U}) && ok;
    return ok;
}

}  // namespace

int RunIpcCopyDirectionChild(int write_fd, const ShareMsg& share_msg)
{
    if (share_msg.test_kind != static_cast<uint32_t>(IpcTestKind::CopyDirection)) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "child expected copy-direction test");
        return 1;
    }
    if (share_msg.handle_count > kMaxSharedHandleCount) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "child received too many shared handles");
        return 1;
    }

    const auto src_kind = EndpointKindFromWire(share_msg.src_endpoint);
    const auto dst_kind = EndpointKindFromWire(share_msg.dst_endpoint);
    const std::string direction = DirectionName(src_kind, dst_kind);

    std::array<PhysicalMapping, kMaxSharedHandleCount> mappings;
    bool ok = true;
    aclError failure_ret = ACL_ERROR_RT_PARAM_INVALID;
    for (uint32_t i = 0; i < share_msg.handle_count; ++i) {
        if (!ok) {
            break;
        }
        const auto memory_kind = MemoryKindFromWire(share_msg.handle_memory_kinds[i]);
        const auto config = MakeConfigForKind(share_msg, memory_kind);
        ok = ImportAndMapSharedHandle(share_msg, i, config, &mappings[i],
                                      &failure_ret);
    }

    DeviceAllocation device_buffer;
    if (ok && EndpointUsesDeviceBuffer(src_kind, dst_kind)) {
        ok = device_buffer.Allocate(static_cast<size_t>(share_msg.test_size));
    }

    const size_t test_size = static_cast<size_t>(share_msg.test_size);
    std::vector<uint8_t> expected = MakePattern(test_size, share_msg.parent_seed);
    std::vector<uint8_t> actual(test_size, 0);

    const uint32_t src_handle_index =
        HandleIndexForEndpoint(share_msg, src_kind, true);
    const uint32_t dst_handle_index =
        HandleIndexForEndpoint(share_msg, dst_kind, false);
    if (ok && IsImportedEndpoint(src_kind) &&
        !ValidImportedIndex(src_handle_index, share_msg)) {
        std::cerr << "  child invalid source handle index\n";
        failure_ret = ACL_ERROR_RT_PARAM_INVALID;
        ok = false;
    }
    if (ok && IsImportedEndpoint(dst_kind) &&
        !ValidImportedIndex(dst_handle_index, share_msg)) {
        std::cerr << "  child invalid destination handle index\n";
        failure_ret = ACL_ERROR_RT_PARAM_INVALID;
        ok = false;
    }

    Endpoint src;
    Endpoint dst;
    if (ok) {
        src = ResolveChildEndpoint(src_kind, src_handle_index, &mappings,
                                   &device_buffer, &expected);
        dst = ResolveChildEndpoint(dst_kind, dst_handle_index, &mappings,
                                   &device_buffer, &actual);
    }

    if (ok && src_kind == IpcEndpointKind::DeviceBuffer) {
        Endpoint setup_src;
        setup_src.name = "host buffer";
        setup_src.ptr = expected.data();
        setup_src.size = expected.size();
        setup_src.side = MemorySide::Host;
        ok = CopyEndpoint(src, setup_src, expected.size(), "child setup");
    }

    if (ok) {
        ok = CopyEndpoint(dst, src, expected.size(), "child copy");
    }

    if (ok && dst_kind == IpcEndpointKind::DeviceBuffer) {
        Endpoint readback_dst;
        readback_dst.name = "host buffer";
        readback_dst.ptr = actual.data();
        readback_dst.size = actual.size();
        readback_dst.side = MemorySide::Host;
        ok = CopyEndpoint(readback_dst, dst, actual.size(), "child readback");
    }

    const bool verify_in_child = ok && !IsImportedEndpoint(dst_kind);
    if (verify_in_child) {
        ok = VerifyIpcDirection(actual, share_msg.parent_seed, direction);
    }
    if (!ok && !verify_in_child) {
        PrintRed("  " + direction + " ×");
    }

    device_buffer.Cleanup();
    for (uint32_t i = 0; i < share_msg.handle_count; ++i) {
        mappings[i].Cleanup();
    }

    const aclError result_ret =
        ok ? ACL_SUCCESS
           : (failure_ret == ACL_SUCCESS ? ACL_ERROR_RT_PARAM_INVALID : failure_ret);
    SendChildResult(write_fd, ok, result_ret,
                    ok ? "child copied direction" : "child copy direction failed");
    return ok ? 0 : 1;
}

bool RunDevicePhysicalIpcEndpointTests(const Options& options)
{
    bool ok = RunIpcEndpointPair(
        options, "[multi-process] imported device VA <-> device buffer",
        IpcEndpointKind::ImportedDeviceVa, IpcEndpointKind::DeviceBuffer, 0x2100U);
    ok = RunIpcEndpointPair(
             options, "[multi-process] imported device VA <-> host buffer",
             IpcEndpointKind::ImportedDeviceVa, IpcEndpointKind::HostBuffer,
             0x2200U) &&
         ok;
    ok = RunIpcEndpointPair(
             options, "[multi-process] imported device VA <-> imported device VA",
             IpcEndpointKind::ImportedDeviceVa, IpcEndpointKind::ImportedDeviceVa,
             0x2300U) &&
         ok;
    return ok;
}

bool RunHostPhysicalIpcEndpointTests(const Options& options)
{
    bool ok = RunIpcEndpointPair(
        options, "[multi-process] imported host VA <-> device buffer",
        IpcEndpointKind::ImportedHostVa, IpcEndpointKind::DeviceBuffer, 0x2400U);
    ok = RunIpcEndpointPair(
             options, "[multi-process] imported host VA <-> host buffer",
             IpcEndpointKind::ImportedHostVa, IpcEndpointKind::HostBuffer,
             0x2500U) &&
         ok;
    ok = RunIpcEndpointPair(
             options, "[multi-process] imported host VA <-> imported host VA",
             IpcEndpointKind::ImportedHostVa, IpcEndpointKind::ImportedHostVa,
             0x2600U) &&
         ok;
    return ok;
}

bool RunDeviceHostPhysicalIpcEndpointTest(const Options& options)
{
    return RunIpcEndpointPair(
        options, "[multi-process] imported device VA <-> imported host VA",
        IpcEndpointKind::ImportedDeviceVa, IpcEndpointKind::ImportedHostVa, 0x2700U);
}

}  // namespace acltest::internal
