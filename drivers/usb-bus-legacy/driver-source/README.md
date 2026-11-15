# 重建版 WDM 驱动

本目录是独立的 x64 WDM 重建工程。`x64/`、`x86/` 中的 2016 年二进制包保持原样；本目录的构建与安装包不覆盖旧包，也不改用户态 ABI。反编译基线保存在被 Git 忽略的 `source-reconstruction/`。

## 角色与替换范围

| 原包角色 | 原包设备 ID | 重建版行为 | 默认安装包 |
|---|---|---|---|
| USB-only enumerator (0) | `root\*xusb_enumbus`；原二进制将其他未匹配 ID 也归入 role 0 | 应用控制接口；添加/移除子 PDO、BusRelations、事务队列 | 是 |
| Root Hub (1) | `vFxsUSB\vRootHUB&Virtual&Fanxiushu` | Hub 与应用接口；基础子 PDO 枚举、Hub name、部分内部请求 | 否，实验包 |
| Host Controller (2) | `PCI\VEN_37FA&DEV_FE09&SUBSYS_9976A077&REV_88` | 仅重建少数请求和 Root Hub PDO 查找 | 否，实验包 |
| USB-tree enumerator (3) | `root\*xusb_tree` | 创建内部 Host Controller PDO | 否，实验包 |

现有 UVC 程序走 USB-only 枚举器：通过冻结的 `GUID_XUSB_VIRT_INTERFACE` 和四个旧 IOCTL 添加/移除设备、提交 BEGIN/END 事务。旧 IOCTL 数值与结构保持在 `src/usb/ioctl.h`，新驱动额外提供只读 `IOCTL_XUSB_QUERY_DRIVER_INFO` 用于标识 FOSS 构建。

子 PDO 支持当前 UVC 活跃路径所需的控制/描述符/等时 URB，并将请求映射到旧用户态事务 ABI。`IRP_MN_QUERY_INTERFACE` 现在提供 USBDI V0：版本、总线时间、基础带宽信息和接口引用；isochronous OUT 按 WDK 接口定义返回 `STATUS_NOT_SUPPORTED`。配置与接口选择会校验描述符；合成 pipe 句柄含代际信息。等时包边界和返回长度会校验。

原二进制的四个角色均有对应源码分支和硬件 ID 识别，但这不代表四角色都能替代旧驱动。Host Controller 还不是完整 Usbport/UCX HCD；USB-tree 依赖该未完成角色；Root Hub 的 hub/port 内部请求也只覆盖已实现部分。因此默认包只绑定当前应用使用的 USB-only role 0，其余三种角色独立放在实验包。当前用户态 handler 还未实现 bulk/interrupt 与 Microsoft OS feature descriptor 的完整处理。

## 安装包与构建

安装 INF 位于 `package/`，未完成角色 INF 位于 `package/experimental/`。`package.ps1` 会先进行 Release/Debug WDK 构建，将一个 `.sys` 和相应 INF stage 到 Git 忽略的 `build/package/x64/<Configuration>/`，然后运行 Inf2Cat。默认包只对应当前应用使用的 USB-only enumerator；显式给 `-IncludeExperimentalRoles` 才会把 Root Hub、USB-tree 和 Host Controller INF 加入包。

```powershell
./build.ps1 -Configuration Release
./build.ps1 -Configuration Debug
./package.ps1 -Configuration Release
./package.ps1 -Configuration Release -IncludeExperimentalRoles
```

工程使用 WDM x64，要求 Visual Studio C++/WDK 组件与匹配的 Windows SDK/WDK。`-Clean` 只清理此目录的忽略构建产物。`package.ps1` 的 Inf2Cat 检查会验证 INF 可签名性并生成 catalog；catalog 仍需完成 Windows 内核驱动发布签名。源码仓库不含发布私钥；面向当前 Windows 的公开分发需通过 Microsoft Hardware Dev Center 的签名流程，测试证书只适用于隔离测试机。

包只安装/匹配已有 PnP 设备节点，不会自动创建 `root\*xusb_enumbus` 根设备实例；部署器还需通过 SetupAPI 或 DevCon 创建该根枚举设备。本仓库目前没有执行驱动安装的部署器。

不要把新 `.sys` 手工覆盖到旧 INF 或旧 catalog。驱动包验证通过也不代表运行时已验证。任何签名包都需要先在可恢复的隔离 Windows 测试机上验证安装、PnP 枚举、UVC 描述符/控制请求、持续等时流、移除/重连；本机尚未完成这条端到端验证，也没有安装驱动或更改启动签名策略。
