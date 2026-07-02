#include "physical_memory_common.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef ACL_RT_VMM_EXPORT_FLAG_DEFAULT
#define ACL_RT_VMM_EXPORT_FLAG_DEFAULT 0x0UL
#endif

#ifndef ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION
#define ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION 0x1UL
#endif

#ifndef ACLTEST_HAS_FABRIC_SHARE_TYPE
#define ACLTEST_HAS_FABRIC_SHARE_TYPE 0
#endif

#ifndef ACLTEST_HAS_V2_SHARE_API
#define ACLTEST_HAS_V2_SHARE_API 0
#endif

namespace acltest {
namespace {

constexpr uint32_t kPidMagic = 0x50494431U;
constexpr uint32_t kShareMagic = 0x53485231U;
constexpr uint32_t kStopMagic = 0x53544f50U;
constexpr uint32_t kResultMagic = 0x52534c54U;
constexpr size_t kMaxSharedHandleBytes = 128;
constexpr size_t kMaxSharedHandleCount = 2;
constexpr aclrtMemAccessFlags kAclMemAccessReadWrite =
    static_cast<aclrtMemAccessFlags>(0x3UL);

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

struct PhysicalMemoryConfig {
    const char* name = "";
    aclrtPhysicalMemProp prop = {};
    aclrtMemLocation access_location = {};
    aclrtMemcpyKind host_to_mapping_kind = ACL_MEMCPY_HOST_TO_DEVICE;
    aclrtMemcpyKind mapping_to_host_kind = ACL_MEMCPY_DEVICE_TO_HOST;
    aclrtMemcpyKind mapping_to_mapping_kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
    const char* write_tag = "H2D";
    const char* read_tag = "D2H";
    const char* va_to_va_tag = "D2D";
};

std::string AclErrorHint(aclError ret)
{
    switch (ret) {
        case ACL_SUCCESS:
            return "ACL_SUCCESS";
        case ACL_ERROR_REPEAT_INITIALIZE:
            return "ACL_ERROR_REPEAT_INITIALIZE";
        case ACL_ERROR_INVALID_PARAM:
            return "ACL_ERROR_INVALID_PARAM";
        case ACL_ERROR_RT_PARAM_INVALID:
            return "ACL_ERROR_RT_PARAM_INVALID";
        case ACL_ERROR_RT_FEATURE_NOT_SUPPORT:
            return "ACL_ERROR_RT_FEATURE_NOT_SUPPORT";
        case ACL_ERROR_RT_NO_DEVICE:
            return "ACL_ERROR_RT_NO_DEVICE";
        default:
            return "unknown";
    }
}

std::string FormatAclRet(aclError ret)
{
    std::ostringstream os;
    os << static_cast<int64_t>(ret) << " (" << AclErrorHint(ret) << ")";
    const char* recent = aclGetRecentErrMsg();
    if (ret != ACL_SUCCESS && recent != nullptr && recent[0] != '\0') {
        os << ", recent=\"" << recent << "\"";
    }
    return os.str();
}

bool LogAcl(const std::string& label, aclError ret)
{
    std::cout << "  " << std::left << std::setw(42) << label
              << " ret=" << FormatAclRet(ret) << "\n";
    return ret == ACL_SUCCESS;
}

void PrintUsage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --device N                 Device id. Default: 0\n"
        << "  --host-numa N              Host NUMA id for host physical tests. Default: 0\n"
        << "  --size BYTES               Data bytes to verify. Default: 4096\n"
        << "  --disable-pid-validation   Export handle without whitelist validation.\n"
        << "  --use-v1                   Force V1 shareable-handle APIs.\n"
        << "  --share-type default       V2 AI Server local handle. Default.\n"
        << "  --share-type fabric        V2 fabric handle for cross AI Server capable systems.\n"
        << "  --help                     Show this help.\n";
}

bool ParseInt(const char* text, int* out)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 0);
    if (end == text || *end != '\0' || value < 0 ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool ParseSize(const char* text, size_t* out)
{
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0' || value == 0ULL) {
        return false;
    }
    *out = static_cast<size_t>(value);
    return true;
}

