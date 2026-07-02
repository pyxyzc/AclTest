#pragma once

#include "physical_memory_common.h"

#include <cstdint>
#include <string>
#include <vector>

#ifndef ACLTEST_HAS_FABRIC_SHARE_TYPE
#define ACLTEST_HAS_FABRIC_SHARE_TYPE 0
#endif

#ifndef ACLTEST_HAS_V2_SHARE_API
#define ACLTEST_HAS_V2_SHARE_API 0
#endif

namespace acltest::internal {

struct PhysicalMemoryConfig {
    const char* name = "";
    aclrtPhysicalMemProp prop = {};
    size_t fixed_allocation_granularity = 0;
    aclrtMemLocation access_location = {};
    aclrtMemcpyKind host_to_mapping_kind = ACL_MEMCPY_HOST_TO_DEVICE;
    aclrtMemcpyKind mapping_to_host_kind = ACL_MEMCPY_DEVICE_TO_HOST;
    aclrtMemcpyKind mapping_to_mapping_kind = ACL_MEMCPY_DEVICE_TO_DEVICE;
    const char* write_tag = "H2D";
    const char* read_tag = "D2H";
    const char* va_to_va_tag = "D2D";
};

class AclRuntime {
public:
    bool Init();
    bool SetDevice(int device);
    ~AclRuntime();

private:
    bool initialized_here_ = false;
    bool device_set_ = false;
    int device_ = 0;
};

bool LogAcl(const std::string& label, aclError ret);

PhysicalMemoryConfig MakeDeviceConfig(const Options& options);
PhysicalMemoryConfig MakeHostConfig(const Options& options);
bool QueryAlignedSize(const PhysicalMemoryConfig& config, size_t requested,
                      size_t* aligned);

std::vector<uint8_t> MakePattern(size_t size, uint32_t seed);
bool VerifyPattern(const std::vector<uint8_t>& data, uint32_t seed,
                   const std::string& label);

struct PhysicalMapping {
    aclrtDrvMemHandle handle = nullptr;
    void* virt = nullptr;
    size_t size = 0;
    bool mapped = false;
    bool reserved = false;
    bool owns_handle = false;

    bool ReserveMapAndSetAccess(aclrtDrvMemHandle input_handle, size_t map_size,
                                const aclrtMemLocation& access_location);
    void Cleanup();
};

bool AllocateAndMapPhysical(const PhysicalMemoryConfig& config, size_t aligned_size,
                            PhysicalMapping* mapping);
bool CopyHostToMapping(void* dst, size_t dst_max, const std::vector<uint8_t>& src,
                       const PhysicalMemoryConfig& config, const std::string& label);
bool CopyMappingToHost(std::vector<uint8_t>* dst, const void* src, size_t src_size,
                       const PhysicalMemoryConfig& config, const std::string& label);
bool CopyMappingToMapping(void* dst, size_t dst_max, const void* src, size_t copy_size,
                          const PhysicalMemoryConfig& config, const std::string& label);
bool CopyWithKind(void* dst, size_t dst_max, const void* src, size_t copy_size,
                  aclrtMemcpyKind kind, const std::string& label);

}  // namespace acltest::internal
