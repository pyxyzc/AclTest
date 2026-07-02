# aclrt Physical Memory IPC Tests

这是一套独立的小测试，用来验证 Ascend Runtime 物理内存与跨进程共享接口。

接口用法总结见 [docs/interface_usage.md](docs/interface_usage.md)。

默认优先使用 V2 接口：

- `aclrtMemExportToShareableHandleV2`
- `aclrtMemSetPidToShareableHandleV2`
- `aclrtMemImportFromShareableHandleV2`

如果当前 CANN 头文件没有 V2 类型，会退回 V1 接口；也可以用 `--use-v1`
强制走 V1。

## 覆盖的能力

1. 单进程 VMM 生命周期：
   `aclrtMemGetAllocationGranularity -> aclrtMallocPhysical ->
   aclrtReserveMemAddress -> aclrtMapMem -> aclrtMemSetAccess ->
   aclrtMemcpy -> aclrtUnmapMem -> aclrtFreePhysical ->
   aclrtReleaseMemAddress`。
2. 多进程共享：
   父进程申请 Device 物理内存并导出 shareable handle，子进程通过
   `aclrtDeviceGetBareTgid` 提供白名单 PID，随后 import 同一块物理内存。
   子进程先读父进程写入的 pattern，再写入新的 pattern，父进程最后读回校验。

## 编译

```bash
cd /home/yp/unified-cache-management/aclrt_physical_memory_ipc_tests
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j
```

## 运行

```bash
./build/physical_memory_ipc_probe --device 0 --size 4096
```

常用参数：

```bash
./build/physical_memory_ipc_probe --device 0 --size 4096
./build/physical_memory_ipc_probe --device 0 --size 4096 --disable-pid-validation
./build/physical_memory_ipc_probe --device 0 --size 4096 --use-v1
./build/physical_memory_ipc_probe --device 0 --size 4096 --share-type fabric
```

`--size` 是实际校验的数据长度；程序会用
`aclrtMemGetAllocationGranularity(... ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM ...)`
把物理内存申请大小向上对齐。

## 官方文档

- aclrtMallocPhysical: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0112.html
- aclrtFreePhysical: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0113.html
- aclrtReserveMemAddress: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0114.html
- aclrtReleaseMemAddress: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0115.html
- aclrtMapMem: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0116.html
- aclrtUnmapMem: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0117.html
- aclrtMemExportToShareableHandle: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0118.html
- aclrtDeviceGetBareTgid: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0119.html
- aclrtMemSetPidToShareableHandle: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0120.html
- aclrtMemImportFromShareableHandle: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0121.html
- aclrtMemGetAllocationGranularity: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0122.html
- aclrtMemExportToShareableHandleV2: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_2006.html
- aclrtMemSetPidToShareableHandleV2: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_2007.html
- aclrtMemImportFromShareableHandleV2: https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_2008.html
