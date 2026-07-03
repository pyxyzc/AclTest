#pragma once

#include "physical_memory_common.h"
#include "physical_memory_utils.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace acltest::internal {

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

bool WriteFull(int fd, const void* data, size_t size);
bool ReadFull(int fd, void* data, size_t size);
void CloseFd(int* fd);

void SendChildResult(int write_fd, bool ok, aclError ret,
                     const std::string& message);
void SendStopMessage(int fd);

bool IsHostPhysicalConfig(const PhysicalMemoryConfig& config);
bool ExportShareableHandle(const Options& options, aclrtDrvMemHandle handle,
                           int32_t child_bare_tgid, SharedHandleBlob* blob);
bool ImportAndMapSharedHandle(const ShareMsg& share_msg, size_t index,
                              const PhysicalMemoryConfig& config,
                              PhysicalMapping* mapping, aclError* failure_ret);

bool RunImportedHostToDeviceProbe(const ShareMsg& share_msg,
                                  const PhysicalMapping& host_mapping,
                                  uint32_t expected_seed,
                                  const std::string& label_prefix);
bool RunDeviceToImportedHostProbe(const ShareMsg& share_msg,
                                  PhysicalMapping* host_mapping,
                                  uint32_t seed,
                                  const std::string& label_prefix);

int RunIpcChild(int read_fd, int write_fd, const PhysicalMemoryConfig& config);
int RunSingleMappingIpcChild(int write_fd, const ShareMsg& share_msg,
                             const PhysicalMemoryConfig& config);
int RunVaToVaIpcChild(int write_fd, const ShareMsg& share_msg,
                      const PhysicalMemoryConfig& config);
bool RunSingleMappingIpcTest(const Options& options,
                             const PhysicalMemoryConfig& config);
bool RunVaToVaIpcTest(const Options& options,
                      const PhysicalMemoryConfig& config);

}  // namespace acltest::internal
