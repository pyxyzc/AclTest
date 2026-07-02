# aclrt Physical Memory IPC Interface Usage

这组 Runtime API 的核心模型是：

1. `aclrtMallocPhysical` 申请一块 Host 或 Device 物理内存，返回 `aclrtDrvMemHandle`。
2. `aclrtDrvMemHandle` 不是可直接读写的指针。每个进程都需要自己调用
   `aclrtReserveMemAddress` 预留虚拟地址，再用 `aclrtMapMem` 把这段虚拟地址映射到物理
   handle。
3. 多进程共享时，源进程把物理 handle 导出为 shareable handle；目标进程 import 后得到本
   进程里的 `aclrtDrvMemHandle`，再走自己的 reserve/map/access 流程。
4. 能使用 V2 时优先使用 V2：`aclrtMemExportToShareableHandleV2`、
   `aclrtMemSetPidToShareableHandleV2`、`aclrtMemImportFromShareableHandleV2`。

## 接口分组

| 分组 | 接口 | 作用 |
| --- | --- | --- |
| 物理内存 | `aclrtMallocPhysical` | 申请物理内存，返回物理内存 handle。 |
| 物理内存 | `aclrtFreePhysical` | 释放物理内存 handle。若仍有虚拟映射，需先 `aclrtUnmapMem`。 |
| 虚拟地址 | `aclrtReserveMemAddress` | 预留一段本进程虚拟地址。 |
| 虚拟地址 | `aclrtReleaseMemAddress` | 释放预留的虚拟地址。 |
| 映射 | `aclrtMapMem` | 把虚拟地址映射到物理内存 handle。 |
| 映射 | `aclrtUnmapMem` | 取消虚拟地址和物理内存之间的映射。 |
| 共享 V1 | `aclrtMemExportToShareableHandle` | 把本进程物理 handle 导出为 V1 `uint64_t` shareable handle。 |
| 共享 V1 | `aclrtMemSetPidToShareableHandle` | 给 V1 shareable handle 设置进程白名单。 |
| 共享 V1 | `aclrtMemImportFromShareableHandle` | 在另一个进程中导入 V1 shareable handle。 |
| 共享 V2 | `aclrtMemExportToShareableHandleV2` | 把物理 handle 导出为 V2 shareable handle，支持 `shareType`。 |
| 共享 V2 | `aclrtMemSetPidToShareableHandleV2` | 给 V2 shareable handle 设置进程白名单。 |
| 共享 V2 | `aclrtMemImportFromShareableHandleV2` | 在另一个进程中导入 V2 shareable handle。 |
| 辅助 | `aclrtDeviceGetBareTgid` | 获取适配物理机/容器场景的进程 ID，用于白名单。 |
| 辅助 | `aclrtMemGetAllocationGranularity` | 查询内存申请粒度，用于对齐物理内存大小和 map 大小。 |

## 单进程虚拟内存流程

单进程场景验证的是“物理内存 handle 如何变成可访问的虚拟地址”：

```cpp
aclrtPhysicalMemProp prop = {};
prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
prop.allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
prop.memAttr = ACL_HBM_MEM_NORMAL;
prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
prop.location.id = deviceId;

size_t granularity = 0;
aclrtMemGetAllocationGranularity(
    &prop, ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM, &granularity);
size_t alignedSize = AlignUp(requestedSize, granularity);

aclrtDrvMemHandle handle = nullptr;
aclrtMallocPhysical(&handle, alignedSize, &prop, 0);

void* virPtr = nullptr;
aclrtReserveMemAddress(&virPtr, alignedSize, 0, nullptr, 0);
aclrtMapMem(virPtr, alignedSize, 0, handle, 0);

aclrtMemAccessDesc access = {};
access.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
access.location.id = deviceId;
access.flags = ACL_RT_MEM_ACCESS_FLAGS_READWRITE;
aclrtMemSetAccess(virPtr, alignedSize, &access, 1);

// Now virPtr can be used by runtime APIs such as aclrtMemcpy.

aclrtUnmapMem(virPtr);
aclrtReleaseMemAddress(virPtr);
aclrtFreePhysical(handle);
```

