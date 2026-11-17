# 驱动包与来源记录

## 驱动包是什么

此项目不是纯用户态虚拟摄像头。`src/usb/virt_dev.cpp` 通过 SetupAPI 查找 `GUID_XUSB_VIRT_INTERFACE`，再向虚拟 USB 总线发送 IOCTL；没有提供该接口的内核驱动时，程序会在启动阶段报 `not found virtual usb path`。

## 当前运行验证（2026-11-17）

测试机同一 Windows 启动周期内运行重建总线 `1.0.0.11` 和版本隔离 HCD `2.0.0.22`；控制器、虚拟根集线器和启动中的摄像头节点均为 OK。应用启停会添加/移除摄像头，无需重启 Windows；HCD 诊断显示待处理事务和移除工作项均回到 0。

DirectShow 已通过真实设备协商并抓帧：1280×720 YUY2 描述符上限约 13.3fps，MJPEG 提供 15/30fps；测试机以 30fps 请求抓取 60 帧，约 25fps。候选 HCD `2.0.0.23` 和用户态已加入 SuperSpeed 端口、USB 3.0 BOS/companion 描述符及 45,000 字节服务间隔协商；Release 构建和协议级检查通过，但尚未在 Windows PnP/DirectShow 上验证。测试机仍运行 `2.0.0.22`，所以不能把 USB 3.0 当作已可用能力。当前实机结果只证明 USB 2.0 摄像头流路径，不证明所有原版驱动角色、URB 家族或生产签名包完全可替代。更多使用命令见[运行说明](usage.md)。

目录 `drivers/usb-bus-legacy/` 保留了上游仓库提供的 x86/x64 包。x64 目录实际包含 `xusb_bus.inf`、`xusb_ctrl.inf`、`xusb_virt.cat` 和 `xusb_virt.sys`；这些文件没有在重构中丢失。它们是旧版二进制包，不是当前构建的一部分。

## 可核实的来源链

