#include <acl/acl.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
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

#ifndef ACL_RT_MEM_ACCESS_FLAGS_READWRITE
#define ACL_RT_MEM_ACCESS_FLAGS_READWRITE 0x3UL
#endif

#ifndef ACL_HBM_MEM_NORMAL
#define ACL_HBM_MEM_NORMAL ACL_MEM_NORMAL
#endif

namespace {

constexpr uint32_t kPidMagic = 0x50494431U;
constexpr uint32_t kShareMagic = 0x53485231U;
constexpr uint32_t kResultMagic = 0x52534c54U;
constexpr size_t kMaxSharedHandleBytes = 128;

enum class ShareApi : uint32_t {
    V1 = 1,
    V2 = 2,
};

enum class ShareKind {
    Default,
    Fabric,
};

struct Options {
    int device = 0;
    size_t requested_size = 4096;
    bool disable_pid_validation = false;
    bool force_v1 = false;
    ShareKind share_kind = ShareKind::Default;
};

struct ChildPidMsg {
    uint32_t magic = kPidMagic;
    int32_t bare_tgid = -1;
    int32_t os_pid = -1;
};

struct ShareMsg {
    uint32_t magic = kShareMagic;
    uint32_t api_version = 0;
    uint32_t share_type = 0;
    uint32_t share_len = 0;
    int32_t device = 0;
    uint64_t aligned_size = 0;
    uint64_t test_size = 0;
    uint32_t parent_seed = 0;
    uint32_t child_seed = 0;
    uint8_t share[kMaxSharedHandleBytes] = {};
};

struct ChildResult {
    uint32_t magic = kResultMagic;
    int32_t ok = 0;
    int32_t ret = 0;
    char message[192] = {};
};

std::string AclErrorHint(aclError ret)
{
    switch (ret) {
        case ACL_SUCCESS:
            return "ACL_SUCCESS";
        case ACL_ERROR_REPEAT_INITIALIZE:
            return "ACL_ERROR_REPEAT_INITIALIZE";
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

void CloseFd(int fd)
{
    if (fd >= 0) {
        (void)close(fd);
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
    prop.memAttr = ACL_HBM_MEM_NORMAL;
    prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = device;
    prop.reserve = 0;
    return prop;
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

bool VerifyPattern(const std::vector<uint8_t>& data, uint32_t seed, const std::string& label)
{
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t expected = static_cast<uint8_t>((seed + (i * 131U) + (i >> 3U)) & 0xffU);
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

    bool ReserveMapAndSetAccess(aclrtDrvMemHandle input_handle, size_t map_size, int device)
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
        access.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        access.location.id = device;
        access.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
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

bool AllocateAndMapPhysical(int device, size_t aligned_size, PhysicalMapping* mapping)
{
    const auto prop = MakeDevicePhysicalMemProp(device);
    aclrtDrvMemHandle handle = nullptr;
    aclError ret = aclrtMallocPhysical(&handle, aligned_size, &prop, 0);
    if (!LogAcl("aclrtMallocPhysical", ret)) {
        return false;
    }
    mapping->owns_handle = true;
    return mapping->ReserveMapAndSetAccess(handle, aligned_size, device);
}

bool CopyHostToDevice(void* dst, size_t dst_max, const std::vector<uint8_t>& src,
                      const std::string& label)
{
    const aclError ret = aclrtMemcpy(dst, dst_max, src.data(), src.size(),
                                     ACL_MEMCPY_HOST_TO_DEVICE);
    return LogAcl(label, ret);
}

bool CopyDeviceToHost(std::vector<uint8_t>* dst, const void* src, size_t src_size,
                      const std::string& label)
{
    const aclError ret = aclrtMemcpy(dst->data(), dst->size(), src, src_size,
                                     ACL_MEMCPY_DEVICE_TO_HOST);
    return LogAcl(label, ret);
}

uint32_t ShareKindValue(const Options& options)
{
    return options.share_kind == ShareKind::Fabric ? 1U : 0U;
}

bool ExportShareableHandle(const Options& options, aclrtDrvMemHandle handle,
                           int32_t child_bare_tgid, ShareMsg* msg)
{
    msg->device = options.device;
    msg->share_type = ShareKindValue(options);
    const uint64_t flags = options.disable_pid_validation
                               ? ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION
                               : ACL_RT_VMM_EXPORT_FLAG_DEFAULT;

#if defined(ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT)
    if (!options.force_v1) {
        msg->api_version = static_cast<uint32_t>(ShareApi::V2);
        aclrtMemSharedHandleType share_type = ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT;
        uint64_t default_handle = 0;
        void* share_ptr = &default_handle;
        size_t share_len = sizeof(default_handle);

#if defined(ACL_MEM_SHARE_HANDLE_TYPE_FABRIC)
        aclrtMemFabricHandle fabric_handle = {};
        if (options.share_kind == ShareKind::Fabric) {
            share_type = ACL_MEM_SHARE_HANDLE_TYPE_FABRIC;
            share_ptr = &fabric_handle;
            share_len = sizeof(fabric_handle);
        }
#else
        if (options.share_kind == ShareKind::Fabric) {
            std::cerr << "  this CANN header does not define ACL_MEM_SHARE_HANDLE_TYPE_FABRIC\n";
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

        msg->share_len = static_cast<uint32_t>(share_len);
        std::memcpy(msg->share, share_ptr, share_len);
        return true;
    }
#endif

    if (options.share_kind == ShareKind::Fabric) {
        std::cerr << "  --share-type fabric requires V2 APIs\n";
        return false;
    }

    msg->api_version = static_cast<uint32_t>(ShareApi::V1);
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

    msg->share_len = sizeof(shareable_handle);
    std::memcpy(msg->share, &shareable_handle, sizeof(shareable_handle));
    return true;
}

bool ImportShareableHandle(const ShareMsg& msg, aclrtDrvMemHandle* handle)
{
    if (msg.api_version == static_cast<uint32_t>(ShareApi::V2)) {
#if defined(ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT)
        aclrtMemSharedHandleType share_type = ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT;
        void* share_ptr = nullptr;
        uint64_t default_handle = 0;
#if defined(ACL_MEM_SHARE_HANDLE_TYPE_FABRIC)
        aclrtMemFabricHandle fabric_handle = {};
#endif

        if (msg.share_type == 0U) {
            if (msg.share_len != sizeof(default_handle)) {
                std::cerr << "  invalid V2 default handle size=" << msg.share_len << "\n";
                return false;
            }
            std::memcpy(&default_handle, msg.share, sizeof(default_handle));
            share_ptr = &default_handle;
        } else {
#if defined(ACL_MEM_SHARE_HANDLE_TYPE_FABRIC)
            if (msg.share_len != sizeof(fabric_handle)) {
                std::cerr << "  invalid V2 fabric handle size=" << msg.share_len << "\n";
                return false;
            }
            std::memcpy(&fabric_handle, msg.share, sizeof(fabric_handle));
            share_type = ACL_MEM_SHARE_HANDLE_TYPE_FABRIC;
            share_ptr = &fabric_handle;
#else
            std::cerr << "  this CANN header does not define ACL_MEM_SHARE_HANDLE_TYPE_FABRIC\n";
            return false;
#endif
        }

        const aclError ret = aclrtMemImportFromShareableHandleV2(
            share_ptr, share_type, 0, handle);
        return LogAcl("aclrtMemImportFromShareableHandleV2", ret);
#else
        std::cerr << "  received V2 handle, but this build lacks V2 definitions\n";
        return false;
#endif
    }

    uint64_t shareable_handle = 0;
    if (msg.share_len != sizeof(shareable_handle)) {
        std::cerr << "  invalid V1 handle size=" << msg.share_len << "\n";
        return false;
    }
    std::memcpy(&shareable_handle, msg.share, sizeof(shareable_handle));
    const aclError ret = aclrtMemImportFromShareableHandle(
        shareable_handle, msg.device, handle);
    return LogAcl("aclrtMemImportFromShareableHandle(V1)", ret);
}

void FillResult(ChildResult* result, bool ok, aclError ret, const std::string& message)
{
    result->magic = kResultMagic;
    result->ok = ok ? 1 : 0;
    result->ret = static_cast<int32_t>(ret);
    std::snprintf(result->message, sizeof(result->message), "%s", message.c_str());
}

int RunChild(int read_fd, int write_fd)
{
    std::cout << "\n[child] started, os_pid=" << getpid() << "\n";

    AclRuntime runtime;
    if (!runtime.Init()) {
        ChildResult result;
        FillResult(&result, false, ACL_ERROR_RT_PARAM_INVALID, "child aclInit failed");
        (void)WriteFull(write_fd, &result, sizeof(result));
        return 1;
    }

    int32_t bare_tgid = -1;
    aclError ret = aclrtDeviceGetBareTgid(&bare_tgid);
    if (!LogAcl("aclrtDeviceGetBareTgid(child)", ret)) {
        ChildResult result;
        FillResult(&result, false, ret, "child aclrtDeviceGetBareTgid failed");
        (void)WriteFull(write_fd, &result, sizeof(result));
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
    if (!ReadFull(read_fd, &share_msg, sizeof(share_msg)) || share_msg.magic != kShareMagic) {
        ChildResult result;
        FillResult(&result, false, ACL_ERROR_RT_PARAM_INVALID, "child failed to read share msg");
        (void)WriteFull(write_fd, &result, sizeof(result));
        return 1;
    }

    if (!runtime.SetDevice(share_msg.device)) {
        ChildResult result;
        FillResult(&result, false, ACL_ERROR_RT_PARAM_INVALID, "child set device failed");
        (void)WriteFull(write_fd, &result, sizeof(result));
        return 1;
    }

    aclrtDrvMemHandle imported = nullptr;
    if (!ImportShareableHandle(share_msg, &imported)) {
        ChildResult result;
        FillResult(&result, false, ACL_ERROR_RT_PARAM_INVALID, "child import failed");
        (void)WriteFull(write_fd, &result, sizeof(result));
        return 1;
    }

    PhysicalMapping mapping;
    mapping.owns_handle = true;
    if (!mapping.ReserveMapAndSetAccess(imported, share_msg.aligned_size, share_msg.device)) {
        mapping.Cleanup();
        ChildResult result;
        FillResult(&result, false, ACL_ERROR_RT_PARAM_INVALID, "child map failed");
        (void)WriteFull(write_fd, &result, sizeof(result));
        return 1;
    }

    std::vector<uint8_t> actual(static_cast<size_t>(share_msg.test_size));
    bool ok = CopyDeviceToHost(&actual, mapping.virt, actual.size(),
                               "child aclrtMemcpy(D2H parent pattern)") &&
              VerifyPattern(actual, share_msg.parent_seed, "child sees parent pattern");

    if (ok) {
        const auto reply = MakePattern(actual.size(), share_msg.child_seed);
        ok = CopyHostToDevice(mapping.virt, mapping.size, reply,
                              "child aclrtMemcpy(H2D reply pattern)");
    }

    mapping.Cleanup();

    ChildResult result;
    FillResult(&result, ok, ok ? ACL_SUCCESS : ACL_ERROR_RT_PARAM_INVALID,
               ok ? "child verified and wrote reply" : "child verification failed");
    (void)WriteFull(write_fd, &result, sizeof(result));
    return ok ? 0 : 1;
}

bool RunSingleProcessVmmTest(const Options& options)
{
    std::cout << "\n[single-process] physical allocation + virtual mapping\n";
    AclRuntime runtime;
    if (!runtime.Init() || !runtime.SetDevice(options.device)) {
        return false;
    }

    const auto prop = MakeDevicePhysicalMemProp(options.device);
    size_t aligned_size = 0;
    if (!QueryAlignedSize(prop, options.requested_size, &aligned_size)) {
        return false;
    }

    PhysicalMapping mapping;
    if (!AllocateAndMapPhysical(options.device, aligned_size, &mapping)) {
        mapping.Cleanup();
        return false;
    }

    const uint32_t seed = 0x31U;
    const auto expected = MakePattern(options.requested_size, seed);
    std::vector<uint8_t> actual(options.requested_size);
    const bool ok = CopyHostToDevice(mapping.virt, mapping.size, expected,
                                     "aclrtMemcpy(H2D single)") &&
                    CopyDeviceToHost(&actual, mapping.virt, actual.size(),
                                     "aclrtMemcpy(D2H single)") &&
                    VerifyPattern(actual, seed, "single-process pattern");
    mapping.Cleanup();
    return ok;
}

bool RunMultiProcessIpcTest(const Options& options)
{
    std::cout << "\n[multi-process] physical memory shareable handle IPC\n";

    int parent_to_child[2] = {-1, -1};
    int child_to_parent[2] = {-1, -1};
    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        std::cerr << "  pipe failed, errno=" << errno << "\n";
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "  fork failed, errno=" << errno << "\n";
        CloseFd(parent_to_child[0]);
        CloseFd(parent_to_child[1]);
        CloseFd(child_to_parent[0]);
        CloseFd(child_to_parent[1]);
        return false;
    }

    if (pid == 0) {
        CloseFd(parent_to_child[1]);
        CloseFd(child_to_parent[0]);
        const int child_ret = RunChild(parent_to_child[0], child_to_parent[1]);
        CloseFd(parent_to_child[0]);
        CloseFd(child_to_parent[1]);
        std::_Exit(child_ret);
    }

    CloseFd(parent_to_child[0]);
    CloseFd(child_to_parent[1]);

    ChildPidMsg pid_msg;
    if (!ReadFull(child_to_parent[0], &pid_msg, sizeof(pid_msg)) ||
        pid_msg.magic != kPidMagic) {
        std::cerr << "  parent failed to read child bare tgid\n";
        return false;
    }
    std::cout << "  child_os_pid=" << pid_msg.os_pid
              << ", child_bare_tgid=" << pid_msg.bare_tgid << "\n";

    AclRuntime runtime;
    bool ok = runtime.Init() && runtime.SetDevice(options.device);

    size_t aligned_size = 0;
    const auto prop = MakeDevicePhysicalMemProp(options.device);
    ok = ok && QueryAlignedSize(prop, options.requested_size, &aligned_size);

    PhysicalMapping mapping;
    ok = ok && AllocateAndMapPhysical(options.device, aligned_size, &mapping);

    const uint32_t parent_seed = 0x51U;
    const uint32_t child_seed = 0xa7U;
    if (ok) {
        const auto parent_pattern = MakePattern(options.requested_size, parent_seed);
        ok = CopyHostToDevice(mapping.virt, mapping.size, parent_pattern,
                              "parent aclrtMemcpy(H2D initial)");
    }

    ShareMsg share_msg;
    share_msg.aligned_size = aligned_size;
    share_msg.test_size = options.requested_size;
    share_msg.parent_seed = parent_seed;
    share_msg.child_seed = child_seed;
    if (ok) {
        ok = ExportShareableHandle(options, mapping.handle, pid_msg.bare_tgid, &share_msg);
    }

    if (ok) {
        ok = WriteFull(parent_to_child[1], &share_msg, sizeof(share_msg));
        if (!ok) {
            std::cerr << "  parent failed to send share msg\n";
        }
    }
    CloseFd(parent_to_child[1]);

    ChildResult result;
    if (ok) {
        ok = ReadFull(child_to_parent[0], &result, sizeof(result)) &&
             result.magic == kResultMagic && result.ok == 1;
        std::cout << "  child_result ok=" << result.ok
                  << ", ret=" << result.ret
                  << ", message=\"" << result.message << "\"\n";
    }
    CloseFd(child_to_parent[0]);

    if (ok) {
        std::vector<uint8_t> actual(options.requested_size);
        ok = CopyDeviceToHost(&actual, mapping.virt, actual.size(),
                              "parent aclrtMemcpy(D2H child reply)") &&
             VerifyPattern(actual, child_seed, "parent sees child reply");
    }

    mapping.Cleanup();

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << "  waitpid failed, errno=" << errno << "\n";
        ok = false;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "  child exited abnormally, status=" << status << "\n";
        ok = false;
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    std::cout << "aclrt physical memory IPC probe\n";
    std::cout << "  device=" << options.device << "\n";
    std::cout << "  requested_size=" << options.requested_size << "\n";
    std::cout << "  pid_validation="
              << (options.disable_pid_validation ? "disabled" : "enabled") << "\n";
    std::cout << "  requested_share_api="
              << (options.force_v1 ? "V1" : "V2 if available") << "\n";
    std::cout << "  share_type="
              << (options.share_kind == ShareKind::Fabric ? "fabric" : "default") << "\n";

#if defined(ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT)
    std::cout << "  build_has_v2_share_api=yes\n";
#else
    std::cout << "  build_has_v2_share_api=no\n";
#endif

    uint32_t device_count = 0;
    {
        AclRuntime runtime;
        if (!runtime.Init()) {
            return 1;
        }
        const aclError ret = aclrtGetDeviceCount(&device_count);
        if (!LogAcl("aclrtGetDeviceCount", ret) || device_count == 0U) {
            return 1;
        }
        std::cout << "  device_count=" << device_count << "\n";
    }

    if (options.device >= static_cast<int>(device_count)) {
        std::cerr << "  device id out of range\n";
        return 1;
    }

    bool ok = true;
    ok = RunSingleProcessVmmTest(options) && ok;
    ok = RunMultiProcessIpcTest(options) && ok;
    std::cout << "\nRESULT: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
