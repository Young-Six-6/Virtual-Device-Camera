# 驱动包与来源记录

## 驱动包是什么

此项目不是纯用户态虚拟摄像头。`src/usb/virt_dev.cpp` 通过 SetupAPI 查找 `GUID_XUSB_VIRT_INTERFACE`，再向虚拟 USB 总线发送 IOCTL；没有提供该接口的内核驱动时，程序会在启动阶段报 `not found virtual usb path`。

目录 `drivers/usb-bus-legacy/` 保留了上游仓库提供的 x86/x64 包。x64 目录实际包含 `xusb_bus.inf`、`xusb_ctrl.inf`、`xusb_virt.cat` 和 `xusb_virt.sys`；这些文件没有在重构中丢失。它们是旧版二进制包，不是当前构建的一部分。

## 可核实的来源链

1. 当前仓库的初始导入提交注明它从 [`jasonsalex/Virtual-Device-Camera`](https://github.com/jasonsalex/Virtual-Device-Camera) fork；该上游 x64 目录包含同名的四个文件。[当前仓库的导入提交](https://github.com/Young-Six-6/Virtual-Device-Camera/commit/d37f201cb120a368be7905ccf3d236a583a0838a)和[上游 x64 驱动目录](https://github.com/jasonsalex/Virtual-Device-Camera/tree/master/%E8%99%9A%E6%8B%9F%E7%AB%AF%E9%A9%B1%E5%8A%A8-%E6%9C%80%E6%96%B0-2016-10%E6%9C%88%E4%BB%BD/x64)可供核对。
2. 归档的项目原说明称 `src/old-drv-interface` 是 Fanxiushu 的 CSDN 资源 [9538188](https://download.csdn.net/detail/fanxiushu/9538188) 中的驱动接口部分，并建议从该资源获取完整驱动源码。该页面目前无法从本次研究环境访问；原说明也没有给出可验证的源码版本或哈希，因此无法确认仓库里的二进制是由该源码版本编译而来。旧说明和旧接口代码保存在 `archive/legacy-2016/`。
3. 本地 `x64/xusb_virt.sys` 的 PE 版本资源标注 `CompanyName=Fanxiushu`、`ProductName=USB Remote Access Driver`、`OriginalFilename=xfs_redir.sys`、`FileVersion=1.0.0.1` 和 `Fanxiushu 2015-2016` 版权信息。Windows `Get-AuthenticodeSignature` 在检查时返回 `Valid`；签名证书主体为上海风创信息咨询有限公司。签名状态本身不能证明驱动适配当前 Windows，也不能证明安装包完整。
4. Fanxiushu 的公开 GitHub 项目还包括 [`xFsRedir`](https://github.com/fanxiushu/xFsRedir)、[`xdisp_virt`](https://github.com/fanxiushu/xdisp_virt) 和 [`vcam_dshow`](https://github.com/fanxiushu/vcam_dshow)。`xdisp_virt` 当前公开的 Windows 虚拟摄像头包使用 `avstrm_virt`，与这里的自定义 USB 总线驱动不同；`xFsRedir` 是文件/目录重定向项目。`xusb_virt.sys` 内部的 `xfs_redir.sys` 原文件名是一个可追踪线索，但现有材料不足以证明它与这些项目中的驱动源码同源。

x64 `xusb_virt.sys` 大小为 51,368 字节；其 Git blob SHA-1 为 `fb62e682170ec53b570bb7dbd1fd2ac9b2ab85b1`，与 `jasonsalex` 上游目录和本仓库初始提交中的文件一致。当前工作区 SHA-256 为 `D3036053F37E3A7377FD273459439E0DF0E2780FE6E008D45CD48E0E7752B417`。这些值能验证分发副本没有在重构时被替换，不等于证明 CSDN 下载源文件的哈希相同。

因此，目前可追溯到“原项目文档引用的 CSDN 资源、同名 GitHub 分发副本和二进制内嵌作者/签名信息”；仓库没有完整驱动源码，也没有足够证据把二进制映射到某个可重建的公开源码提交。驱动二进制的独立许可条款也未在包内提供，不能仅凭应用程序的 MIT `LICENSE` 推断驱动许可。

`drivers/usb-bus-legacy/driver-source/` 是独立的新 WDM 源码工程，不会覆盖旧二进制包。原包四种角色及当前复刻状态见[重建驱动说明](../drivers/usb-bus-legacy/driver-source/README.md)：默认包包含应用使用的 USB-only enumerator 与用于更新旧应用控制接口提供者的 Root Hub；Host Controller/USB-tree 仍隔离在实验包。原二进制在角色选择中将未匹配 ID 归入 USB-only role 0；重建版已保留此回退行为，INF 仍只匹配受支持的已知 ID。

用户态通过 `IOCTL_XUSB_QUERY_DRIVER_INFO` 探测重建驱动。FOSS 版本返回带 `FOSS` 签名的版本结构；旧原版不识别这个私有查询码，用户态将其标记为“原版驱动”。既有四个 USB IOCTL 编号和结构保持不变。Web UI 的驱动状态卡也会显示识别结果。

- 父设备按硬件 ID 识别 USB-only、USB-tree、Root HUB 和 Host Controller 角色。已按旧二进制补齐角色设备接口：USB-only 使用应用控制接口，Root HUB 使用标准 USB HUB 接口并额外提供应用控制接口，Host Controller 使用标准主控制器接口。USB-only 与 Root HUB 可通过控制接口新增/移除子 PDO 并提供 `BusRelations`；Host Controller 与 USB-tree 启动时分别创建并经 `BusRelations` 枚举 Root HUB 和 Host Controller 内部 PDO；USB-tree 没有用户控制接口。内部 PDO 的标识和基础关系已实现。
- 子 PDO 经 cancel-safe queue 转发旧协议支持的控制/控制扩展、设备/接口/端点描述符、vendor/class、status、feature、pipe reset、frame number、Microsoft feature descriptor、配置/接口选择、bulk/interrupt 和等时 URB。`RESET_PORT`/`CYCLE_PORT` 也映射到旧 BEGIN/END 协议。
- 配置/接口选择会校验当前配置。Pipe 和 interface 句柄带有代际信息，避免 alternate setting 切换后旧句柄误指向新端点。每个父设备最多创建 256 个子 PDO；每个子 PDO 最多允许 256 个事务和 64 MiB 事务预算，父设备总上限为 1024 个事务和 256 MiB。
- 子 PDO 使用从旧二进制恢复的 `IoCreateDeviceSecure` 默认 SDDL 和 class GUID：SY 完全访问，BA/WD/RC 读写执行；相同 class GUID 保留 Windows 类注册表安全覆盖机制。子 PDO 处理基础 query/set power IRP。父 FDO 已实现 WMI provider 注册和 8 字节数据块回调。

协议路径打通不代表与仓库当前用户态服务兼容。现有 UVC handler 尚未实现 bulk/interrupt 传输或 Microsoft OS feature descriptor 的完整处理，因此这些路径不能据此认定可用。驱动已兼容该 handler 对 `GET_CURRENT_FRAME_NUMBER` 返回的 16 位值，并按 URB 需要扩展为 32 位。重建版实现 USB bus type 查询，并在 Root Hub FDO 本地响应 Hub name、节点描述符/能力、连接信息、设备描述符转发、DriverKeyName 与 `IOCTL_INTERNAL_USB_GET_HUB_COUNT`；descriptor proxy 等待用户态最多 5 秒后取消。Host Controller 返回 Root Hub PDO/栈顶，子 PDO 根据 present 状态响应 `IOCTL_INTERNAL_USB_GET_PORT_STATUS`。USBDI 查询现按原版提供 V0–V3 接口槽和回调返回值；PDO 处理设备文本和 usage notification，明确不支持 Eject，并复用用户态建议的 BusAddress（分配限制在 USB 1–127 地址范围）。子 PDO 的 PnP STOP/START 与 D3/D0 现暂停/恢复事务队列并清理停止状态下的 pipe 配置。Root Hub 仍缺少 EX_V2 连接信息、扩展状态和完整多端口/电源行为。驱动已将 WMI MOF schema/resource 嵌入镜像并通过 `mofcomp -WMI` 检查，WMI 消费者发现 class 的行为仍未在目标系统验证。常规 USB 用户态事务仍无单请求超时。完整 USB Host Controller 协议、父 FDO 控制接口的最终访问策略以及 Windows USB/PnP/摄像头端到端运行均待验证。

当前 Release 驱动源码已构建，默认 x64 包已由 Inf2Cat 完成 Windows 10 20H2 与 Windows 11 24H2 signability 检查并生成两个 catalog；catalog 未签名，无法作为生产包直接部署。新驱动最低要求 Windows 10 2004（build 19041）。`driver-source/deploy.ps1` 负责签名包的设备创建/更新/移除，需管理员运行并从 WDK 获取 DevCon。构建脚本会自动选取已安装的 WDK 10.0.19041.0 或更新版本。新包的构建命令、角色隔离和验证边界见[重建驱动说明](../drivers/usb-bus-legacy/driver-source/README.md)。这仍是替换候选，尚未证明可全功能替代原驱动：Root Hub/Host Controller/USB-tree 协议尚不完整，且未在目标 Windows 设备上完成安装、摄像头流和热插拔验证。上游 `x86/`、`x64/` 二进制保持原样。反编译清单和函数分析保存在本机 `source-reconstruction/` 目录，由 `.gitignore` 排除，不属于项目源码。

## INF 中的复制与改名

INF 的 `CopyFiles` 文件列表使用“目标文件名，源文件名”语法。因此同一个 `xusb_virt.sys` 会在安装时复制成不同服务需要的目标名称；这些目标名称不是必须随包提供的额外源文件。[Microsoft CopyFiles 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-copyfiles-directive)说明了该字段顺序。

| INF 安装项 | 安装后的目标名称 | 包内源文件 |
|---|---|---|
| `xusb_bus.inf` 的 USB ONLY 安装项 | `xusb_enumbus.sys` | `xusb_virt.sys` |
| `xusb_bus.inf` 的 USB TREE 安装项 | `xusb_virt.sys` | `xusb_virt.sys` |
| `xusb_ctrl.inf` 的 Root HUB 安装项 | `xusb_roothub.sys` | `xusb_virt.sys` |
| `xusb_ctrl.inf` 的 Host Controller 安装项 | `xusb_controller.sys` | `xusb_virt.sys` |

本包为这些安装项提供了对应源文件，不能仅因目录中没有上述目标名称就判断包不完整。当前 Windows 版本上的安装、加载和设备工作情况仍须实机验证；本次没有安装驱动或更改启动设置。

## 继续维护前的建议

- 保留现有驱动包及其哈希；确认目标 Windows 版本上的安装和加载情况。需要修改内核行为时，取得允许重建的完整驱动源码。
- 核验 INF 中每个 `CopyFiles`/`ServiceBinary` 引用、目录架构、签名和设备安装流程。确认材料后再更新本目录和此文档。
- 保持 `src/usb/ioctl.h`、SetupAPI 接口 GUID、IOCTL 结构与驱动端 ABI 一致；本仓库的旧接口代码只能帮助理解，不能代替驱动端源码。
- 在 x64 测试机上验证总线接口出现、程序启动、UVC 描述符协商、摄像头发现和卸载恢复；单纯验证签名或 C++ 构建不构成驱动验证。
- 安装前查阅 Microsoft 的[驱动签名说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing)和 [PnPUtil 命令参考](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)。不要沿用旧文档中关闭签名强制或修改系统启动策略的做法。

## 归档说明

旧版 `说明.txt`、驱动安装笔记和 `src/old-drv-interface` 已移入 `archive/legacy-2016/`，作为来源材料保留。它们描述的是 2016 年前后的环境，可能包含过时或不安全的系统级操作建议；当前操作应以本文和 Microsoft 现行文档为准。
