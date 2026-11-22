# 第三方来源与许可声明

## USB Video Class 结构定义

`src/protocol/uvc_struct.h` 中的 USB Video Class 结构定义改编自 Linux UAPI 头文件 [`include/uapi/linux/usb/video.h`](https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/video.h)。上游文件标注 SPDX `GPL-2.0 WITH Linux-syscall-note`，并注明 Laurent Pinchart 版权所有（2009）。该文件的原始来源、许可标识和作者归属集中记录于此；仓库根目录的 MIT `LICENSE` 不替代这些第三方声明。

## 历史虚拟 USB ABI 头文件

`src/usb/ioctl.h` 与 `archive/legacy-2016/old-drv-interface/` 中的 ABI 参考头文件源自仓库保留的 2016 年接口材料。原头文件顶部曾标注 Fanxiushu 及 2016-04-06、2016-04-27 日期；`src/protocol/uvc_struct.h` 的本地注释曾标注 2016-10-01。为统一源文件头部格式，这些作者/日期注释已从头文件移除，协议布局和历史来源仍保留在本声明及[驱动来源说明](docs/driver.md)中。
