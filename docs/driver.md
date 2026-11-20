# 驱动包与来源记录

## 驱动包是什么

此项目不是纯用户态虚拟摄像头。`src/usb/virt_dev.cpp` 通过 SetupAPI 查找 `GUID_XUSB_VIRT_INTERFACE`，再向虚拟 USB 总线发送 IOCTL；没有提供该接口的内核驱动时，程序会在启动阶段报 `not found virtual usb path`。

## 当前运行验证（2026-11-21）

测试机当前运行 role-3 USB tree `1.0.0.22` 和 UCX HCD `2.0.0.41`，控制器、tree 与 USB 3 Root Hub 均为 `OK`，根集线器报告 7 个端口。两包以本机测试证书签名，在同一次 Windows 启动周期内安装和绑定成功；无需重启。此前 `.36` 转储中的 `0x3B` 是延迟完成路径缺少请求上下文；`.37` 增加按需分配，`.38` 修正 bulk OUT 完成长度，`.39` 增加 USB 2.0 enable-change 状态并收紧 URB 校验。

`.41` 上 DirectShow 以 1280×720 YUY2、30fps 抓取 120 帧，用时 3.95 秒；同一应用进程通过控制管道从 1280×720 热切换到 640×480、再切回 1280×720，状态均为 `RUNNING`，两种分辨率各抓取 30 帧成功。退出应用后摄像头 PDO 消失，HCD 的 `PendingTransactions` 与 `PendingRemovalWorkItems` 均为 0。低速与全速 HID 夹具均枚举为 `HIDClass`、持续收到 interrupt-IN 并干净移除；WinUSB 夹具完成一次 512 字节 bulk IN 与 16 字节 bulk OUT，两个用户态事务均收到 END（这些合成设备探针是在 `.39` 上执行）。该 bulk 检查是短事务，不是吞吐或长时间压力测试；现有生产摄像头 responder 也不实现任意 bulk/interrupt 设备类处理。

此前 `0x144/3` 蓝屏的栈归属测试客户端：它注册 `USBD_CLIENT_CONTRACT_VERSION_602` 却使用栈分配 URB。客户端现改用 `WdfUsbTargetDeviceCreateUrb`，修正版探针取得 40 字节和 10 字节的 Microsoft OS 1.0 描述符并干净移除。Windows USB 栈在到达 HCD 前把 feature URB 转成 vendor-control 传输；未观察到 HCD 收到 direct feature URB。该事故不能据此认定生产 HCD 导致蓝屏。当前系统最近一次 BugCheck 1001 仍是该测试客户端事故；`.41/.22` 安装及采集后没有新的 BugCheck，系统启动时间保持不变。