1. 当前仓库的初始导入提交注明它从 [`jasonsalex/Virtual-Device-Camera`](https://github.com/jasonsalex/Virtual-Device-Camera) fork；该上游 x64 目录包含同名的四个文件。[当前仓库的导入提交](https://github.com/Young-Six-6/Virtual-Device-Camera/commit/d37f201cb120a368be7905ccf3d236a583a0838a)和[上游 x64 驱动目录](https://github.com/jasonsalex/Virtual-Device-Camera/tree/master/%E8%99%9A%E6%8B%9F%E7%AB%AF%E9%A9%B1%E5%8A%A8-%E6%9C%80%E6%96%B0-2016-10%E6%9C%88%E4%BB%BD/x64)可供核对。
2. 归档的项目原说明称 `src/old-drv-interface` 是 Fanxiushu 的 CSDN 资源 [9538188](https://download.csdn.net/detail/fanxiushu/9538188) 中的驱动接口部分，并建议从该资源获取完整驱动源码。该页面目前无法从本次研究环境访问；原说明也没有给出可验证的源码版本或哈希，因此无法确认仓库里的二进制是由该源码版本编译而来。旧说明和旧接口代码保存在 `archive/legacy-2016/`。
3. 本地 `x64/xusb_virt.sys` 的 PE 版本资源标注 `CompanyName=Fanxiushu`、`ProductName=USB Remote Access Driver`、`OriginalFilename=xfs_redir.sys`、`FileVersion=1.0.0.1` 和 `Fanxiushu 2015-2016` 版权信息。Windows `Get-AuthenticodeSignature` 在检查时返回 `Valid`；签名证书主体为上海风创信息咨询有限公司。签名状态本身不能证明驱动适配当前 Windows，也不能证明安装包完整。
4. Fanxiushu 的公开 GitHub 项目还包括 [`xFsRedir`](https://github.com/fanxiushu/xFsRedir)、[`xdisp_virt`](https://github.com/fanxiushu/xdisp_virt) 和 [`vcam_dshow`](https://github.com/fanxiushu/vcam_dshow)。`xdisp_virt` 当前公开的 Windows 虚拟摄像头包使用 `avstrm_virt`，与这里的自定义 USB 总线驱动不同；`xFsRedir` 是文件/目录重定向项目。`xusb_virt.sys` 内部的 `xfs_redir.sys` 原文件名是一个可追踪线索，但现有材料不足以证明它与这些项目中的驱动源码同源。

x64 `xusb_virt.sys` 大小为 51,368 字节；其 Git blob SHA-1 为 `fb62e682170ec53b570bb7dbd1fd2ac9b2ab85b1`，与 `jasonsalex` 上游目录和本仓库初始提交中的文件一致。当前工作区 SHA-256 为 `D3036053F37E3A7377FD273459439E0DF0E2780FE6E008D45CD48E0E7752B417`。这些值能验证分发副本没有在重构时被替换，不等于证明 CSDN 下载源文件的哈希相同。

因此，目前可追溯到“原项目文档引用的 CSDN 资源、同名 GitHub 分发副本和二进制内嵌作者/签名信息”；原作者完整源码包不可得，也没有足够证据把二进制映射到某个可重建的公开源码提交。仓库中的 WDM/UCX 工程是独立重建实现，不应称为原作者源码或逐字节复刻。驱动二进制的独立许可条款也未在包内提供，不能仅凭应用程序的 MIT `LICENSE` 推断驱动许可。

`drivers/usb-bus-legacy/driver-source/` 是独立的新 WDM 源码工程，不会覆盖旧二进制包。原包四种角色及当前复刻状态见[重建驱动说明](../drivers/usb-bus-legacy/driver-source/README.md)：默认包包含应用使用的 USB-only enumerator 与用于更新旧应用控制接口提供者的 Root Hub；Host Controller/USB-tree 仍隔离在实验包。原二进制在角色选择中将未匹配 ID 归入 USB-only role 0；重建版已保留此回退行为，INF 仍只匹配受支持的已知 ID。

用户态通过 `IOCTL_XUSB_QUERY_DRIVER_INFO` 探测重建驱动。FOSS 版本返回带 `FOSS` 签名的版本结构；旧原版不识别这个私有查询码，用户态将其标记为“原版驱动”。既有四个 USB IOCTL 编号和结构保持不变。Web UI 的驱动状态卡也会显示识别结果。

- 父设备按硬件 ID 识别 USB-only、USB-tree、Root HUB 和 Host Controller 角色。已按旧二进制补齐角色设备接口：USB-only 使用应用控制接口，Root HUB 使用标准 USB HUB 接口并额外提供应用控制接口，Host Controller 使用标准主控制器接口。USB-only 与 Root HUB 可通过控制接口新增/移除子 PDO 并提供 `BusRelations`；Host Controller 与 USB-tree 启动时分别创建并经 `BusRelations` 枚举 Root HUB 和 Host Controller 内部 PDO；USB-tree 没有用户控制接口。USB-tree 的内部 HCD PDO 现在报告专用软件 bus GUID 与 `PNPBus`，和 UCX 的 Custom 父总线类型一致；普通 USB PDO 仍报告标准 USB bus GUID。
- 子 PDO 经 cancel-safe queue 转发旧协议支持的控制/控制扩展、设备/接口/端点描述符、vendor/class、status、feature、pipe reset、frame number、Microsoft feature descriptor、配置/接口选择、bulk/interrupt 和等时 URB。`RESET_PORT`/`CYCLE_PORT` 也映射到旧 BEGIN/END 协议。
- 配置/接口选择会校验当前配置。Pipe 和 interface 句柄带有代际信息，避免 alternate setting 切换后旧句柄误指向新端点。每个父设备最多创建 256 个子 PDO；每个子 PDO 最多允许 256 个事务和 64 MiB 事务预算，父设备总上限为 1024 个事务和 256 MiB。
- 子 PDO 使用从旧二进制恢复的 `IoCreateDeviceSecure` 默认 SDDL 和 class GUID：SY 完全访问，BA/WD/RC 读写执行；相同 class GUID 保留 Windows 类注册表安全覆盖机制。子 PDO 处理基础 query/set power IRP。父 FDO 已实现 WMI provider 注册和 8 字节数据块回调。

当前已在目标 Windows 上通过虚拟根集线器运行用户态摄像头，Windows UVC 驱动可协商并抓取 YUY2 与 MJPEG 等时流。尚未实现或验证的边界包括 bulk/interrupt 数据传输、Microsoft OS feature descriptor 的完整处理、完整多端口/电源行为、所有 Host Controller URB 家族和生产签名发布。重建版实现 USB bus type 查询，并在 Root Hub FDO 本地响应 Hub name、节点描述符/能力、连接信息、设备描述符转发、DriverKeyName 与 `IOCTL_INTERNAL_USB_GET_HUB_COUNT`；descriptor proxy 等待用户态最多 5 秒后取消。Host Controller 返回 Root Hub PDO/栈顶，子 PDO 根据 present 状态响应 `IOCTL_INTERNAL_USB_GET_PORT_STATUS`。USBDI 查询按原版提供 V0–V3 接口槽和回调返回值；PDO 处理设备文本和 usage notification，明确不支持 Eject，并复用用户态建议的 BusAddress（分配限制在 USB 1–127 地址范围）。子 PDO 的 PnP STOP/START 与 D3/D0 暂停/恢复事务队列并清理停止状态下的 pipe 配置。WMI MOF schema/resource 已嵌入镜像并通过 `mofcomp -WMI` 检查，WMI 消费者是否能发现 class 尚未在目标系统验证。常规 USB 用户态事务仍无单请求超时；完整 Host Controller 兼容性仍需逐项验证。

当前 Release 驱动源码可构建；部署包经过 Inf2Cat 检查并生成 catalog。catalog 未完成生产签名，不能作为面向公众的正式签名包发布。新驱动最低要求 Windows 10 2004（build 19041）。`replacement-package/replacement-deploy.ps1` 用于版本化 HCD 部署，`driver-source/deploy.ps1` 用于默认包设备创建/更新/移除；均需管理员权限。构建脚本会从已安装 Kits 中选择 WDK。角色划分和限制见[重建驱动说明](../drivers/usb-bus-legacy/driver-source/README.md)。当前 HCD `2.0.0.22` 已在目标机安装并完成根集线器和摄像头抓帧验证；仍不能据此认定覆盖原版的所有角色与协议。上游 `x86/`、`x64/` 二进制保持原样。反编译清单和函数分析保存在本机 `source-reconstruction/` 目录，由 `.gitignore` 排除，不属于项目源码。

## INF 中的复制与改名

INF 的 `CopyFiles` 文件列表使用“目标文件名，源文件名”语法。因此同一个 `xusb_virt.sys` 会在安装时复制成不同服务需要的目标名称；这些目标名称不是必须随包提供的额外源文件。[Microsoft CopyFiles 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-copyfiles-directive)说明了该字段顺序。

| INF 安装项 | 安装后的目标名称 | 包内源文件 |
|---|---|---|
| `xusb_bus.inf` 的 USB ONLY 安装项 | `xusb_enumbus.sys` | `xusb_virt.sys` |
| `xusb_bus.inf` 的 USB TREE 安装项 | `xusb_virt.sys` | `xusb_virt.sys` |
| `xusb_ctrl.inf` 的 Root HUB 安装项 | `xusb_roothub.sys` | `xusb_virt.sys` |
| `xusb_ctrl.inf` 的 Host Controller 安装项 | `xusb_controller.sys` | `xusb_virt.sys` |

本包为这些安装项提供了对应源文件，不能仅因目录中没有上述目标名称就判断包不完整。HCD `2.0.0.7` 重绑后曾重复触发 Code 38（`STATUS_DRIVER_FAILED_PRIOR_UNLOAD`，`0xC000038E`）；清理多个旧 INF 并重启后仍复现。为避免每次代码迭代都重启，打包器按 `DriverVer` 为每个 HCD 版本生成独立服务名和镜像名。HCD `2.0.0.10` 首次使用 `ucx_hcd_2_0_0_10` 与 `VirtualCameraUcxHcd_2_0_0_10.sys`，在同一启动周期替换失败的旧实例并启动成功。相同版本卸载/重绑仍可能遇到 Code 38，因此调试新版本时递增 `DriverVer` 并使用新服务。部署脚本按 INF 读取服务名、镜像和 catalog。2026-11-16 的 HCD `2.0.0.17` 曾枚举 `USB\\ROOT_HUB30` 但子设备报 Code 43；该历史故障已由当前 `2.0.0.22` 运行状态取代，见本文顶部验证记录。

## UCX 根集线器启动故障排查（2026-11-16）

当前测试机 HCD `2.0.0.17` 和虚拟控制器正常；子设备 `USB\ROOT_HUB30\2&87D351D&0&0` 报 Code 43（`CM_PROB_FAILED_POST_START`）。整机启动时间未变化。本次只对这一个根集线器做了一次 PnP 重启，没有重启 Windows。USBHUB3 符号状态 `0xC5B` 对应“启动时控制传输失败后发送 Hub Reset”；跟踪中可见启动阶段的端口控制错误及随后复位超限。记录到的 `STATUS_CANCELLED` 出现在该失败/清理路径中，不能据此断定它是第一个失败状态。

UCX 已成功准备根集线器并完成 USB 2.0/3.0 端口信息回调；HCD 的控制 URB 与中断回调计数仍为 0。新跟踪启用了 UCX IRP 关键字，但没有捕获到活动 URB 派发事件。因此当前能确认根集线器 PnP 启动失败，尚未确定请求为何没有进入 HCD 回调。父 HCD 仍为 OK。此前没有证据表明整机重启可解决这个启动错误；重启 Windows 会重载驱动，但不应把它作为常规调试步骤。

为避免重复安装或重启整台电脑，使用 `scripts/capture-ucx-roothub-start.ps1` 在同一次根集线器启动期间捕获 USBHUB3 与含 IRP 派发事件的 UCX 跟踪。需要管理员 PowerShell；`-RestartRootHub` 只重启指定的虚拟根集线器 PnP 设备，不重启 Windows。脚本会校验目标设备确实属于本项目的虚拟 HCD，并将 `.etl`、XML 和设备状态保存到被 Git 忽略的 `build/diagnostics/`：

```powershell
.\scripts\capture-ucx-roothub-start.ps1 `
  -RootHubInstanceId 'USB\ROOT_HUB30\2&87D351D&0&0' `
  -RestartRootHub
```

只有在准备好采集完整启动链路时才运行该命令；无需为了被动观察而重启。驱动在 `ReleaseHardware` 中清理根集线器缓存句柄和端口电源状态，以避免 PnP 停止/启动后复用旧状态。此段记录的 HCD `2.0.0.18` / tree `1.0.0.13` 是 2026-11-16 的未安装候选；当前测试机已运行 HCD `2.0.0.22` 并通过摄像头抓帧验证。后续设备更新按具体修复需要执行，不安排整机重启。

部署脚本若收到 PnPUtil 返回码 `3010`（Windows 要求重启）会立即停止并报告具体命令，不会继续执行后续设备变更，也不会自动重启。此返回码意味着刚执行的操作可能已部分改变 PnP 状态；先检查设备和 DriverStore，再决定是否继续，避免在待重启状态下反复叠加安装。

## 继续维护前的建议

- 保留现有驱动包及其哈希；确认目标 Windows 版本上的安装和加载情况。需要修改内核行为时，取得允许重建的完整驱动源码。
- 核验 INF 中每个 `CopyFiles`/`ServiceBinary` 引用、目录架构、签名和设备安装流程。确认材料后再更新本目录和此文档。
- 保持 `src/usb/ioctl.h`、SetupAPI 接口 GUID、IOCTL 结构与驱动端 ABI 一致；本仓库的旧接口代码只能帮助理解，不能代替驱动端源码。
- 在 x64 测试机上验证总线接口出现、程序启动、UVC 描述符协商、摄像头发现和卸载恢复；单纯验证签名或 C++ 构建不构成驱动验证。
- 安装前查阅 Microsoft 的[驱动签名说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing)和 [PnPUtil 命令参考](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)。不要沿用旧文档中关闭签名强制或修改系统启动策略的做法。

## 归档说明

旧版 `说明.txt`、驱动安装笔记和 `src/old-drv-interface` 已移入 `archive/legacy-2016/`，作为来源材料保留。它们描述的是 2016 年前后的环境，可能包含过时或不安全的系统级操作建议；当前操作应以本文和 Microsoft 现行文档为准。

### UCX 停止与取消路径修正

HCD `2.0.0.18` 将取消注册、事务摘链和 `WdfRequestUnmarkCancelable` 放在同一锁内；Unmark 返回 `STATUS_CANCELLED` 时，仅由取消回调安排完成，避免另一线程提前释放事务。移除工作项的计数与排空事件也在同一锁下更新。端口每次分配使用新的 generation，文件、USB 设备、端点及事务均校验该值，避免旧句柄或延迟清理在停止/再启动后误操作新设备。私有 IOCTL 的 caller-context 路径已改用 `WdfDeviceEnqueueRequest`；微软说明 `WdfRequestForwardToIoQueue` 只适用于已经从队列取出的请求，不能用作首次入队。参见 [EnqueueRequest](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdeviceenqueuerequest) 和 [ForwardToIoQueue](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestforwardtoioqueue)。

诊断响应追加到 168 字节，同时保留 64/104/132 字节读取，报告释放硬件、D0 电源和排空计数。新脚本兼容旧 HCD 的较短返回；设备移除时用户队列会被清空，因而这些状态只用于运行中及再次启动后的快照，不能保证在内核卸载卡住后仍可查询。