关键点：

- `aclrtMallocPhysical` 的 `size` 应按 `ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM`
  对齐。
- `aclrtMapMem` 的 `size` 必须和物理内存申请大小匹配，并满足粒度对齐要求。
- `offset` 和 `flags` 当前固定为 `0`。
- 释放虚拟地址前必须先 `aclrtUnmapMem`。

## 多进程共享流程

多进程共享不是把一个进程里的虚拟地址传给另一个进程。共享的是“底层物理内存”，每个进程有自己的虚拟地址映射。

### 父进程

父进程负责申请物理内存，并导出 shareable handle：

```cpp
aclrtDrvMemHandle handle = nullptr;
aclrtMallocPhysical(&handle, alignedSize, &prop, 0);

uint64_t flags = ACL_RT_VMM_EXPORT_FLAG_DEFAULT;
uint64_t shareableHandle = 0;
aclrtMemExportToShareableHandle(
    handle, ACL_MEM_HANDLE_TYPE_NONE, flags, &shareableHandle);

int32_t childBareTgid = ...; // child calls aclrtDeviceGetBareTgid and sends it back.
aclrtMemSetPidToShareableHandle(shareableHandle, &childBareTgid, 1);
```

V2 版本建议优先使用：

```cpp
uint64_t flags = ACL_RT_VMM_EXPORT_FLAG_DEFAULT;
aclrtMemSharedHandleType shareType = ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT;
uint64_t shareableHandle = 0;

aclrtMemExportToShareableHandleV2(handle, flags, shareType, &shareableHandle);
aclrtMemSetPidToShareableHandleV2(&shareableHandle, shareType, &childBareTgid, 1);
```

如果要关闭白名单校验，可以把 `flags` 设置为
`ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION`，此时无需调用
`aclrtMemSetPidToShareableHandle*`。能力探测时可以这样做，但真实业务里建议保留白名单。

### 子进程

子进程导入 shareable handle 后，会得到本进程自己的物理 handle：

```cpp
aclrtDrvMemHandle imported = nullptr;
aclrtMemImportFromShareableHandle(shareableHandle, deviceId, &imported);
```

V2：

```cpp
aclrtDrvMemHandle imported = nullptr;
aclrtMemImportFromShareableHandleV2(&shareableHandle, shareType, 0, &imported);
```

导入之后，子进程仍然必须自己：

```cpp
aclrtReserveMemAddress(&virPtr, alignedSize, 0, nullptr, 0);
aclrtMapMem(virPtr, alignedSize, 0, imported, 0);
aclrtMemSetAccess(virPtr, alignedSize, &access, 1);
```

## V1 与 V2 的差异

| 项目 | V1 | V2 |
| --- | --- | --- |
| 导出接口 | `aclrtMemExportToShareableHandle` | `aclrtMemExportToShareableHandleV2` |
| 白名单接口 | `aclrtMemSetPidToShareableHandle` | `aclrtMemSetPidToShareableHandleV2` |
| 导入接口 | `aclrtMemImportFromShareableHandle` | `aclrtMemImportFromShareableHandleV2` |
| shareable handle 形态 | `uint64_t` | 由 `shareType` 决定 |
| AI Server 内共享 | 支持 | `ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT` |
| 跨 AI Server 共享 | 不表达 | `ACL_MEM_SHARE_HANDLE_TYPE_FABRIC`，仅部分 A3 产品支持 |

V2 的 `shareableHandle` 是调用者提供的一段内存：

- `ACL_MEM_SHARE_HANDLE_TYPE_DEFAULT`：指向一个 `uint64_t`。
- `ACL_MEM_SHARE_HANDLE_TYPE_FABRIC`：指向一个 `aclrtMemFabricHandle`，文档定义大小为
  128 字节。

## 白名单与进程 ID

