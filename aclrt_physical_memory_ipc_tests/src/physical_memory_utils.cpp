#include "physical_memory_utils.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace acltest::internal {
namespace {

constexpr size_t kHostPhysicalFixedGranularity = 2UL * 1024UL * 1024UL;
constexpr aclrtMemAccessFlags kAclMemAccessReadWrite =
    static_cast<aclrtMemAccessFlags>(0x3UL);

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

}  // namespace

bool LogAcl(const std::string& label, aclError ret)
{
    std::cout << "  " << std::left << std::setw(42) << label
              << " ret=" << FormatAclRet(ret) << "\n";
    return ret == ACL_SUCCESS;
}


bool AclRuntime::Init()
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

bool AclRuntime::SetDevice(int device)
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

AclRuntime::~AclRuntime()
{
    if (device_set_) {
        (void)aclrtResetDevice(device_);
    }
    if (initialized_here_) {
        (void)aclFinalize();
    }
}

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
    config.fixed_allocation_granularity = kHostPhysicalFixedGranularity;
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

bool QueryAlignedSize(const PhysicalMemoryConfig& config, size_t requested, size_t* aligned)
{
    if (config.fixed_allocation_granularity != 0U) {
        *aligned = AlignUp(requested, config.fixed_allocation_granularity);
        std::cout << "  skip aclrtMemGetAllocationGranularity for "
                  << config.name << "\n";
        std::cout << "  requested_size=" << requested
                  << ", fixed_granularity="
                  << config.fixed_allocation_granularity
                  << ", aligned_size=" << *aligned << "\n";
        return true;
    }

    size_t minimum = 0;
    auto ret = aclrtMemGetAllocationGranularity(
        const_cast<aclrtPhysicalMemProp*>(&config.prop),
        ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM, &minimum);
    if (!LogAcl("aclrtMemGetAllocationGranularity(MIN)", ret)) {
        return false;
    }

    size_t recommended = 0;
    ret = aclrtMemGetAllocationGranularity(
        const_cast<aclrtPhysicalMemProp*>(&config.prop),
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


bool PhysicalMapping::ReserveMapAndSetAccess(aclrtDrvMemHandle input_handle,
                                             size_t map_size,
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

void PhysicalMapping::Cleanup()
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

}  // namespace acltest::internal
