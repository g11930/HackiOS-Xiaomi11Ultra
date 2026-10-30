# HackiOS on Xiaomi 11 Ultra (star)

小米 11 Ultra **star / 代号 star / Snapdragon 888（SM8350，lahaina）** 的 HackiOS 研究性移植。通过临时 UEFI / XNU loader 在 RAM 中启动 iOS 16.5，包含 STM FTS 触摸适配尝试、Adreno 660 桌面合成、已实机验证的 Metal 硬件渲染、原生壁纸部分修复与降采样桌面优化。

**本次公开的是 Wi‑Fi / 触摸 / 蓝牙 适配前、经过实机验证的源码基线。这些功能尚不可用；这不是可日用 ROM，也没有完整 Metal 兼容或稳定无撕裂 60 FPS 的承诺。**

本仓库将内核、loader 和用户空间源码放在同一个工作目录，克隆一次即可获得项目自己的源码。Apple 系统文件、SDK、设备固件、DTB、分区备份和预编译镜像均需在本地准备，不随仓库分发。

## 文档入口

| 文档 | 内容 |
|---|---|
| [本机移植笔记](docs/STAR.zh-CN.md) | 机型硬件参数、启动链、各功能状态、已知问题、构建与恢复 |
| [上游通用移植指南](docs/PORTING.md) | 通用 HackiOS 构建与移植说明 |
| [署名和许可证](NOTICE.md) | HackiOS、XNU、Mu‑Silicium、Mesa、ath11k 等来源 |

## 本版本实际支持什么

| 功能 | star 状态 |
|---|---|
| 临时 UEFI / XNU 启动、iOS 锁屏和桌面 | 已实机验证，RAM 启动 |
| 电源 / 音量键 | 已适配（PMK8350 电源块 GPIO 通道） |
| GPU 硬件渲染（Adreno 660） | 已验证 Metal 加速：WindowServer 加速渲染器 + Metal 设备 FD660；需 cmdline `gpu=1 metal=1 camtl=1 metal_profile=1` 且注入 `/redwood/bin/metal-enable` |
| 桌面显示 | 已进桌面，但单缓冲 + 60Hz CPU 拷贝 vs 120Hz DSI video 面板，存在横向分层 / 重影 / 撕裂；`fbscale=2` 可缓解但文字发糊；真正修复需内核双缓冲 + vblank 翻转（未做） |
| STM FTS K2 触摸 | 驱动已接到正确总线（QUPv3 SE11 SPI）并完成 RPMh VDD 供电链，但 probe 读 ID 失败（芯片有电、IRQ 拉低、MISO 恒高），疑似 bit‑bang 物理层 vs 原厂 GENI 硬件 SPI，仍在排查 |
| Wi‑Fi（WCN6855 / FastConnect 6900，PCIe `17cb:1103`） | 固件 / BDF / NV 已抽取并字节校验，但内核缺 PCIe LTSSM PIPE 时钟与 MHI/BHI 下载器，链路未起 |
| 蓝牙 | 无 BT 驱动，设置显示 Unavailable |
| 音频 / 相机 / 蜂窝 / 正常充电管理 | 未完成（放电电量准、充电电量不准，loader 走端电压回退） |
| 壁纸 | 任务切换器底色已修；主屏仍黑（缺 PosterBoard 服务，系统卷 LaunchDaemons 为空，全靠 ramdisk 那 26 个） |
| 语言 / 设置重启丢失 | 缺 `darwin` GPT 分区，`var_store` 落不了盘；方案已拟（语言分区方案）未刷入 |
| 持久化安装、日常数据保留 | 本版本采用 RAM 会话，不提供刷机安装流程 |

上游 POCO X5 Pro（redwood）的 Wi‑Fi、音频、相机或 8 核测试结果**不能**直接当成 star 的支持情况。不要把上游 `gpu=1 wifi=1 adsp=1` 启动串套用到本设备。

## 性能与限制

- GPU 已真正接管渲染（判定 HARDWARE）：GPU 常驻、Metal 设备 FD660 工作、UI 正常；但显示上屏仍走单缓冲 CPU 拷贝，与 120Hz 面板扫描错位，产生可见撕裂 / 重影。这是上屏路径问题，不是 GPU 失效。
- 一次实机验证记录了 GPU 送屏零 CPU 回退、Metal 应用绘制零读回错误；但完整帧率受上屏拷贝带宽限制，未达稳定无撕裂 60 FPS。
- 曾出现多指滑动后完全冻结（内核 panic，栈上 `RowJob` 被超时 helper 野指针解引用）。已结构性修复为常驻环形槽 + 代数校验（`MiniIOMobileFramebuffer.cpp`），但多指路径仍需更多实机覆盖验证。

## 开始之前

适用机型是已解锁 bootloader、可运行 root Android 的 **star（小米 11 Ultra，SM8350）**；其他 SKU / 红米 K40（alioth）等相近设备未验证。保留可回到 Android 的启动链和自己的分区备份。

```
git clone https://github.com/<your-github-account>/HackiOS-Xiaomi11Ultra.git
cd HackiOS-Xiaomi11Ultra
```

接着阅读 [本机移植笔记](docs/STAR.zh-CN.md) 与上游 [PORTING.md](docs/PORTING.md)。源码树中的 `common/setup.sh` 只检查工具，不会再拉取第二个用户空间仓库。本发布没有 `prebuilt/`（已被 `.gitignore` 排除），需要构建工具链。

这套流程使用 `fastboot boot` 临时启动，默认禁用普通分区写入；loader 仍会使用 **logdump** 保存诊断。它不是“完全没有存储写入”。本次 star 验证没有刷写系统分区、重新分区或清空用户数据。

## 基线与上游

- 内核和 star 工具：`xnu-8796.121.2`（Darwin 22.5.0 / iOS 16.5）。
- UEFI：[Project‑Silicium/Mu‑Silicium](https://github.com/Project-Silicium/Mu-Silicium) 基础上适配。
- Metal 运行时：lemonade CI `c8033b632ec9`（metal‑runtime‑v1）；metal `53bf913b`、air `11e3ddac`、bundle `f60bd54b`，三件套必须同一次构建。
- 起点：[OOSEX GANG HackiOS kernel](https://github.com/oosexgang/hackios-xnu-kernel) 与 [userland](https://github.com/oosexgang/hackios-xnu-userland)（redwood / POCO X5 Pro 基线）。star 适配不代表上游官方支持。
- Wi‑Fi 驱动：内核已编入 ath11k（`xnu-deps/build/ath11k`），但仅驱动代码，缺 PCIe 链路与 MHI/BHI 下载。

本仓库是干净的公开源码快照。原始私有仓库历史未公开；XNU 版本、Metal 基线与上游来源记录于 [RELEASE.json](RELEASE.json)。未公开后续 Wi‑Fi PCIe 诊断、GPU 时间戳、双核或命令环缓存试验。

## 贡献与许可

请先提供机型、Android 固件版本、公开源码提交、启动参数、能否回到 Android 和脱敏后的错误片段。不要把系统镜像、固件、设备序列号、账号或网络密码上传到 issue。

开发默认 **只 commit，不自动 push**；维护者明确要求发布时再同步远端。

XNU 衍生部分保留 [APSL 2.0](LICENSE)，用户空间 / loader / 其他组件以原文件头和 [NOTICE.md](NOTICE.md) 为准。本项目与 Apple、Xiaomi 或 Qualcomm 无官方关联。