bool ParseArgs(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--device") {
            const char* value = require_value("--device");
            if (value == nullptr || !ParseInt(value, &options->device)) {
                return false;
            }
        } else if (arg == "--host-numa") {
            const char* value = require_value("--host-numa");
            if (value == nullptr || !ParseInt(value, &options->host_numa)) {
                return false;
            }
        } else if (arg == "--size") {
            const char* value = require_value("--size");
            if (value == nullptr || !ParseSize(value, &options->requested_size)) {
                return false;
            }
        } else if (arg == "--disable-pid-validation") {
            options->disable_pid_validation = true;
        } else if (arg == "--use-v1") {
            options->force_v1 = true;
        } else if (arg == "--share-type") {
            const char* value = require_value("--share-type");
            if (value == nullptr) {
                return false;
            }
            const std::string share_type = value;
            if (share_type == "default") {
                options->share_kind = ShareKind::Default;
            } else if (share_type == "fabric") {
                options->share_kind = ShareKind::Fabric;
            } else {
                std::cerr << "unknown --share-type: " << share_type << "\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

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

class AclRuntime {
public:
    bool Init()
    {
        const aclError init_ret = aclInit(nullptr);
        std::cout << "  " << std::left << std::setw(42) << "aclInit"
                  << " ret=" << FormatAclRet(init_ret) << "\n";
        if (init_ret != ACL_SUCCESS && init_ret != ACL_ERROR_REPEAT_INITIALIZE) {
            return false;
        }
        initialized_here_ = (init_ret == ACL_SUCCESS);
        return true;
    }

    bool SetDevice(int device)
    {
        const aclError ret = aclrtSetDevice(device);
        if (!LogAcl("aclrtSetDevice(" + std::to_string(device) + ")", ret)) {
            return false;
        }
        if (ret == ACL_SUCCESS && !device_set_) {
            device_ = device;
            device_set_ = true;
        }
        return true;
    }

    ~AclRuntime()
    {
        if (device_set_) {
            (void)aclrtResetDevice(device_);
        }
        if (initialized_here_) {
            (void)aclFinalize();
        }
    }

private:
    bool initialized_here_ = false;
    bool device_set_ = false;
    int device_ = 0;
};

aclrtPhysicalMemProp MakeDevicePhysicalMemProp(int device)
{
    aclrtPhysicalMemProp prop = {};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = ACL_HBM_MEM_HUGE;
    prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = device;
    prop.reserve = 0;
    return prop;
}

aclrtPhysicalMemProp MakeHostPhysicalMemProp(int host_numa)
{
    aclrtPhysicalMemProp prop = {};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
    prop.memAttr = ACL_DDR_MEM_HUGE;
    prop.location.type = ACL_MEM_LOCATION_TYPE_HOST_NUMA;
    prop.location.id = host_numa;
    prop.reserve = 0;
    return prop;
}

PhysicalMemoryConfig MakeDeviceConfig(const Options& options)
{
    PhysicalMemoryConfig config;
    config.name = "device physical memory";
    config.prop = MakeDevicePhysicalMemProp(options.device);
    config.access_location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    config.access_location.id = options.device;
    config.host_to_mapping_kind = ACL_MEMCPY_HOST_TO_DEVICE;
    config.mapping_to_host_kind = ACL_MEMCPY_DEVICE_TO_HOST;
    config.mapping_to_mapping_kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
    config.write_tag = "H2D";
    config.read_tag = "D2H";
    config.va_to_va_tag = "D2D";
    return config;
}

PhysicalMemoryConfig MakeHostConfig(const Options& options)
{
    PhysicalMemoryConfig config;
    config.name = "host physical memory";
    config.prop = MakeHostPhysicalMemProp(options.host_numa);
    config.access_location.type = ACL_MEM_LOCATION_TYPE_HOST;
    config.access_location.id = 0;
    config.host_to_mapping_kind = ACL_MEMCPY_HOST_TO_HOST;
    config.mapping_to_host_kind = ACL_MEMCPY_HOST_TO_HOST;
    config.mapping_to_mapping_kind = ACL_MEMCPY_HOST_TO_HOST;
    config.write_tag = "H2H";
    config.read_tag = "H2H";
    config.va_to_va_tag = "H2H";
    return config;
}

size_t AlignUp(size_t value, size_t alignment)
{
    if (alignment == 0U) {
        return value;
    }
    return ((value + alignment - 1U) / alignment) * alignment;
}

bool QueryAlignedSize(const aclrtPhysicalMemProp& prop, size_t requested, size_t* aligned)
{
    size_t minimum = 0;
    auto ret = aclrtMemGetAllocationGranularity(
        const_cast<aclrtPhysicalMemProp*>(&prop),
        ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM, &minimum);
    if (!LogAcl("aclrtMemGetAllocationGranularity(MIN)", ret)) {
        return false;
    }

    size_t recommended = 0;
    ret = aclrtMemGetAllocationGranularity(
        const_cast<aclrtPhysicalMemProp*>(&prop),
        ACL_RT_MEM_ALLOC_GRANULARITY_RECOMMENDED, &recommended);
    (void)LogAcl("aclrtMemGetAllocationGranularity(REC)", ret);

    if (minimum == 0U) {
        std::cerr << "  minimum granularity is zero\n";
        return false;
    }

    std::cout << "  minimum_granularity=" << minimum << "\n";
    *aligned = AlignUp(requested, minimum);
    std::cout << "  requested_size=" << requested
              << ", minimum_granularity=" << minimum
              << ", recommended_granularity=" << recommended
              << ", aligned_size=" << *aligned << "\n";
    return true;
}

std::vector<uint8_t> MakePattern(size_t size, uint32_t seed)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((seed + (i * 131U) + (i >> 3U)) & 0xffU);
    }
    return data;
}

