# aclrt Physical Memory IPC Tests

这是一套独立的小测试，用来验证 Ascend Runtime 物理内存、VMM 映射与跨进程共享接口。

接口用法总结见 [docs/interface_usage.md](docs/interface_usage.md)。

默认优先使用 V2 接口：

- `aclrtMemExportToShareableHandleV2`
- `aclrtMemSetPidToShareableHandleV2`
- `aclrtMemImportFromShareableHandleV2`

也可以用 `--use-v1` 强制走 V1。

## 覆盖的能力

1. 单进程 VMM 生命周期：
   `aclrtMemGetAllocationGranularity -> aclrtMallocPhysical ->
   aclrtReserveMemAddress -> aclrtMapMem -> aclrtMemSetAccess ->
   aclrtMemcpy -> aclrtUnmapMem -> aclrtFreePhysical ->
   aclrtReleaseMemAddress`。除 host/vector 与 VMM VA 之间的拷贝外，还会申请两块独立
   device physical memory 并验证 VMM device VA-to-device VA memcpy；随后申请 host
   physical memory 与 device physical memory，验证 host VA -> device VA 和 device
   VA -> host VA。
2. Device physical memory 跨进程共享：
   父进程申请 `ACL_MEM_LOCATION_TYPE_DEVICE + ACL_HBM_MEM_HUGE` 物理内存并导出
   shareable handle，子进程 import 后重新 reserve/map/set access。子进程先读父进程
   pattern，再写入 reply pattern，父进程最后读回校验。随后追加两块独立 device
   physical memory 的跨进程 VA-to-VA 校验，parent 和 child 各执行一次 D2D VA-to-VA
   memcpy。
3. Host physical memory 跨进程共享：
   父进程申请 `ACL_MEM_LOCATION_TYPE_HOST_NUMA + ACL_DDR_MEM_HUGE` 物理内存并导出
   shareable handle，子进程 import 后重新 reserve/map/set access。该测试使用
   2MiB 固定对齐申请大小，不再对 host physical prop 调用
   `aclrtMemGetAllocationGranularity`；随后使用 `ACL_MEM_LOCATION_TYPE_HOST`
   access desc 和 `ACL_MEMCPY_HOST_TO_HOST` 做双向校验。
   若子进程 import/map 成功，还会额外用一块 device physical mapping 探测
   imported host VA 参与 `ACL_MEMCPY_HOST_TO_DEVICE` 与
   `ACL_MEMCPY_DEVICE_TO_HOST` 的能力。
   随后追加两块独立 host physical memory 的跨进程 VA-to-VA 校验，parent 和 child
   各执行一次 H2H VA-to-VA memcpy。任一步不支持或失败都会返回 FAIL。

## 编译

```bash
cd /home/yp/AclTest/aclrt_physical_memory_ipc_tests
source /usr/local/Ascend/ascend-toolkit/set_env.sh
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j
```

## 运行

单项 probe：

```bash
./build/single_process_vmm_probe --device 0 --size 4096
./build/device_physical_ipc_probe --device 0 --size 4096
./build/host_physical_ipc_probe --device 0 --host-numa 0 --size 4096
```

兼容 suite 入口会顺序运行单进程、device IPC 和 host IPC：

```bash
./build/physical_memory_ipc_probe --device 0 --host-numa 0 --size 4096
```

常用参数：

```bash
./build/device_physical_ipc_probe --device 0 --size 4096 --disable-pid-validation
./build/device_physical_ipc_probe --device 0 --size 4096 --use-v1
./build/device_physical_ipc_probe --device 0 --size 4096 --share-type fabric
./build/host_physical_ipc_probe --device 0 --host-numa 0 --size 4096 --use-v1
```

`--size` 是实际校验的数据长度；device physical memory 会用
`aclrtMemGetAllocationGranularity(... ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM ...)`
把申请大小向上对齐，host physical memory 则跳过 granularity 查询并直接按 2MiB
对齐。`--host-numa` 只影响 host physical memory 测试。

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