`.40` 候选曾错误地把 `USBD_ISO_PACKET_DESCRIPTOR.Length` 当作 IN 请求容量，导致 DirectShow 采集不能完成。该字段由主控制器写入实际收到的长度；`.41` 改为按相邻包偏移计算可写缓冲区，再受端点每服务间隔上限约束，并校验设备端响应长度。该语义符合 [Microsoft 等时传输文档](https://learn.microsoft.com/windows-hardware/drivers/usbcon/transfer-data-to-isochronous-endpoints)。`.41` Release 构建、协议测试和安装包校验通过；实际 720p 采集与热分辨率往返也已通过。完整覆盖边界见[UCX HCD runtime probes](../drivers/usb-bus-legacy/ucx-driver/tests/README.md)。

### 历史摄像头链路验证（2026-11-19）

又在摄像头应用关闭、虚拟摄像头 PDO 不存在时，以管理员权限执行 pnputil /restart-device "PCI\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88\1&2D12BED1&3&1"，没有使用 /reboot。PnPUtil 退出码为 0；虚拟 HCD、USB-tree 和 USB 3 Root Hub 均重新达到 OK，UCX 初始化阶段状态均为 0。随后启动 Release 摄像头应用，DirectShow 以 1280×720 YUY2/30fps 抓取 60 帧，用时 2.004 秒；退出应用后虚拟摄像头 PDO 消失，挂起事务和移除工作项均为 0。重启后查询到的生命周期计数属于新初始化的实例，不能单独用来证明旧实例的 ReleaseHardware 回调次数。

在 Release 摄像头应用运行时，单独以管理员权限执行 pnputil /restart-device "USB\ROOT_HUB30\2&87D351D&0&0"，未重启 Windows，也未退出应用。虚拟 Root Hub 与摄像头 PDO USB\VID_BB10&PID_CC10&MI_00\4&263B9AAE&0&0000 均恢复为 OK，应用进程仍在运行；DirectShow 在重启后重新打开摄像头并抓取 60 帧 1280×720 YUY2，耗时 1.992 秒（约 30fps）。退出应用后虚拟摄像头 PDO 消失，UCX 状态查询显示 PendingTransactions=0、PendingRemovalWorkItems=0，树和 HCD 仍为 OK。这验证了单次 Root Hub PnP 重启期间的摄像头重枚举与重新打开，不覆盖睡眠/唤醒、所有电源状态切换或全部 URB 行为。

目标机 Windows 10 build 19045 以本机测试证书签名运行重建总线 `1.0.0.18` 和版本隔离 HCD `2.0.0.34`。控制器、USB 3 根集线器和 SuperSpeed 摄像头节点状态均为 OK。FFmpeg/DirectShow 单路抓取 720p YUY2 120 帧耗时 3.93 秒（约 30.5fps）；双路同时抓取时每路 120 帧耗时 3.96 秒（约 30.3fps/路）。应用退出后摄像头 PDO 消失，HCD 挂起事务和移除工作项均归零。更新、抓取和清理都在同一 Windows 启动周期完成。

FFmpeg/DirectShow 以 1280×720、30fps、YUY2 单路抓取 120 帧，耗时 3.93 秒（约 30.5fps）。随后同一应用同时枚举两台摄像头，并在一个 FFmpeg 进程中打开两路 720p YUY2；每路 120 帧在 3.96 秒内完成（约 30.3fps/路）。运行时 UVC SuperSpeed 端点描述符按 8,192 字节/125 微秒报告间隔预算；每路 720p YUY2 30fps 需要约 55.3 MB/s。USB 2.0 高速等时端点的理论有效负载约 24.6 MB/s，对应 720p YUY2 约 13.3fps；压缩 MJPEG 可降低 USB 2.0 带宽需求。这些是软件虚拟总线上的端到端测试，不是物理 USB 3 链路测速。

HCD `2.0.0.34` 还按 USB 端点 `bInterval` 为低/全速中断请求设置最早可响应时间。低速和全速 HID 夹具均枚举为 `HIDClass` 且状态为 `OK`；此前 5 秒观察分别收到 498（低速）和 495（全速）次 interrupt-IN 请求，移除通知分别在 772ms 和 780ms 内到达。多次运行约为 100 次/秒。移除后 PnP 节点消失。夹具发送零填充键盘报告，不代表真实键盘输入测试。关闭相机应用后 HCD 的挂起事务和移除工作项为 0。后续 `.39` 的 HID 与 bulk 结果见当前验证记录。可复现命令见[UCX HCD runtime probes](../drivers/usb-bus-legacy/ucx-driver/tests/README.md)。

另验证了控制管道热更新分辨率：同一应用进程从 1280×720 切到 640×480，再恢复 1280×720；每次都返回 `REOPEN_REQUIRED`，`STATUS` 显示新分辨率且状态为 `RUNNING`。每次更新后重新打开 DirectShow 摄像头，720p 抓取 30 帧约 0.99 秒，480p 抓取 60 帧约 1.99 秒，恢复 720p 后抓取 30 帧约 0.99 秒，均约 30fps。应用正常退出后摄像头 PDO 消失，HCD 挂起事务和移除工作项均归零，未重启系统。

2026-11-19 又以 `pnputil /restart-device "ROOT\SYSTEM\0003"` 重启虚拟设备树，未使用 `/reboot`，PnPUtil 报告成功。树驱动 `1.0.0.18`、HCD `2.0.0.34` 与其 `ROOT_HUB30` 均重新达到 `OK`。随后重新启动 Release 用户态程序，DirectShow 再次枚举 `Pipe HD Camera 1`，以 1280×720 YUY2 30fps 抓取 60 帧耗时 1.98 秒（约 30fps）。停止程序后虚拟摄像头 PDO 消失，HCD 挂起事务和移除工作项为 0；两台物理摄像头保持正常。该结果覆盖一次树设备 PnP 重启后的重枚举与抓流，不代表 Windows 睡眠/唤醒或所有 D3/D0 转换均已验证。

此验证证明的是重建软件 HCD 上的虚拟 USB 摄像头完整枚举和抓帧链路，不是物理 USB 3 控制器带宽测量。它也不证明所有原版驱动角色、URB 家族或生产签名包完全可替代。HCD catalog 尚未完成生产签名，不能作为面向公众的正式签名包发布。更多使用命令见[运行说明](usage.md)。

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

当前已在目标 Windows 上通过虚拟根集线器运行用户态摄像头，Windows UVC 驱动可协商并抓取 YUY2 与 MJPEG 等时流。HCD `2.0.0.41` 保留端点 `bInterval` 周期中断调度、USB 2.0 低/全速 PDO 添加 IOCTL 及按速度校验 EP0/端点参数；相关 ABI 有静态断言并通过 Release 构建。低速和全速 HID 夹具已枚举为 HIDClass，验证 interrupt-IN 与移除清理；合成 WinUSB 夹具也完成了一次短 bulk IN 和 bulk OUT 往返（该组探针在 `.39` 上运行）。夹具发送零填充键盘报告，不代表真实键盘输入或所有 HID 兼容性。低速仅允许控制与中断端点，全速支持控制、bulk、中断与等时端点。尚未覆盖的边界包括生产摄像头用户态程序对 bulk/interrupt 类设备的数据处理、HCD 直接 Microsoft OS feature URB 映射、所有多端口/电源/链路行为、所有 Host Controller URB 家族和生产签名发布。重建版实现 USB bus type 查询，并在 Root Hub FDO 本地响应 Hub name、节点描述符/能力、连接信息、设备描述符转发、DriverKeyName 与 `IOCTL_INTERNAL_USB_GET_HUB_COUNT`；descriptor proxy 等待用户态最多 5 秒后取消。Host Controller 返回 Root Hub PDO/栈顶，子 PDO 根据 present 状态响应 `IOCTL_INTERNAL_USB_GET_PORT_STATUS`。USBDI 查询按原版提供 V0–V3 接口槽和回调返回值；PDO 处理设备文本和 usage notification，明确不支持 Eject，并复用用户态建议的 BusAddress（分配限制在 USB 1–127 地址范围）。子 PDO 的 PnP STOP/START 与 D3/D0 暂停/恢复事务队列并清理停止状态下的 pipe 配置。WMI MOF schema/resource 已嵌入镜像并通过 `mofcomp -WMI` 检查，WMI 消费者是否能发现 class 尚未在目标系统验证。常规 USB 用户态事务仍无单请求超时；完整 Host Controller 兼容性仍需逐项验证。

当前 Release 驱动源码可构建；部署包通过 InfVerif/Inf2Cat 并生成 catalog。catalog 未完成生产签名，不能作为面向公众的正式签名包发布。新驱动最低要求 Windows 10 2004（build 19041）。构建脚本会自动选择具备 UCX 1.1/KMDF 1.31 的 WDK，也可显式指定版本。`replacement-package/replacement-deploy.ps1` 用于版本化 HCD 部署，`driver-source/deploy.ps1` 用于默认包设备创建/更新/移除；均需管理员权限并会阻止同名镜像冲突，不自动重启。测试机当前以本机测试证书运行 HCD `2.0.0.41` 与 role-3 `1.0.0.22`；Release 摄像头程序已完成单路 720p30 120 帧采集和 720p→480p→720p 热分辨率往返。HCD `.34` 上的单/双路 720p30 和 `.39` 上的 HID、bulk 合成夹具结果是先前版本验证，不能视为 `.41` 上的同版本复测。夹具只发送零填充报告或固定测试 payload。当前用户态程序仍只选择 USB 2.0 高速或 SuperSpeed。HCD 的 bulk/interrupt 桥接已用测试 responder 端到端验证，但生产用户态 handler 尚无对应设备类数据处理实现。重建驱动仍未覆盖原版所有角色、URB 和 Root Hub 行为。角色划分和限制见[重建驱动说明](../drivers/usb-bus-legacy/driver-source/README.md)。上游 `x86/`、`x64/` 二进制保持原样。反编译清单和函数分析保存在本机 `source-reconstruction/` 目录，由 `.gitignore` 排除，不属于项目源码。

`replacement-deploy.ps1 -Action Cleanup` 可在不重新安装驱动的情况下清理旧的重建 role-3/UCX 包。它要求当前树设备和控制器处于 `OK`，保留活动及更新的树版本、任何当前 PnP 节点正在使用的 INF，并且只用不带 `/uninstall`、`/force` 的 PnPUtil 删除旧包；删除前会先将包导出到 Git 忽略目录。若 Windows 拒绝删除，脚本保留该包并继续处理；若 PnPUtil 报告需重启，则立即停止，不会自动重启。

## INF 中的复制与改名

INF 的 `CopyFiles` 文件列表使用“目标文件名，源文件名”语法。因此同一个 `xusb_virt.sys` 会在安装时复制成不同服务需要的目标名称；这些目标名称不是必须随包提供的额外源文件。[Microsoft CopyFiles 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-copyfiles-directive)说明了该字段顺序。

| INF 安装项 | 安装后的目标名称 | 包内源文件 |
|---|---|---|
| `xusb_bus.inf` 的 USB ONLY 安装项 | `xusb_enumbus.sys` | `xusb_virt.sys` |
| `xusb_bus.inf` 的 USB TREE 安装项 | `xusb_virt.sys` | `xusb_virt.sys` |
| `xusb_ctrl.inf` 的 Root HUB 安装项 | `xusb_roothub.sys` | `xusb_virt.sys` |
| `xusb_ctrl.inf` 的 Host Controller 安装项 | `xusb_controller.sys` | `xusb_virt.sys` |

本包为这些安装项提供了对应源文件，不能仅因目录中没有上述目标名称就判断包不完整。HCD `2.0.0.7` 重绑后曾重复触发 Code 38（`STATUS_DRIVER_FAILED_PRIOR_UNLOAD`，`0xC000038E`）；清理多个旧 INF 并重启后仍复现。为避免每次代码迭代都重启，打包器按 `DriverVer` 为每个 HCD 版本生成独立服务名和镜像名。HCD `2.0.0.10` 首次使用 `ucx_hcd_2_0_0_10` 与 `VirtualCameraUcxHcd_2_0_0_10.sys`，在同一启动周期替换失败的旧实例并启动成功。相同版本卸载/重绑仍可能遇到 Code 38，因此调试新版本时递增 `DriverVer` 并使用新服务。部署脚本按 INF 读取服务名、镜像和 catalog。2026-11-16 的 HCD `2.0.0.17` 曾枚举 `USB\\ROOT_HUB30` 但子设备报 Code 43；该历史故障已由当前 HCD `2.0.0.41` 运行状态取代，见本文顶部验证记录。该段末尾的 `.34`/`.18` 版本号是 2026-11-19 的旧快照。

## UCX 根集线器启动故障排查（2026-11-16）

当前测试机 HCD `2.0.0.17` 和虚拟控制器正常；子设备 `USB\ROOT_HUB30\2&87D351D&0&0` 报 Code 43（`CM_PROB_FAILED_POST_START`）。整机启动时间未变化。本次只对这一个根集线器做了一次 PnP 重启，没有重启 Windows。USBHUB3 符号状态 `0xC5B` 对应“启动时控制传输失败后发送 Hub Reset”；跟踪中可见启动阶段的端口控制错误及随后复位超限。记录到的 `STATUS_CANCELLED` 出现在该失败/清理路径中，不能据此断定它是第一个失败状态。

UCX 已成功准备根集线器并完成 USB 2.0/3.0 端口信息回调；HCD 的控制 URB 与中断回调计数仍为 0。新跟踪启用了 UCX IRP 关键字，但没有捕获到活动 URB 派发事件。因此当前能确认根集线器 PnP 启动失败，尚未确定请求为何没有进入 HCD 回调。父 HCD 仍为 OK。此前没有证据表明整机重启可解决这个启动错误；重启 Windows 会重载驱动，但不应把它作为常规调试步骤。

为避免重复安装或重启整台电脑，使用 `scripts/capture-ucx-roothub-start.ps1` 在同一次根集线器启动期间捕获 USBHUB3 与含 IRP 派发事件的 UCX 跟踪。需要管理员 PowerShell；`-RestartRootHub` 只重启指定的虚拟根集线器 PnP 设备，不重启 Windows。脚本会校验目标设备确实属于本项目的虚拟 HCD，并将 `.etl`、XML 和设备状态保存到被 Git 忽略的 `build/diagnostics/`：

```powershell
.\scripts\capture-ucx-roothub-start.ps1 `
  -RootHubInstanceId 'USB\ROOT_HUB30\2&87D351D&0&0' `
  -RestartRootHub
```

只有在准备好采集完整启动链路时才运行该命令；无需为了被动观察而重启。驱动在 `ReleaseHardware` 中清理根集线器缓存句柄和端口电源状态，以避免 PnP 停止/启动后复用旧状态。此段记录的 HCD `2.0.0.18` / tree `1.0.0.13` 是 2026-11-16 的未安装候选；当前测试机已运行 HCD `2.0.0.34` 与 tree `1.0.0.18` 并通过双摄像头抓帧验证。后续设备更新按具体修复需要执行，不安排整机重启。

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