bool VerifyPattern(const std::vector<uint8_t>& data, uint32_t seed,
                   const std::string& label)
{
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t expected =
            static_cast<uint8_t>((seed + (i * 131U) + (i >> 3U)) & 0xffU);
        if (data[i] != expected) {
            std::cerr << "  " << label << " mismatch at offset=" << i
                      << ", expected=" << static_cast<int>(expected)
                      << ", actual=" << static_cast<int>(data[i]) << "\n";
            return false;
        }
    }
    std::cout << "  " << label << " verified " << data.size() << " bytes\n";
    return true;
}

struct PhysicalMapping {
    aclrtDrvMemHandle handle = nullptr;
    void* virt = nullptr;
    size_t size = 0;
    bool mapped = false;
    bool reserved = false;
    bool owns_handle = false;

    bool ReserveMapAndSetAccess(aclrtDrvMemHandle input_handle, size_t map_size,
                                const aclrtMemLocation& access_location)
    {
        handle = input_handle;
        size = map_size;

        aclError ret = aclrtReserveMemAddress(&virt, size, 0, nullptr, 0);
        if (!LogAcl("aclrtReserveMemAddress", ret)) {
            return false;
        }
        reserved = true;
        std::cout << "  reserved virt=" << virt << "\n";

        ret = aclrtMapMem(virt, size, 0, handle, 0);
        if (!LogAcl("aclrtMapMem", ret)) {
            return false;
        }
        mapped = true;

        aclrtMemAccessDesc access = {};
        access.location = access_location;
        access.flags = kAclMemAccessReadWrite;
        ret = aclrtMemSetAccess(virt, size, &access, 1);
        if (!LogAcl("aclrtMemSetAccess(READWRITE)", ret)) {
            return false;
        }
        return true;
    }

    void Cleanup()
    {
        if (mapped) {
            (void)LogAcl("cleanup aclrtUnmapMem", aclrtUnmapMem(virt));
            mapped = false;
        }
        if (reserved) {
            (void)LogAcl("cleanup aclrtReleaseMemAddress", aclrtReleaseMemAddress(virt));
            reserved = false;
        }
        if (owns_handle && handle != nullptr) {
            (void)LogAcl("cleanup aclrtFreePhysical", aclrtFreePhysical(handle));
            handle = nullptr;
            owns_handle = false;
        }
    }
};

bool AllocateAndMapPhysical(const PhysicalMemoryConfig& config, size_t aligned_size,
                            PhysicalMapping* mapping)
{
    aclrtDrvMemHandle handle = nullptr;
    aclError ret = aclrtMallocPhysical(&handle, aligned_size, &config.prop, 0);
    if (!LogAcl("aclrtMallocPhysical", ret)) {
        return false;
    }
    mapping->owns_handle = true;
    return mapping->ReserveMapAndSetAccess(handle, aligned_size, config.access_location);
}

