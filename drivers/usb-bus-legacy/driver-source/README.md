# 重建版 WDM 驱动源码

这是独立的新驱动源码目录，与保留的 2016 年驱动包及本机反编译分析目录分开。旧 INF 没有指向此目录，目录中构建出的 `.sys` 也不是旧驱动的替代品。

## 当前实现

目前已补入有限的总线枚举路径和 URB 事务。USB-only 角色可通过父设备控制接口处理 PDO 新增/移除 IOCTL、创建子 PDO、返回 BusRelations 和 PnP ID，并在用户清理或 PDO 移除时回收引用、发出移除事件。角色设备接口已按旧二进制恢复：USB-only 使用应用控制接口，Root HUB 使用标准 USB HUB 接口并额外提供应用控制接口，Host Controller 使用标准主控制器接口。USB-tree 角色处理 BusRelations 但没有用户控制接口。旧驱动在 Host Controller/USB-tree PnP 启动时创建内部 Root HUB/Host Controller PDO；这组角色专属 PDO 创建和完整关系枚举尚未复刻。子 PDO 将控制/控制扩展、设备/接口/端点描述符、vendor/class、status、feature、pipe reset、frame number、Microsoft feature descriptor、配置/接口选择、bulk/interrupt 与 isochronous URB 映射到既有 64 字节 BEGIN/END ABI；RESET_PORT/CYCLE_PORT 也转成旧协议的 Type 4 事务。配置 URB 解析接口/端点描述符并生成代际化的合成句柄；等时传输校验包表并回写数据、长度与状态。子 PDO 使用从旧二进制恢复的 IoCreateDeviceSecure 默认 SDDL 和 class GUID；默认 ACL 允许 SY 完全访问、BA/WD/RC 读写执行，Windows class 注册表 override 通过相同 GUID 生效。cancel-safe queue 仲裁取消与 END，设备移除/惊奇移除会停止并清空队列；每个子 PDO 限制 256 个事务、合计 64 MiB 非分页事务缓冲区。子 PDO 可响应基础 query/set power IRP；父 FDO 已补 WMI provider 注册及 8 字节数据块回调。当前用户态 UVC handler 尚未处理 bulk/interrupt、GET_MS_FEATURE_DESCRIPTOR 和 GET_CURRENT_FRAME_NUMBER 的响应，因此这些协议路径不能端到端成功。旧二进制的 child URB 序列化路径中未发现 GET_PORT_STATUS 支持；Root HUB 角色是否经其他路径处理该请求尚未确认。其他 USB hub/port 请求仍未覆盖；WMI MOF schema/resource 尚未加入，消费者发现 class 的行为未验证。用户态服务无响应时也没有单请求超时。父设备按 INF/旧二进制 ID 识别四种角色。完整角色专属 PnP/电源行为、父 FDO 控制接口的最终访问策略，以及端到端 USB/PnP/摄像头运行时验证仍未完成，因此仍不能安装或替换仓库中的旧驱动。

共享 ABI 头直接包含 `src/usb/ioctl.h`，字段、布局和控制码以该文件为准。不要为了适配重建驱动而改变用户态协议。

## 工具链与构建

工程面向 x64 WDM，要求 Visual Studio C++ 构建工具、Visual Studio 的 `Windows Driver Kit` 组件，以及匹配的 Windows SDK/WDK 10.0.28000.0。微软说明 SDK 与 WDK 的版本号应匹配；可从[官方 WDK 下载说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)获取安装方式。

在 PowerShell 中构建：

```powershell
./build.ps1 -Configuration Release
./build.ps1 -Configuration Debug
```

加 `-Clean` 会删除此目录下被 Git 忽略的 `build/` 产物后再构建。输出分别位于 `build/x64/Release/` 与 `build/x64/Debug/`。

工程关闭了自动测试签名，因此输出 `.sys` 未签名，不能直接安装。构建成功只代表代码通过编译，不代表已满足 Windows USB/PnP 的完整契约。当前实现仍缺少若干 hub/port 请求、WMI MOF schema、完整角色专属 PnP/电源行为和目标系统运行验证；不能把新 `.sys` 接入现有 INF 或替换旧驱动包。
