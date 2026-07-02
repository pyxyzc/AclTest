#include "physical_memory_common.h"
#include "physical_memory_utils.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace acltest {
using namespace internal;
namespace {

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

}  // namespace

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