bool CopyHostToMapping(void* dst, size_t dst_max, const std::vector<uint8_t>& src,
                       const PhysicalMemoryConfig& config, const std::string& label)
{
    const aclError ret = aclrtMemcpy(dst, dst_max, src.data(), src.size(),
                                     config.host_to_mapping_kind);
    return LogAcl(label, ret);
}

bool CopyMappingToHost(std::vector<uint8_t>* dst, const void* src, size_t src_size,
                       const PhysicalMemoryConfig& config, const std::string& label)
{
    const aclError ret = aclrtMemcpy(dst->data(), dst->size(), src, src_size,
                                     config.mapping_to_host_kind);
    return LogAcl(label, ret);
}

bool CopyMappingToMapping(void* dst, size_t dst_max, const void* src, size_t copy_size,
                          const PhysicalMemoryConfig& config, const std::string& label)
{
    const aclError ret = aclrtMemcpy(dst, dst_max, src, copy_size,
                                     config.mapping_to_mapping_kind);
    return LogAcl(label, ret);
}

bool CopyWithKind(void* dst, size_t dst_max, const void* src, size_t copy_size,
                  aclrtMemcpyKind kind, const std::string& label)
{
    const aclError ret = aclrtMemcpy(dst, dst_max, src, copy_size, kind);
    return LogAcl(label, ret);
}

uint32_t ShareKindValue(const Options& options)
{
    return options.share_kind == ShareKind::Fabric ? 1U : 0U;
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
    return true;
}

