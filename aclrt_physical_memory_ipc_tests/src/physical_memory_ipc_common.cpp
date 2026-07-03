#include "physical_memory_ipc_internal.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef ACL_RT_VMM_EXPORT_FLAG_DEFAULT
#define ACL_RT_VMM_EXPORT_FLAG_DEFAULT 0x0UL
#endif

#ifndef ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION
#define ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION 0x1UL
#endif

namespace acltest::internal {
namespace {

uint32_t ShareKindValue(const Options& options)
{
    return options.share_kind == ShareKind::Fabric ? 1U : 0U;
}

const char* BlobApiName(uint32_t api_version)
{
    if (api_version == static_cast<uint32_t>(ShareApi::V1)) {
        return "V1";
    }
    if (api_version == static_cast<uint32_t>(ShareApi::V2)) {
        return "V2";
    }
    return "unknown";
}

const char* BlobShareKindName(uint32_t share_type)
{
    return share_type == 0U ? "default" : "fabric";
}

uint64_t ReadU64Prefix(const uint8_t* data, size_t size)
{
    uint64_t value = 0;
    if (size >= sizeof(value)) {
        std::memcpy(&value, data, sizeof(value));
    }
    return value;
}

void PrintSharedHandleBlob(const char* label, const SharedHandleBlob& blob)
{
    std::cout << "  " << label
              << " api=" << BlobApiName(blob.api_version)
              << "(" << blob.api_version << ")"
              << ", share_kind=" << BlobShareKindName(blob.share_type)
              << "(" << blob.share_type << ")"
              << ", share_len=" << blob.share_len;
    if (blob.share_len >= sizeof(uint64_t)) {
        std::cout << ", u64_prefix=" << ReadU64Prefix(blob.share, blob.share_len);
    }
    std::cout << "\n";
}

const char* RuntimeShareTypeName(aclrtMemSharedHandleType share_type)
{
    if (share_type == ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT) {
        return "ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT";
    }
#if ACLTEST_HAS_FABRIC_SHARE_TYPE
    if (share_type == ACL_MEM_SHARE_HANDLE_TYPE_FABRIC) {
        return "ACL_MEM_SHARE_HANDLE_TYPE_FABRIC";
    }
#endif
    return "unknown";
}

bool ImportShareableHandle(const SharedHandleBlob& blob, int device,
                           aclrtDrvMemHandle* handle, aclError* failure_ret)
{
    if (failure_ret != nullptr) {
        *failure_ret = ACL_SUCCESS;
    }

    if (blob.api_version == static_cast<uint32_t>(ShareApi::V2)) {
#if ACLTEST_HAS_V2_SHARE_API
        aclrtMemSharedHandleType share_type = ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT;
        void* share_ptr = nullptr;
        uint64_t default_handle = 0;
#if ACLTEST_HAS_FABRIC_SHARE_TYPE
        aclrtMemFabricHandle fabric_handle = {};
#endif

        if (blob.share_type == 0U) {
            if (blob.share_len != sizeof(default_handle)) {
                std::cerr << "  invalid V2 default handle size=" << blob.share_len << "\n";
                if (failure_ret != nullptr) {
                    *failure_ret = ACL_ERROR_RT_PARAM_INVALID;
                }
                return false;
            }
            std::memcpy(&default_handle, blob.share, sizeof(default_handle));
            share_ptr = &default_handle;
        } else {
#if ACLTEST_HAS_FABRIC_SHARE_TYPE
            if (blob.share_len != sizeof(fabric_handle)) {
                std::cerr << "  invalid V2 fabric handle size=" << blob.share_len << "\n";
                if (failure_ret != nullptr) {
                    *failure_ret = ACL_ERROR_RT_PARAM_INVALID;
                }
                return false;
            }
            std::memcpy(&fabric_handle, blob.share, sizeof(fabric_handle));
            share_type = ACL_MEM_SHARE_HANDLE_TYPE_FABRIC;
            share_ptr = &fabric_handle;
#else
            std::cerr << "  this CANN header does not define fabric share handles\n";
            return false;
#endif
        }

        std::cout << "  import request api=V2"
                  << ", runtime_share_type=" << RuntimeShareTypeName(share_type)
                  << "(" << static_cast<int>(share_type) << ")"
                  << ", flags=0"
                  << ", current_device=" << device;
        if (blob.share_len >= sizeof(uint64_t)) {
            std::cout << ", u64_prefix=" << ReadU64Prefix(blob.share, blob.share_len);
        }
        std::cout << "\n";
        const aclError ret = aclrtMemImportFromShareableHandleV2(
            share_ptr, share_type, 0, handle);
        if (failure_ret != nullptr) {
            *failure_ret = ret;
        }
        return LogAcl("aclrtMemImportFromShareableHandleV2", ret);
#else
        std::cerr << "  this CANN header does not define V2 shareable handle APIs\n";
        if (failure_ret != nullptr) {
            *failure_ret = ACL_ERROR_RT_FEATURE_NOT_SUPPORT;
        }
        return false;
#endif
    }

    uint64_t shareable_handle = 0;
    if (blob.share_len != sizeof(shareable_handle)) {
        std::cerr << "  invalid V1 handle size=" << blob.share_len << "\n";
        if (failure_ret != nullptr) {
            *failure_ret = ACL_ERROR_RT_PARAM_INVALID;
        }
        return false;
    }
    std::memcpy(&shareable_handle, blob.share, sizeof(shareable_handle));
    std::cout << "  import request api=V1"
              << ", device=" << device
              << ", shareable_handle=" << shareable_handle << "\n";
    const aclError ret = aclrtMemImportFromShareableHandle(
        shareable_handle, device, handle);
    if (failure_ret != nullptr) {
        *failure_ret = ret;
    }
    return LogAcl("aclrtMemImportFromShareableHandle(V1)", ret);
}

void FillResult(ChildResult* result, bool ok, aclError ret,
                const std::string& message)
{
    result->magic = kResultMagic;
    result->ok = ok ? 1 : 0;
    result->ret = static_cast<int32_t>(ret);
    std::snprintf(result->message, sizeof(result->message), "%s", message.c_str());
}

struct DeviceAllocation {
    void* ptr = nullptr;
    size_t size = 0;
    bool allocated = false;

