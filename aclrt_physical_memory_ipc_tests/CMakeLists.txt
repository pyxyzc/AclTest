cmake_minimum_required(VERSION 3.16)

project(aclrt_physical_memory_ipc_tests LANGUAGES CXX)

if(NOT UNIX)
    message(FATAL_ERROR "This probe uses fork/pipe and must be built on Linux.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(physical_memory_ipc_probe
    src/physical_memory_ipc_probe.cpp
)

target_include_directories(physical_memory_ipc_probe PRIVATE
    "${ACL_INCLUDE_DIR}"
)

target_link_libraries(physical_memory_ipc_probe PRIVATE
    "${ASCENDCL_LIBRARY}"
    pthread
    dl
)

message(STATUS "ACL include dir: ${ACL_INCLUDE_DIR}")
message(STATUS "AscendCL library: ${ASCENDCL_LIBRARY}")