默认导出 flag 是 `ACL_RT_VMM_EXPORT_FLAG_DEFAULT`，表示启用进程白名单校验。启用时：

1. 目标进程先调用 `aclrtDeviceGetBareTgid`。
2. 目标进程把得到的 `pid` 传给源进程。
3. 源进程调用 `aclrtMemSetPidToShareableHandle` 或 V2 版本设置白名单。
4. 目标进程再 import shareable handle。

不要直接用 `getpid()` 替代 `aclrtDeviceGetBareTgid`。官方文档说明该接口适配了物理机和 Docker 场景；容器中直接取 OS PID 可能不是 Runtime 期望的白名单 ID。

## 生命周期与释放顺序

每个进程都要释放自己持有的资源：

```text
aclrtUnmapMem
aclrtReleaseMemAddress
aclrtFreePhysical
```

注意：

- 导入进程也需要对 import 得到的 handle 调 `aclrtFreePhysical`。
- 只有所有相关进程都释放各自的 handle 后，底层共享物理内存才会真正释放。
- 源进程不能在目标进程 import 前提前释放物理内存。
- 释放后继续使用原 handle 或虚拟地址属于未定义行为。

## 测试覆盖关系

本目录拆成三个单项 probe，并保留 `physical_memory_ipc_probe` 兼容 suite 入口。每个
probe 都保留 host/vector 指针与 VMM VA 之间的校验，并追加独立 physical memory 之间的
VMM VA-to-VA 校验：

| Probe | 覆盖接口 |
| --- | --- |
| `single_process_vmm_probe` | 单映射 H2D/D2H 校验；两块独立 device physical memory 的 VA-to-VA D2D 校验 |
| `device_physical_ipc_probe` parent | 单 handle IPC 双向校验；两 handle IPC 中 parent 执行一次 VA-to-VA D2D，并导出 src/dst 两个 handle |
| `device_physical_ipc_probe` child | import/map src/dst 两个 handle；验证 parent VA-to-VA 结果；再执行一次 child VA-to-VA D2D |
| `host_physical_ipc_probe` | Host NUMA physical memory 的单 handle IPC 双向校验；两 handle IPC 中 parent/child 各执行一次 VA-to-VA H2H |
| cleanup | `aclrtUnmapMem`、`aclrtReleaseMemAddress`、`aclrtFreePhysical` |

运行命令：

```bash
./build/single_process_vmm_probe --device 0 --size 4096
./build/device_physical_ipc_probe --device 0 --size 4096
./build/host_physical_ipc_probe --device 0 --host-numa 0 --size 4096
./build/physical_memory_ipc_probe --device 0 --host-numa 0 --size 4096
```

辅助探测：

```bash
# 关闭白名单校验，确认导出/import 基础能力
./build/device_physical_ipc_probe --device 0 --size 4096 --disable-pid-validation

# 强制使用 V1 接口
./build/device_physical_ipc_probe --device 0 --size 4096 --use-v1

# 探测 fabric handle；仅部分 A3 跨 AI Server 共享场景支持
./build/device_physical_ipc_probe --device 0 --size 4096 --share-type fabric
```

## 官方文档入口

- [aclrtMallocPhysical](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0112.html)
- [aclrtFreePhysical](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0113.html)
- [aclrtReserveMemAddress](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0114.html)
- [aclrtReleaseMemAddress](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0115.html)
- [aclrtMapMem](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0116.html)
- [aclrtUnmapMem](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0117.html)
- [aclrtMemExportToShareableHandle](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0118.html)
- [aclrtDeviceGetBareTgid](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0119.html)
- [aclrtMemSetPidToShareableHandle](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0120.html)
- [aclrtMemImportFromShareableHandle](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0121.html)
- [aclrtMemGetAllocationGranularity](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0122.html)
- [aclrtMemExportToShareableHandleV2](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_2006.html)
- [aclrtMemSetPidToShareableHandleV2](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_2007.html)
- [aclrtMemImportFromShareableHandleV2](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_2008.html)
