# 重建版 WDM 驱动源码

这是独立的新驱动源码目录，与保留的 2016 年驱动包及本机反编译分析目录分开。旧 INF 没有指向此目录，目录中构建出的 `.sys` 也不是旧驱动的替代品。

## 当前实现

该目录提供一个独立的 x64 WDM 源码工程。实现按“父 FDO 管理总线与子 PDO、子 PDO 转发 USB 请求到既有用户态 ABI”组织，主要代码位于 `src/xusb_driver.c`、`src/xusb_bus.c`、`src/xusb_transactions.c` 和 `src/xusb_wmi.c`。

### 设备角色和枚举

- USB-only 与 Root Hub 父设备提供用户态控制接口，支持现有 PDO add/remove IOCTL，并通过 BusRelations 枚举子 PDO。
- Host Controller 启动时创建 Root Hub 内部 PDO；USB-tree 启动时创建 Host Controller 内部 PDO。内部 PDO 通过 BusRelations 暴露，并使用旧 INF/二进制中恢复的标识。
- USB-only、Root Hub、Host Controller 的设备接口分别对应应用控制接口、标准 USB Hub 接口（另提供应用控制接口）和标准 USB Host Controller 接口。USB-tree 不暴露用户控制接口。
- FDO/PDO 返回 USB bus type 信息；子 PDO 还返回恢复的 PnP ID 和 target-device relation。PDO 在用户 cleanup、PnP remove、surprise removal 和父设备移除路径上停止新事务、排空队列并释放引用；移除通知事件在适用时触发。Root Hub FDO 本地响应 Hub name/`GET_HUB_COUNT`，Host Controller 返回 Root Hub PDO 和当前设备栈顶，子 PDO 根据 present/remove 状态响应 `GET_PORT_STATUS`。

### URB 和事务映射

子 PDO 将旧二进制 ABI 能表达的控制/控制扩展、描述符、vendor/class、status、feature、pipe reset、frame number、configuration/interface selection、bulk/interrupt 和 isochronous URB 映射到 `src/usb/ioctl.h` 定义的 BEGIN/END 事务。`RESET_PORT` 与 `CYCLE_PORT` 映射为旧协议的 Type 4 事务。`GET_PORT_STATUS` 在内核本地根据子 PDO 的 present/remove 状态返回连接/启用位，不占用用户态 ABI。

配置选择会解析并校验 USB 配置、接口和端点描述符；合成的 pipe/interface 句柄包含代际信息，alternate setting 或配置切换后旧句柄不再有效。等时传输校验包描述符边界，并将数据、长度和包状态复制回 URB。GET_CURRENT_FRAME_NUMBER 同时接受现有用户态 handler 的 16 位结果和标准 URB 所需的 32 位结果，短值会零扩展。

每个父设备最多创建 256 个子 PDO；每个子 PDO 最多允许 256 个待处理事务、64 MiB 事务预算。父设备另外限制总计 1024 个待处理事务和 256 MiB 事务预算，避免多设备将单 PDO 上限无限放大。cancel-safe queue 负责 END、IRP cancel 和设备移除之间的队列仲裁。子 PDO 使用从旧二进制恢复的 `IoCreateDeviceSecure` 默认 SDDL/class GUID：SY 完全访问，BA、WD、RC 读写执行；相同 class GUID 保留 Windows 类注册表 ACL override 能力。子 PDO 响应基础 query/set power IRP。

### WMI

父 FDO 注册 WMI provider，提供 8 字节数据块回调。MOF schema/resource 被嵌入驱动，构建脚本会运行 `mofcomp -WMI` 检查；WMI class 能否被目标系统消费者发现，仍需实机确认。

## 已知限制

- 仓库当前 UVC handler 未实现 bulk/interrupt 传输或 Microsoft OS feature descriptor，因此对应路径尚不能与该用户态端到端工作。
- Hub name、`GET_HUB_COUNT`、`GET_ROOTHUB_PDO` 与 `GET_PORT_STATUS` 的本地响应是为了补足总线语义的重建行为，尚未证明与旧二进制完全一致；其他 Hub/port 请求未覆盖。完整 USB Host Controller 协议也未实现。
- 用户态服务无响应时没有独立的单请求超时。旧父 FDO 控制接口的最终访问策略还需目标系统确认。
- 未在真实 Windows USB/PnP/摄像头环境完成端到端运行验证。当前 `.sys` 为未签名构建，不能安装，也不能替换仓库中的旧驱动包。

共享 ABI 头直接包含 `src/usb/ioctl.h`，字段、布局和控制码以该文件为准。不要为了适配重建驱动而改变用户态协议。

## 工具链与构建

工程面向 x64 WDM，要求 Visual Studio C++ 构建工具、Visual Studio 的 `Windows Driver Kit` 组件，以及匹配的 Windows SDK/WDK 10.0.28000.0。微软说明 SDK 与 WDK 的版本号应匹配；可从[官方 WDK 下载说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)获取安装方式。

在 PowerShell 中构建：

```powershell
./build.ps1 -Configuration Release
./build.ps1 -Configuration Debug
```

加 `-Clean` 会删除此目录下被 Git 忽略的 `build/` 产物后再构建。输出分别位于 `build/x64/Release/` 与 `build/x64/Debug/`。

工程关闭了自动测试签名，因此输出 `.sys` 未签名，不能直接安装。构建成功只代表代码通过编译，不代表已满足 Windows USB/PnP 的完整契约。当前实现仍缺少若干 hub/port 请求、完整 USB Host Controller 协议和目标系统运行验证；不能把新 `.sys` 接入现有 INF 或替换旧驱动包。