    bool Allocate(size_t bytes)
    {
        size = bytes;
        const aclError ret = aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_FIRST);
        allocated = (ret == ACL_SUCCESS);
        return LogAcl("aclrtMalloc(device ptr)", ret);
    }

    void Cleanup()
    {
        if (allocated && ptr != nullptr) {
            (void)LogAcl("cleanup aclrtFree(device ptr)", aclrtFree(ptr));
            ptr = nullptr;
            allocated = false;
        }
    }
};

}  // namespace

bool WriteFull(int fd, const void* data, size_t size)
{
    const auto* ptr = static_cast<const uint8_t*>(data);
    while (size != 0U) {
        const ssize_t written = write(fd, ptr, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        ptr += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

bool ReadFull(int fd, void* data, size_t size)
{
    auto* ptr = static_cast<uint8_t*>(data);
    while (size != 0U) {
        const ssize_t got = read(fd, ptr, size);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false;
        }
        ptr += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

void CloseFd(int* fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

void SendChildResult(int write_fd, bool ok, aclError ret,
                     const std::string& message)
{
    ChildResult result;
    FillResult(&result, ok, ret, message);
    (void)WriteFull(write_fd, &result, sizeof(result));
}

void SendStopMessage(int fd)
{
    ShareMsg msg;
    msg.magic = kStopMagic;
    if (!WriteFull(fd, &msg, sizeof(msg))) {
        std::cerr << "  parent failed to send stop msg\n";
    }
}

bool IsHostPhysicalConfig(const PhysicalMemoryConfig& config)
{
    return config.access_location.type == ACL_MEM_LOCATION_TYPE_HOST;
}

bool ExportShareableHandle(const Options& options, aclrtDrvMemHandle handle,
                           int32_t child_bare_tgid, SharedHandleBlob* blob)
{
    blob->share_type = ShareKindValue(options);
    const uint64_t flags = options.disable_pid_validation
                               ? ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION
                               : ACL_RT_VMM_EXPORT_FLAG_DEFAULT;

#if ACLTEST_HAS_V2_SHARE_API
    if (!options.force_v1) {
        blob->api_version = static_cast<uint32_t>(ShareApi::V2);
        aclrtMemSharedHandleType share_type = ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT;
        uint64_t default_handle = 0;
        void* share_ptr = &default_handle;
        size_t share_len = sizeof(default_handle);

#if ACLTEST_HAS_FABRIC_SHARE_TYPE
        aclrtMemFabricHandle fabric_handle = {};
        if (options.share_kind == ShareKind::Fabric) {
            share_type = ACL_MEM_SHARE_HANDLE_TYPE_FABRIC;
            share_ptr = &fabric_handle;
            share_len = sizeof(fabric_handle);
        }
#else
        if (options.share_kind == ShareKind::Fabric) {
            std::cerr << "  this CANN header does not define fabric share handles\n";
            return false;
        }
#endif

        std::cout << "  export request api=V2"
                  << ", runtime_share_type=" << RuntimeShareTypeName(share_type)
                  << "(" << static_cast<int>(share_type) << ")"
                  << ", flags=" << flags
                  << ", child_bare_tgid=" << child_bare_tgid << "\n";
        aclError ret = aclrtMemExportToShareableHandleV2(handle, flags, share_type, share_ptr);
        if (!LogAcl("aclrtMemExportToShareableHandleV2", ret)) {
            return false;
        }

        if (!options.disable_pid_validation) {
            ret = aclrtMemSetPidToShareableHandleV2(
                share_ptr, share_type, &child_bare_tgid, 1);
            if (!LogAcl("aclrtMemSetPidToShareableHandleV2", ret)) {
                return false;
            }
        }

        blob->share_len = static_cast<uint32_t>(share_len);
        std::memcpy(blob->share, share_ptr, share_len);
        PrintSharedHandleBlob("exported share blob", *blob);
        return true;
    }
#else
    if (!options.force_v1 && options.share_kind == ShareKind::Fabric) {
        std::cerr << "  this CANN header does not define V2 shareable handle APIs\n";
        return false;
    }
#endif

    if (options.share_kind == ShareKind::Fabric) {
        std::cerr << "  --share-type fabric requires V2 APIs\n";
        return false;
    }

    blob->api_version = static_cast<uint32_t>(ShareApi::V1);
    uint64_t shareable_handle = 0;
    std::cout << "  export request api=V1"
              << ", handle_type=ACL_MEM_HANDLE_TYPE_NONE"
              << ", flags=" << flags
              << ", child_bare_tgid=" << child_bare_tgid << "\n";
    aclError ret = aclrtMemExportToShareableHandle(
        handle, ACL_MEM_HANDLE_TYPE_NONE, flags, &shareable_handle);
    if (!LogAcl("aclrtMemExportToShareableHandle(V1)", ret)) {
        return false;
    }

    if (!options.disable_pid_validation) {
        ret = aclrtMemSetPidToShareableHandle(shareable_handle, &child_bare_tgid, 1);
        if (!LogAcl("aclrtMemSetPidToShareableHandle(V1)", ret)) {
            return false;
        }
    }

    blob->share_len = sizeof(shareable_handle);
    std::memcpy(blob->share, &shareable_handle, sizeof(shareable_handle));
    PrintSharedHandleBlob("exported share blob", *blob);
    return true;
}

bool ImportAndMapSharedHandle(const ShareMsg& share_msg, size_t index,
                              const PhysicalMemoryConfig& config,
                              PhysicalMapping* mapping, aclError* failure_ret)
{
    if (failure_ret != nullptr) {
        *failure_ret = ACL_ERROR_RT_PARAM_INVALID;
    }
    if (index >= share_msg.handle_count || index >= kMaxSharedHandleCount) {
        std::cerr << "  invalid shared handle index=" << index
                  << ", handle_count=" << share_msg.handle_count << "\n";
        return false;
    }

    std::cout << "  import handle[" << index << "] for " << config.name
              << ", share_msg_device=" << share_msg.device
              << ", aligned_size=" << share_msg.aligned_size
              << ", test_size=" << share_msg.test_size << "\n";
    PrintSharedHandleBlob("received share blob", share_msg.handles[index]);
    aclrtDrvMemHandle imported = nullptr;
    if (!ImportShareableHandle(share_msg.handles[index], share_msg.device, &imported,
                               failure_ret)) {
        return false;
    }

    mapping->owns_handle = true;
    if (!mapping->ReserveMapAndSetAccess(imported, share_msg.aligned_size,
                                         config.access_location)) {
        mapping->Cleanup();
        if (failure_ret != nullptr) {
            *failure_ret = ACL_ERROR_RT_PARAM_INVALID;
        }
        return false;
    }
    return true;
}

bool RunImportedHostToDeviceProbe(const ShareMsg& share_msg,
                                  const PhysicalMapping& host_mapping,
                                  uint32_t expected_seed,
                                  const std::string& label_prefix)
{
    std::cout << "  " << label_prefix
              << " imported host VA -> device ptr H2D probe\n";

    Options options;
    options.device = share_msg.device;
    options.requested_size = static_cast<size_t>(share_msg.test_size);

    DeviceAllocation device;
    bool ok = device.Allocate(options.requested_size);
    std::vector<uint8_t> actual(options.requested_size);
    if (ok) {
        ok = CopyWithKind(
                 device.ptr, device.size, host_mapping.virt, actual.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 label_prefix + " aclrtMemcpy(H2D imported host VA to device ptr)") &&
             CopyWithKind(
                 actual.data(), actual.size(), device.ptr, actual.size(),
                 ACL_MEMCPY_DEVICE_TO_HOST,
                 label_prefix + " aclrtMemcpy(D2H imported host H2D result)") &&
             VerifyPattern(actual, expected_seed,
                           label_prefix + " imported host H2D pattern");
    }

    device.Cleanup();
    return ok;
}

bool RunDeviceToImportedHostProbe(const ShareMsg& share_msg,
                                  PhysicalMapping* host_mapping,
                                  uint32_t seed,
                                  const std::string& label_prefix)
{
    std::cout << "  " << label_prefix
              << " device ptr -> imported host VA D2H probe\n";

    Options options;
    options.device = share_msg.device;
    options.requested_size = static_cast<size_t>(share_msg.test_size);

    DeviceAllocation device;
    bool ok = device.Allocate(options.requested_size);
    if (ok) {
        const auto expected = MakePattern(options.requested_size, seed);
        const auto clear = MakePattern(options.requested_size, seed ^ 0xffU);
        std::vector<uint8_t> actual(options.requested_size);

        ok = CopyWithKind(
                 device.ptr, device.size, expected.data(), expected.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 label_prefix + " aclrtMemcpy(H2D device ptr source)") &&
             CopyWithKind(
                 host_mapping->virt, host_mapping->size, device.ptr,
                 expected.size(), ACL_MEMCPY_DEVICE_TO_HOST,
                 label_prefix + " aclrtMemcpy(D2H device ptr to imported host VA)") &&
             CopyWithKind(
                 device.ptr, device.size, clear.data(), clear.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 label_prefix + " aclrtMemcpy(H2D clear device ptr)") &&
             CopyWithKind(
                 device.ptr, device.size, host_mapping->virt, expected.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 label_prefix + " aclrtMemcpy(H2D imported host VA roundtrip)") &&
             CopyWithKind(
                 actual.data(), actual.size(), device.ptr, actual.size(),
                 ACL_MEMCPY_DEVICE_TO_HOST,
                 label_prefix + " aclrtMemcpy(D2H imported host roundtrip result)") &&
             VerifyPattern(actual, seed,
                           label_prefix + " imported host D2H/H2D roundtrip pattern");
    }

    device.Cleanup();
    return ok;
}

int RunIpcChild(int read_fd, int write_fd, const PhysicalMemoryConfig& config)
{
    std::cout << "\n[child] started, os_pid=" << getpid() << "\n";

    AclRuntime runtime;
    if (!runtime.Init()) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID, "child aclInit failed");
        return 1;
    }

    int32_t bare_tgid = -1;
    aclError ret = aclrtDeviceGetBareTgid(&bare_tgid);
    if (!LogAcl("aclrtDeviceGetBareTgid(child)", ret)) {
        SendChildResult(write_fd, false, ret, "child aclrtDeviceGetBareTgid failed");
        return 1;
    }

    ChildPidMsg pid_msg;
    pid_msg.bare_tgid = bare_tgid;
    pid_msg.os_pid = static_cast<int32_t>(getpid());
    if (!WriteFull(write_fd, &pid_msg, sizeof(pid_msg))) {
        std::cerr << "[child] failed to send bare tgid\n";
        return 1;
    }

    ShareMsg share_msg;
    if (!ReadFull(read_fd, &share_msg, sizeof(share_msg))) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "child failed to read share msg");
        return 1;
    }
    if (share_msg.magic == kStopMagic) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "parent setup failed before sharing handle");
        return 1;
    }
    if (share_msg.magic != kShareMagic) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "child received invalid share msg");
        return 1;
    }

    if (!runtime.SetDevice(share_msg.device)) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID, "child set device failed");
        return 1;
    }

    if (share_msg.handle_count == 1U) {
        return RunSingleMappingIpcChild(write_fd, share_msg, config);
    }
    if (share_msg.handle_count == 2U) {
        return RunVaToVaIpcChild(write_fd, share_msg, config);
    }

    SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                    "child received unsupported handle count");
    return 1;
}

}  // namespace acltest::internal
