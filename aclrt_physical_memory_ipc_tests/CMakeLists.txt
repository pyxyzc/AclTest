cmake_minimum_required(VERSION 3.16)

project(aclrt_physical_memory_ipc_tests LANGUAGES CXX)

if(NOT UNIX)
    message(FATAL_ERROR "This probe uses fork/pipe and must be built on Linux.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CheckCXXSourceCompiles)

set(ASCEND_ROOT "" CACHE PATH "Path to Ascend toolkit root")

if(NOT ASCEND_ROOT)
    set(_ascend_root_candidates
        "$ENV{ASCEND_ROOT}"
        "$ENV{ASCEND_HOME}"
        "$ENV{ASCEND_TOOLKIT_HOME}"
        "$ENV{ASCEND_INSTALL_PATH}/ascend-toolkit/latest"
        "/usr/local/Ascend/ascend-toolkit/latest"
    )
    foreach(_candidate IN LISTS _ascend_root_candidates)
        if(_candidate AND EXISTS "${_candidate}")
            set(ASCEND_ROOT "${_candidate}" CACHE PATH "Path to Ascend toolkit root" FORCE)
            break()
        endif()
    endforeach()
endif()

find_path(ACL_INCLUDE_DIR
    NAMES acl/acl.h
    HINTS
        "${ASCEND_ROOT}/include"
        "$ENV{ASCEND_ROOT}/include"
        "$ENV{ASCEND_HOME}/include"
        "$ENV{ASCEND_TOOLKIT_HOME}/include"
        "$ENV{ASCEND_INSTALL_PATH}/ascend-toolkit/latest/include"
        "/usr/local/Ascend/ascend-toolkit/latest/include"
)

find_library(ASCENDCL_LIBRARY
    NAMES ascendcl
    HINTS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/lib64/stub"
        "$ENV{ASCEND_ROOT}/lib64"
        "$ENV{ASCEND_HOME}/lib64"
        "$ENV{ASCEND_TOOLKIT_HOME}/lib64"
        "$ENV{ASCEND_INSTALL_PATH}/ascend-toolkit/latest/lib64"
        "/usr/local/Ascend/ascend-toolkit/latest/lib64"
)

if(NOT ACL_INCLUDE_DIR)
    message(FATAL_ERROR
        "Cannot find acl/acl.h. Pass -DASCEND_ROOT=/path/to/ascend-toolkit/latest")
endif()

if(NOT ASCENDCL_LIBRARY)
    message(FATAL_ERROR
        "Cannot find libascendcl.so. Pass -DASCEND_ROOT=/path/to/ascend-toolkit/latest")
endif()

set(CMAKE_REQUIRED_INCLUDES "${ACL_INCLUDE_DIR}")
check_cxx_source_compiles("
#include <acl/acl.h>
int main()
{
    using ExportFn = decltype(&aclrtMemExportToShareableHandleV2);
    using ImportFn = decltype(&aclrtMemImportFromShareableHandleV2);
    using SetPidFn = decltype(&aclrtMemSetPidToShareableHandleV2);
    ExportFn export_fn = nullptr;
    ImportFn import_fn = nullptr;
    SetPidFn set_pid_fn = nullptr;
    (void)export_fn;
    (void)import_fn;
    (void)set_pid_fn;
    return 0;
}
" ACLTEST_HAS_V2_SHARE_API)

check_cxx_source_compiles("
#include <acl/acl.h>
int main()
{
    aclrtMemSharedHandleType type = ACL_MEM_SHARE_HANDLE_TYPE_FABRIC;
    aclrtMemFabricHandle handle = {};
    (void)type;
    (void)handle;
    return 0;
}
" ACLTEST_HAS_FABRIC_SHARE_TYPE)
unset(CMAKE_REQUIRED_INCLUDES)

add_library(physical_memory_probe_common STATIC
    src/physical_memory_common.cpp
)

target_include_directories(physical_memory_probe_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ACL_INCLUDE_DIR}"
)

target_link_libraries(physical_memory_probe_common PUBLIC
    "${ASCENDCL_LIBRARY}"
    pthread
    dl
)

if(ACLTEST_HAS_FABRIC_SHARE_TYPE)
    target_compile_definitions(physical_memory_probe_common PUBLIC
        ACLTEST_HAS_FABRIC_SHARE_TYPE=1)
else()
    target_compile_definitions(physical_memory_probe_common PUBLIC
        ACLTEST_HAS_FABRIC_SHARE_TYPE=0)
endif()

if(ACLTEST_HAS_V2_SHARE_API)
    target_compile_definitions(physical_memory_probe_common PUBLIC
        ACLTEST_HAS_V2_SHARE_API=1)
else()
    target_compile_definitions(physical_memory_probe_common PUBLIC
        ACLTEST_HAS_V2_SHARE_API=0)
endif()

function(add_physical_memory_probe target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE physical_memory_probe_common)
endfunction()

add_physical_memory_probe(single_process_vmm_probe
    src/single_process_vmm_probe.cpp)
add_physical_memory_probe(device_physical_ipc_probe
    src/device_physical_ipc_probe.cpp)
add_physical_memory_probe(host_physical_ipc_probe
    src/host_physical_ipc_probe.cpp)
add_physical_memory_probe(physical_memory_ipc_probe
    src/physical_memory_ipc_probe.cpp)

message(STATUS "ACL include dir: ${ACL_INCLUDE_DIR}")
message(STATUS "AscendCL library: ${ASCENDCL_LIBRARY}")
message(STATUS "V2 shareable handle API: ${ACLTEST_HAS_V2_SHARE_API}")
message(STATUS "Fabric share handle type: ${ACLTEST_HAS_FABRIC_SHARE_TYPE}")
