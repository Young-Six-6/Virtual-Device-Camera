# 重建版 WDM 驱动

本目录是独立的 x64 WDM 重建工程。`x64/`、`x86/` 中的 2016 年二进制包保持原样；本目录的构建与安装包不覆盖旧包，也不改用户态 ABI。反编译基线保存在被 Git 忽略的 `source-reconstruction/`。

## 角色与替换范围

| 原包角色 | 原包设备 ID | 重建版行为 | 默认安装包 |
|---|---|---|---|
| USB-only enumerator (0) | `root\*xusb_enumbus`；原二进制将其他未匹配 ID 也归入 role 0 | 应用控制接口；添加/移除子 PDO、BusRelations、事务队列 | 是 |
| Root Hub (1) | `vFxsUSB\vRootHUB&Virtual&Fanxiushu` | Hub 与应用接口；16 个连接索引、Hub name、节点/能力/连接信息查询、部分内部请求 | 是，应用接口替换 |
| Host Controller (2) | `PCI\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88` | 仅重建少数请求和 Root Hub PDO 查找 | 否，实验包 |
| USB-tree enumerator (3) | `root\*xusb_tree` | 创建内部 Host Controller PDO | 否，实验包 |

现有 UVC 程序走 USB-only 枚举器：通过冻结的 `GUID_XUSB_VIRT_INTERFACE` 和四个旧 IOCTL 添加/移除设备、提交 BEGIN/END 事务。旧 IOCTL 数值与结构保持在 `src/usb/ioctl.h`，新驱动额外提供只读 `IOCTL_XUSB_QUERY_DRIVER_INFO` 用于标识 FOSS 构建。

USB-tree role 创建的内部 Host Controller PDO 报告项目专用软件总线 GUID，`LegacyBusType` 为 `PNPBus`；它与 UCX controller config 的 `UcxControllerParentBusTypeCustom` 对齐。USB-only、Root Hub 及普通 USB 子 PDO 继续报告标准 USB bus GUID。

子 PDO 将控制、描述符、等时、bulk/interrupt、Microsoft OS feature descriptor 请求映射到冻结的旧用户态事务 ABI；bulk/interrupt 方向必须与所选 pipe 的端点方向一致，UCX 路径按 `bInterval` 排队 interrupt 请求。`IRP_MN_QUERY_INTERFACE` 按旧二进制的布局提供 USBDI V0–V3，结构大小逐版校验；版本号、USB 2.0 标记和各回调结果与旧回调对齐。原版对应的 QueryBusTime/BusInformation/isochronous OUT 等回调本身就是失败 stub，新实现保留其返回状态。PDO 还处理设备文本、usage notification、ContainerID fallback 和移除状态；它声明不支持 Eject，由现有用户态 remove IOCTL 触发动态移除。PnP 查询停止、停止、取消停止、D3/D0 使用可恢复的事务 quiesce 状态：停止期间拒绝新 URB、取消挂起事务，START/D0 恢复接收；STOP 清空选中配置和 pipe 句柄。配置与接口选择会校验描述符；合成 pipe 句柄含代际信息。等时包边界和返回长度会校验。

冻结事务 ABI 对 bulk/interrupt 使用 wire type 3、transfer type 2；`EndpointAddress` 是 pipe 地址，`IsRead` 来自 URB 方向，payload 是 OUT 请求数据或 IN 回复数据。Microsoft OS feature descriptor 使用 wire type 1、descriptor type 5；`Index` 低 16 位保持现有 `Recipient << 8 | InterfaceNumber` 编码，高 16 位携带 `MS_PageIndex`，`Value` 携带 `MS_FeatureDescriptorIndex`。这只是驱动到 responder 的传输约定，不代表当前 UVC responder 已实现这些功能。

用户态在 PDO_ADD 中把上次返回的 BusAddress 作为建议值传回。新驱动会在该地址空闲时复用它，从而让同一摄像头断开/重连保持相同的 PnP instance ID；若地址仍被占用则重新分配。地址分配遵循 USB 地址范围 1–127。当前 ABI 没有设备序列号/稳定唯一 ID，驱动不伪造硬件 ContainerID，让 PnP 依据可移除设备能力生成容器 ID。

Root Hub role 1 现在报告原包的 16-port descriptor/connection-index ABI，并按固定槽位查询子 PDO；这是源码与 Release 构建验证，当前 UCX 测试机没有安装这个 WDM Root Hub role，因此尚无运行时验证。节点能力、连接/设备描述符、DriverKeyName 及基础 EX/EX_V2 信息已实现；WDM Root Hub 的完整端口变化、电源与异常状态行为仍未完整重建。UCX 路径已增加 USB 2 `C_PORT_ENABLE` 状态与通知；新 URB 限制需用实际 Root Hub 请求序列验证后再启用。

