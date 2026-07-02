#include "physical_memory_common.h"
#include "physical_memory_utils.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
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

namespace acltest {
using namespace internal;
namespace {

constexpr uint32_t kPidMagic = 0x50494431U;
constexpr uint32_t kShareMagic = 0x53485231U;
constexpr uint32_t kStopMagic = 0x53544f50U;
constexpr uint32_t kResultMagic = 0x52534c54U;
constexpr size_t kMaxSharedHandleBytes = 128;
constexpr size_t kMaxSharedHandleCount = 2;

enum class ShareApi : uint32_t {
    V1 = 1,
    V2 = 2,
};

struct ChildPidMsg {
    uint32_t magic = kPidMagic;
    int32_t bare_tgid = -1;
    int32_t os_pid = -1;
};

struct SharedHandleBlob {
    uint32_t api_version = 0;
    uint32_t share_type = 0;
    uint32_t share_len = 0;
    uint32_t reserved = 0;
    uint8_t share[kMaxSharedHandleBytes] = {};
};

struct ShareMsg {
    uint32_t magic = kShareMagic;
    uint32_t handle_count = 0;
    int32_t device = 0;
    uint32_t reserved = 0;
    uint64_t aligned_size = 0;
    uint64_t test_size = 0;
    uint32_t parent_seed = 0;
    uint32_t child_seed = 0;
    SharedHandleBlob handles[kMaxSharedHandleCount] = {};
};

struct ChildResult {
    uint32_t magic = kResultMagic;
    int32_t ok = 0;
    int32_t ret = 0;
    char message[192] = {};
};

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

void FillResult(ChildResult* result, bool ok, aclError ret, const std::string& message)
{
    result->magic = kResultMagic;
    result->ok = ok ? 1 : 0;
    result->ret = static_cast<int32_t>(ret);
    std::snprintf(result->message, sizeof(result->message), "%s", message.c_str());
}

void SendChildResult(int write_fd, bool ok, aclError ret, const std::string& message)
{
    ChildResult result;
    FillResult(&result, ok, ret, message);
    (void)WriteFull(write_fd, &result, sizeof(result));
}

bool IsHostPhysicalConfig(const PhysicalMemoryConfig& config)
{
    return config.access_location.type == ACL_MEM_LOCATION_TYPE_HOST;
}

bool AllocateChildDeviceMapping(const ShareMsg& share_msg, PhysicalMapping* device)
{
    Options options;
    options.device = share_msg.device;
    options.requested_size = static_cast<size_t>(share_msg.test_size);

    const auto device_config = MakeDeviceConfig(options);
    size_t aligned_size = 0;
    if (!QueryAlignedSize(device_config, options.requested_size, &aligned_size)) {
        return false;
    }
    return AllocateAndMapPhysical(device_config, aligned_size, device);
}

bool RunImportedHostToDeviceProbe(const ShareMsg& share_msg,
                                  const PhysicalMapping& host_mapping,
                                  uint32_t expected_seed,
                                  const std::string& label_prefix)
{
    std::cout << "  " << label_prefix
              << " imported host VA -> device VA H2D probe\n";

    Options options;
    options.device = share_msg.device;
    options.requested_size = static_cast<size_t>(share_msg.test_size);
    const auto device_config = MakeDeviceConfig(options);

    PhysicalMapping device;
    bool ok = AllocateChildDeviceMapping(share_msg, &device);
    std::vector<uint8_t> actual(options.requested_size);
    if (ok) {
        ok = CopyWithKind(
                 device.virt, device.size, host_mapping.virt, actual.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 label_prefix + " aclrtMemcpy(H2D imported host VA to device VA)") &&
             CopyMappingToHost(
                 &actual, device.virt, actual.size(), device_config,
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
              << " device VA -> imported host VA D2H probe\n";

    Options options;
    options.device = share_msg.device;
    options.requested_size = static_cast<size_t>(share_msg.test_size);
    const auto device_config = MakeDeviceConfig(options);

    PhysicalMapping device;
    bool ok = AllocateChildDeviceMapping(share_msg, &device);
    if (ok) {
        const auto expected = MakePattern(options.requested_size, seed);
        const auto clear = MakePattern(options.requested_size, seed ^ 0xffU);
        std::vector<uint8_t> actual(options.requested_size);

        ok = CopyHostToMapping(
                 device.virt, device.size, expected, device_config,
                 label_prefix + " aclrtMemcpy(H2D device VA source)") &&
             CopyWithKind(
                 host_mapping->virt, host_mapping->size, device.virt,
                 expected.size(), ACL_MEMCPY_DEVICE_TO_HOST,
                 label_prefix + " aclrtMemcpy(D2H device VA to imported host VA)") &&
             CopyHostToMapping(
                 device.virt, device.size, clear, device_config,
                 label_prefix + " aclrtMemcpy(H2D clear device VA)") &&
             CopyWithKind(
                 device.virt, device.size, host_mapping->virt, expected.size(),
                 ACL_MEMCPY_HOST_TO_DEVICE,
                 label_prefix + " aclrtMemcpy(H2D imported host VA roundtrip)") &&
             CopyMappingToHost(
                 &actual, device.virt, actual.size(), device_config,
                 label_prefix + " aclrtMemcpy(D2H imported host roundtrip result)") &&
             VerifyPattern(actual, seed,
                           label_prefix + " imported host D2H/H2D roundtrip pattern");
    }

    device.Cleanup();
    return ok;
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

int RunSingleMappingIpcChild(int write_fd, const ShareMsg& share_msg,
                             const PhysicalMemoryConfig& config)
{
    if (share_msg.handle_count != 1U) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "child expected one shared handle");
        return 1;
    }

    PhysicalMapping mapping;
    aclError failure_ret = ACL_ERROR_RT_PARAM_INVALID;
    if (!ImportAndMapSharedHandle(share_msg, 0, config, &mapping, &failure_ret)) {
        mapping.Cleanup();
        SendChildResult(write_fd, false, failure_ret, "child map failed");
        return 1;
    }

    bool host_to_device_ok = true;
    if (IsHostPhysicalConfig(config)) {
        host_to_device_ok = RunImportedHostToDeviceProbe(
            share_msg, mapping, share_msg.parent_seed,
            "child single mapping");
    }

    std::vector<uint8_t> actual(static_cast<size_t>(share_msg.test_size));
    const bool read_ok = CopyMappingToHost(
                             &actual, mapping.virt, actual.size(), config,
                             "child aclrtMemcpy(" + std::string(config.read_tag) +
                                 " parent pattern)") &&
                         VerifyPattern(actual, share_msg.parent_seed,
                                       "child sees parent pattern");

    bool device_to_host_ok = true;
    if (IsHostPhysicalConfig(config)) {
        device_to_host_ok = RunDeviceToImportedHostProbe(
            share_msg, &mapping, share_msg.child_seed,
            "child single mapping");
    }

    bool write_ok = false;
    if (read_ok) {
        const auto reply = MakePattern(actual.size(), share_msg.child_seed);
        write_ok = CopyHostToMapping(
            mapping.virt, mapping.size, reply, config,
            "child aclrtMemcpy(" + std::string(config.write_tag) + " reply pattern)");
    }

    const bool ok = read_ok && write_ok && host_to_device_ok && device_to_host_ok;
    mapping.Cleanup();
    SendChildResult(write_fd, ok, ok ? ACL_SUCCESS : ACL_ERROR_RT_PARAM_INVALID,
                    ok ? "child verified and wrote reply" : "child verification failed");
    return ok ? 0 : 1;
}

int RunVaToVaIpcChild(int write_fd, const ShareMsg& share_msg,
                      const PhysicalMemoryConfig& config)
{
    if (share_msg.handle_count != 2U) {
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID,
                        "child expected two shared handles");
        return 1;
    }

    PhysicalMapping src;
    PhysicalMapping dst;
    aclError failure_ret = ACL_ERROR_RT_PARAM_INVALID;
    bool ok = ImportAndMapSharedHandle(share_msg, 0, config, &src, &failure_ret) &&
              ImportAndMapSharedHandle(share_msg, 1, config, &dst, &failure_ret);
    if (ok) {
        failure_ret = ACL_ERROR_RT_PARAM_INVALID;
    }

    bool host_to_device_ok = true;
    if (ok && IsHostPhysicalConfig(config)) {
        host_to_device_ok = RunImportedHostToDeviceProbe(
            share_msg, dst, share_msg.parent_seed,
            "child VA-to-VA dst");
    }

    std::vector<uint8_t> actual(static_cast<size_t>(share_msg.test_size));
    bool read_ok = false;
    if (ok) {
        read_ok = CopyMappingToHost(
                      &actual, dst.virt, actual.size(), config,
                      "child aclrtMemcpy(" + std::string(config.read_tag) +
                          " dst after parent VA-to-VA)") &&
                  VerifyPattern(actual, share_msg.parent_seed,
                                "child sees parent VA-to-VA pattern");
    }

    bool device_to_host_ok = true;
    if (ok && IsHostPhysicalConfig(config)) {
        device_to_host_ok = RunDeviceToImportedHostProbe(
            share_msg, &src, share_msg.child_seed,
            "child VA-to-VA src");
    }

    bool reply_ok = false;
    if (read_ok) {
        const auto reply = MakePattern(actual.size(), share_msg.child_seed);
        reply_ok = CopyHostToMapping(
                       src.virt, src.size, reply, config,
                       "child aclrtMemcpy(" + std::string(config.write_tag) +
                           " VA-to-VA source)") &&
                   CopyMappingToMapping(
                       dst.virt, dst.size, src.virt, reply.size(), config,
                       "child aclrtMemcpy(" + std::string(config.va_to_va_tag) +
                           " VA-to-VA reply)");
    }

    ok = ok && read_ok && reply_ok && host_to_device_ok && device_to_host_ok;
    dst.Cleanup();
    src.Cleanup();
    SendChildResult(write_fd, ok, ok ? ACL_SUCCESS : failure_ret,
                    ok ? "child VA-to-VA verified and wrote reply"
                       : "child VA-to-VA verification failed");
    return ok ? 0 : 1;
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

void SendStopMessage(int fd)
{
    ShareMsg msg;
    msg.magic = kStopMagic;
    if (!WriteFull(fd, &msg, sizeof(msg))) {
        std::cerr << "  parent failed to send stop msg\n";
    }
}

bool RunSingleMappingIpcTest(const Options& options, const PhysicalMemoryConfig& config)
{
    std::cout << "\n[multi-process] " << config.name
              << " single mapping shareable handle IPC\n";
    (void)std::signal(SIGPIPE, SIG_IGN);

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
        const int child_ret = RunIpcChild(parent_to_child[0], child_to_parent[1], config);
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

    size_t aligned_size = 0;
    ok = ok && QueryAlignedSize(config, options.requested_size, &aligned_size);

    PhysicalMapping mapping;
    ok = ok && AllocateAndMapPhysical(config, aligned_size, &mapping);

    const uint32_t parent_seed = 0x51U;
    const uint32_t child_seed = 0xa7U;
    if (ok) {
        const auto parent_pattern = MakePattern(options.requested_size, parent_seed);
        ok = CopyHostToMapping(
            mapping.virt, mapping.size, parent_pattern, config,
            "parent aclrtMemcpy(" + std::string(config.write_tag) + " initial)");
    }

    ShareMsg share_msg;
    share_msg.handle_count = 1;
    share_msg.device = options.device;
    share_msg.aligned_size = aligned_size;
    share_msg.test_size = options.requested_size;
    share_msg.parent_seed = parent_seed;
    share_msg.child_seed = child_seed;
    if (ok) {
        ok = ExportShareableHandle(options, mapping.handle, pid_msg.bare_tgid,
                                   &share_msg.handles[0]);
    }

    if (ok) {
        ok = WriteFull(parent_to_child[1], &share_msg, sizeof(share_msg));
        if (!ok) {
            std::cerr << "  parent failed to send share msg\n";
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

    if (ok) {
        std::vector<uint8_t> actual(options.requested_size);
        ok = CopyMappingToHost(
                 &actual, mapping.virt, actual.size(), config,
                 "parent aclrtMemcpy(" + std::string(config.read_tag) + " child reply)") &&
             VerifyPattern(actual, child_seed, "parent sees child reply");
    }

    mapping.Cleanup();

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << "  waitpid failed, errno=" << errno << "\n";
        ok = false;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (ok) {
            std::cerr << "  child exited abnormally, status=" << status << "\n";
        } else {
            std::cerr << "  child exited with status=" << status << "\n";
        }
        ok = false;
    }
    return ok;
}

bool RunVaToVaIpcTest(const Options& options, const PhysicalMemoryConfig& config)
{
    std::cout << "\n[multi-process] " << config.name
              << " VA-to-VA shareable handle IPC\n";
    (void)std::signal(SIGPIPE, SIG_IGN);

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
        const int child_ret = RunIpcChild(parent_to_child[0], child_to_parent[1], config);
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

    size_t aligned_size = 0;
    ok = ok && QueryAlignedSize(config, options.requested_size, &aligned_size);

    PhysicalMapping src;
    PhysicalMapping dst;
    ok = ok && AllocateAndMapPhysical(config, aligned_size, &src);
    ok = ok && AllocateAndMapPhysical(config, aligned_size, &dst);

    const uint32_t parent_seed = 0x61U;
    const uint32_t child_seed = 0xb7U;
    if (ok) {
        const auto parent_pattern = MakePattern(options.requested_size, parent_seed);
        ok = CopyHostToMapping(
                 src.virt, src.size, parent_pattern, config,
                 "parent aclrtMemcpy(" + std::string(config.write_tag) +
                     " VA-to-VA source)") &&
             CopyMappingToMapping(
                 dst.virt, dst.size, src.virt, parent_pattern.size(), config,
                 "parent aclrtMemcpy(" + std::string(config.va_to_va_tag) +
                     " VA-to-VA initial)");
    }

    ShareMsg share_msg;
    share_msg.handle_count = 2;
    share_msg.device = options.device;
    share_msg.aligned_size = aligned_size;
    share_msg.test_size = options.requested_size;
    share_msg.parent_seed = parent_seed;
    share_msg.child_seed = child_seed;
    if (ok) {
        ok = ExportShareableHandle(options, src.handle, pid_msg.bare_tgid,
                                   &share_msg.handles[0]) &&
             ExportShareableHandle(options, dst.handle, pid_msg.bare_tgid,
                                   &share_msg.handles[1]);
    }

    if (ok) {
        ok = WriteFull(parent_to_child[1], &share_msg, sizeof(share_msg));
        if (!ok) {
            std::cerr << "  parent failed to send VA-to-VA share msg\n";
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

    if (ok) {
        std::vector<uint8_t> actual(options.requested_size);
        ok = CopyMappingToHost(
                 &actual, dst.virt, actual.size(), config,
                 "parent aclrtMemcpy(" + std::string(config.read_tag) +
                     " child VA-to-VA reply)") &&
             VerifyPattern(actual, child_seed, "parent sees child VA-to-VA reply");
    }

    dst.Cleanup();
    src.Cleanup();

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << "  waitpid failed, errno=" << errno << "\n";
        ok = false;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (ok) {
            std::cerr << "  child exited abnormally, status=" << status << "\n";
        } else {
            std::cerr << "  child exited with status=" << status << "\n";
        }
        ok = false;
    }
    return ok;
}

bool RunIpcTest(const Options& options, const PhysicalMemoryConfig& config)
{
    bool ok = RunSingleMappingIpcTest(options, config);
    ok = RunVaToVaIpcTest(options, config) && ok;
    return ok;
}

}  // namespace

bool RunDevicePhysicalIpcTest(const Options& options)
{
    return RunIpcTest(options, MakeDeviceConfig(options));
}

bool RunHostPhysicalIpcTest(const Options& options)
{
    return RunIpcTest(options, MakeHostConfig(options));
}

}  // namespace acltest