bool ImportShareableHandle(const SharedHandleBlob& blob, int device,
                           aclrtDrvMemHandle* handle)
{
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
                return false;
            }
            std::memcpy(&default_handle, blob.share, sizeof(default_handle));
            share_ptr = &default_handle;
        } else {
#if ACLTEST_HAS_FABRIC_SHARE_TYPE
            if (blob.share_len != sizeof(fabric_handle)) {
                std::cerr << "  invalid V2 fabric handle size=" << blob.share_len << "\n";
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

        const aclError ret = aclrtMemImportFromShareableHandleV2(
            share_ptr, share_type, 0, handle);
        return LogAcl("aclrtMemImportFromShareableHandleV2", ret);
#else
        std::cerr << "  this CANN header does not define V2 shareable handle APIs\n";
        return false;
#endif
    }

    uint64_t shareable_handle = 0;
    if (blob.share_len != sizeof(shareable_handle)) {
        std::cerr << "  invalid V1 handle size=" << blob.share_len << "\n";
        return false;
    }
    std::memcpy(&shareable_handle, blob.share, sizeof(shareable_handle));
    const aclError ret = aclrtMemImportFromShareableHandle(
        shareable_handle, device, handle);
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

bool ImportAndMapSharedHandle(const ShareMsg& share_msg, size_t index,
                              const PhysicalMemoryConfig& config,
                              PhysicalMapping* mapping)
{
    if (index >= share_msg.handle_count || index >= kMaxSharedHandleCount) {
        std::cerr << "  invalid shared handle index=" << index
                  << ", handle_count=" << share_msg.handle_count << "\n";
        return false;
    }

    std::cout << "  import handle[" << index << "]\n";
    aclrtDrvMemHandle imported = nullptr;
    if (!ImportShareableHandle(share_msg.handles[index], share_msg.device, &imported)) {
        return false;
    }

    mapping->owns_handle = true;
    if (!mapping->ReserveMapAndSetAccess(imported, share_msg.aligned_size,
                                         config.access_location)) {
        mapping->Cleanup();
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
    if (!ImportAndMapSharedHandle(share_msg, 0, config, &mapping)) {
        mapping.Cleanup();
        SendChildResult(write_fd, false, ACL_ERROR_RT_PARAM_INVALID, "child map failed");
        return 1;
    }

    std::vector<uint8_t> actual(static_cast<size_t>(share_msg.test_size));
    bool ok = CopyMappingToHost(
                  &actual, mapping.virt, actual.size(), config,
                  "child aclrtMemcpy(" + std::string(config.read_tag) + " parent pattern)") &&
              VerifyPattern(actual, share_msg.parent_seed, "child sees parent pattern");

    if (ok) {
        const auto reply = MakePattern(actual.size(), share_msg.child_seed);
        ok = CopyHostToMapping(
            mapping.virt, mapping.size, reply, config,
            "child aclrtMemcpy(" + std::string(config.write_tag) + " reply pattern)");
    }

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
    bool ok = ImportAndMapSharedHandle(share_msg, 0, config, &src) &&
              ImportAndMapSharedHandle(share_msg, 1, config, &dst);

    std::vector<uint8_t> actual(static_cast<size_t>(share_msg.test_size));
    if (ok) {
        ok = CopyMappingToHost(
                 &actual, dst.virt, actual.size(), config,
                 "child aclrtMemcpy(" + std::string(config.read_tag) +
                     " dst after parent VA-to-VA)") &&
             VerifyPattern(actual, share_msg.parent_seed,
                           "child sees parent VA-to-VA pattern");
    }

    if (ok) {
        const auto reply = MakePattern(actual.size(), share_msg.child_seed);
        ok = CopyHostToMapping(
                 src.virt, src.size, reply, config,
                 "child aclrtMemcpy(" + std::string(config.write_tag) +
                     " VA-to-VA source)") &&
             CopyMappingToMapping(
                 dst.virt, dst.size, src.virt, reply.size(), config,
                 "child aclrtMemcpy(" + std::string(config.va_to_va_tag) +
                     " VA-to-VA reply)");
    }

    dst.Cleanup();
    src.Cleanup();
    SendChildResult(write_fd, ok, ok ? ACL_SUCCESS : ACL_ERROR_RT_PARAM_INVALID,
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
    ok = ok && QueryAlignedSize(config.prop, options.requested_size, &aligned_size);

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
    ok = ok && QueryAlignedSize(config.prop, options.requested_size, &aligned_size);

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

bool ValidateDevice(const Options& options)
{
    uint32_t device_count = 0;
    {
        AclRuntime runtime;
        if (!runtime.Init()) {
            return false;
        }
        const aclError ret = aclrtGetDeviceCount(&device_count);
        if (!LogAcl("aclrtGetDeviceCount", ret) || device_count == 0U) {
            return false;
        }
        std::cout << "  device_count=" << device_count << "\n";
    }

    if (options.device >= static_cast<int>(device_count)) {
        std::cerr << "  device id out of range\n";
        return false;
    }
    return true;
}

void PrintOptions(const Options& options)
{
    std::cout << "  device=" << options.device << "\n";
    std::cout << "  host_numa=" << options.host_numa << "\n";
    std::cout << "  requested_size=" << options.requested_size << "\n";
    std::cout << "  pid_validation="
              << (options.disable_pid_validation ? "disabled" : "enabled") << "\n";
    std::cout << "  requested_share_api="
              << (options.force_v1 ? "V1" : "V2 if available") << "\n";
    std::cout << "  share_type="
              << (options.share_kind == ShareKind::Fabric ? "fabric" : "default") << "\n";
    std::cout << "  build_has_v2_share_api="
              << (ACLTEST_HAS_V2_SHARE_API ? "yes" : "no") << "\n";
    std::cout << "  build_has_fabric_share_type="
              << (ACLTEST_HAS_FABRIC_SHARE_TYPE ? "yes" : "no") << "\n";
}

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
    bool ok = QueryAlignedSize(host_config.prop, options.requested_size,
                               &host_aligned_size);

    size_t device_aligned_size = 0;
    ok = ok && QueryAlignedSize(device_config.prop, options.requested_size,
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
    if (!QueryAlignedSize(config.prop, options.requested_size, &aligned_size)) {
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

bool RunDevicePhysicalIpcTest(const Options& options)
{
    return RunIpcTest(options, MakeDeviceConfig(options));
}

bool RunHostPhysicalIpcTest(const Options& options)
{
    return RunIpcTest(options, MakeHostConfig(options));
}

int RunTestProgram(int argc, char** argv, const char* title,
                   const TestCase* tests, size_t test_count)
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    std::cout << title << "\n";
    PrintOptions(options);

    if (!ValidateDevice(options)) {
        return 1;
    }

    bool ok = true;
    for (size_t i = 0; i < test_count; ++i) {
        std::cout << "\n== " << tests[i].name << " ==\n";
        ok = tests[i].run(options) && ok;
    }

    std::cout << "\nRESULT: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}

}  // namespace acltest
