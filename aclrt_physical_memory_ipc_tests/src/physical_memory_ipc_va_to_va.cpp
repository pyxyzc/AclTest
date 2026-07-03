#include "physical_memory_ipc_internal.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace acltest::internal {

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

bool RunVaToVaIpcTest(const Options& options,
                      const PhysicalMemoryConfig& config)
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

}  // namespace acltest::internal