原摄像头 responder 只声明 control/isochronous UVC endpoints，也没有 Microsoft OS string `0xEE`；它没有处理 bulk/interrupt 数据或 descriptor type 5。因此这些 wire transaction 不会挂住，未知请求会以失败回复收尾，但不能宣称现有摄像头应用已提供通用 bulk/interrupt 或 Microsoft OS descriptor 服务。补充这些服务需要设备 responder 自己提供 endpoint 数据和对应 descriptor，不能由总线驱动推断或伪造。默认包仍绑定 USB-only role 0，并包含用于更新旧应用控制接口提供者的 Root Hub role 1；UCX Host Controller 与 USB-tree 隔离在实验包。

## 安装包与构建

安装 INF 位于 `package/`，未完成角色 INF 位于 `package/experimental/`。`package.ps1` 会先进行 Release/Debug WDK 构建，再以 UTC 时间槽生成包版本、服务名和 SYS 文件名，将版本化镜像与 INF stage 到 Git 忽略的 `build/package/x64/<Configuration>/`，随后运行 InfVerif 与 Inf2Cat。默认包包含 USB-only enumerator 和 Root Hub；显式给 `-IncludeExperimentalRoles` 才会把 USB-tree 和 Host Controller INF 加入包。

```powershell
./build.ps1 -Configuration Release
./build.ps1 -Configuration Debug
./package.ps1 -Configuration Release
./package.ps1 -Configuration Release -IncludeExperimentalRoles
# 显式选择 Inf2Cat 当前支持的目标 OS 标识
./package.ps1 -Configuration Release -OsTargets 10_VB_X64,10_GE_X64
```

工程使用 WDM x64，要求 Visual Studio C++/WDK 组件以及 10.0.19041.0 或更新的 WDK。驱动运行最低要求为 Windows 10 版本 2004（build 19041），INF 会拒绝更早的系统；这是因为内核使用 `ExAllocatePool2`。`build.ps1` 会从已安装 Kits 中选择版本最高且包含 x64 kernel headers/libraries 的 WDK，不再绑定某一台机器上的单个版本。`-Clean` 只清理此目录的忽略构建产物。`package.ps1` 会从当前 Inf2Cat 支持列表选择 Windows 10 20H2 x64 与最新的 Windows 11 x64 目标，也可通过 `-OsTargets` 显式覆盖。Inf2Cat 检查会验证 INF 可签名性并生成 catalog；catalog 仍需完成 Windows 内核驱动发布签名。源码仓库不含发布私钥；面向当前 Windows 的公开分发需通过 Microsoft Hardware Dev Center 的签名流程，测试证书只适用于隔离测试机。

签名后的包可由 `deploy.ps1` 安装或升级；部署器会校验 catalog 可信状态、INF 版本/服务/镜像一致性以及同名镜像 SHA-256，再用 PnPUtil 暂存 INF，调用 WDK 的 DevCon 创建/更新 USB-only 根节点，并确认 PnP 最终绑定到请求的版本服务。不同版本有独立服务和镜像，部署器拒绝覆盖可能仍映射在内存中的旧镜像，不请求系统重启。关掉摄像头程序后，在管理员 PowerShell 执行：

```powershell
./deploy.ps1 -PackageDirectory .\build\package\x64\Release
# 已有旧版 USB-only 根设备时升级其驱动绑定
./deploy.ps1 -PackageDirectory .\build\package\x64\Release -UpdateExisting
# 移除根设备实例（保留 Driver Store 包）
./deploy.ps1 -Remove
```

默认包面向当前应用的两个 `GUID_XUSB_VIRT_INTERFACE` 提供角色。更新/卸载后仍需确认没有额外旧的接口提供者，避免用户态枚举到未更新的驱动实例。部署脚本不会删除 Driver Store 中的旧包。

不要把新 `.sys` 手工覆盖到旧 INF 或旧 catalog。此默认双角色包的完整性能数据来自 role-3 `1.0.0.18` 与 HCD `2.0.0.34`：单路 720p YUY2 120 帧用时 3.93 秒，两路同时采集时每路 120 帧用时 3.96 秒（约 30fps）；低/全速 HID 夹具也完成 PnP 枚举、周期 interrupt-IN 和移除清理。当前测试机运行的是单独维护的替换包 role-3 `1.0.0.22` 与 UCX HCD `2.0.0.41`，已完成单路 720p30 120 帧及热分辨率往返验证。HCD 更新使用独立服务名与镜像名，并在 `.33`→`.34` 和 `.40`→`.41` 安装中无需重启；相同版本卸载/重绑仍未作为常规工作流验证。当前验证不代表覆盖所有 USB URB、卸载/重载和原版所有角色；实验 Host Controller/USB-tree 角色的剩余范围见本目录的 USB 3.0 与限制说明。实验包不能视为通用 USB 主控驱动或物理 USB 3 带宽测试。
