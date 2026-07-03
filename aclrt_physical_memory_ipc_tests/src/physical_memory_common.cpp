#include "physical_memory_common.h"
#include "console_utils.h"
#include "physical_memory_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
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

std::string FormatOptions(const Options& options)
{
    std::ostringstream out;
    out << "  device=" << options.device << "\n";
    out << "  host_numa=" << options.host_numa << "\n";
    out << "  requested_size=" << options.requested_size << "\n";
    out << "  pid_validation="
        << (options.disable_pid_validation ? "disabled" : "enabled") << "\n";
    out << "  requested_share_api="
        << (options.force_v1 ? "V1" : "V2 if available") << "\n";
    out << "  share_type="
        << (options.share_kind == ShareKind::Fabric ? "fabric" : "default") << "\n";
    out << "  build_has_v2_share_api="
        << (ACLTEST_HAS_V2_SHARE_API ? "yes" : "no") << "\n";
    out << "  build_has_fabric_share_type="
        << (ACLTEST_HAS_FABRIC_SHARE_TYPE ? "yes" : "no");
    return out.str();
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool Contains(const std::string& value, const std::string& needle)
{
    return value.find(needle) != std::string::npos;
}

std::string GetSocName()
{
    const char* soc_name = aclrtGetSocName();
    if (soc_name == nullptr || soc_name[0] == '\0') {
        return "unknown";
    }
    return soc_name;
}

std::string DetectAscendFamily(const std::string& soc_name)
{
    const std::string lower = ToLower(soc_name);
    if (Contains(lower, "a3") || Contains(lower, "910_93") ||
        Contains(lower, "910_95")) {
        return "Ascend A3";
    }
    if (Contains(lower, "a2") || Contains(lower, "910b")) {
        return "Ascend A2";
    }
    if (Contains(lower, "a5") || Contains(lower, "950") ||
        Contains(lower, "cloud_v5") || Contains(lower, "cloudv5")) {
        return "Ascend A5";
    }
    return "unknown";
}

std::string GetCannVersion()
{
#if ACLTEST_HAS_ACLSYS_GET_VERSION_STR
    auto try_get_version = [](char* pkg_name) -> std::string {
        char version_str[ACL_PKG_VERSION_MAX_SIZE] = {0};
        const aclError ret = aclsysGetVersionStr(pkg_name, version_str);
        if (ret == ACL_SUCCESS && version_str[0] != '\0') {
            return version_str;
        }
        return {};
    };

    char cann_pkg_name[] = "CANN";
    std::string version = try_get_version(cann_pkg_name);
    if (!version.empty()) {
        return version;
    }

    char runtime_pkg_name[] = "runtime";
    version = try_get_version(runtime_pkg_name);
    if (!version.empty()) {
        return version;
    }
#endif

    int32_t major_version = 0;
    int32_t minor_version = 0;
    int32_t patch_version = 0;
    const aclError ret =
        aclrtGetVersion(&major_version, &minor_version, &patch_version);
    if (ret == ACL_SUCCESS) {
        std::ostringstream out;
        out << "aclrt_interface_version=" << major_version << "." << minor_version
            << "." << patch_version;
        return out.str();
    }
    return "unknown";
}

void PrintSingleProcessBanner(const char* title, const Options& options)
{
    const std::string soc_name = GetSocName();
    std::ostringstream env;
    env << title << "\n";
    env << "  ascend_family=" << DetectAscendFamily(soc_name) << "\n";
    env << "  soc_name=" << soc_name << "\n";
    env << "  cann_version=" << GetCannVersion();

    PrintRed(env.str());
    std::cout << "\n";
    PrintBlue(FormatOptions(options));
    std::cout << "\n";
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
    std::cout << FormatOptions(options) << "\n";
}

}  // namespace

int RunTestProgram(int argc, char** argv, const char* title,
                   const TestCase* tests, size_t test_count,
                   StartupDisplayMode display_mode)
{
    Options options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    if (display_mode == StartupDisplayMode::SingleProcessBanner) {
        PrintSingleProcessBanner(title, options);
    } else {
        std::cout << title << "\n";
        PrintOptions(options);
    }

    if (!ValidateDevice(options)) {
        return 1;
    }

    bool ok = true;
    for (size_t i = 0; i < test_count; ++i) {
        if (display_mode != StartupDisplayMode::SingleProcessBanner) {
            std::cout << "\n== " << tests[i].name << " ==\n";
        }
        ok = tests[i].run(options) && ok;
    }

    std::cout << "\nRESULT: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}

}  // namespace acltest
