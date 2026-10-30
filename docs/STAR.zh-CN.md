# star 移植笔记（Xiaomi 11 Ultra / SM8350）

> 机型专属文档：硬件参数、启动链、功能状态、已知问题、构建与恢复。
> 通用 HackiOS 构建见 `docs/PORTING.md`。本文件随源码发布，状态以发布日（2026-10-30）为准。

## 一、硬件参数（已核实）

| 项目 | 值 | 来源 |
|---|---|---|
| 触控 IC | STM FTS K2（固件 `st_fts_k2.ftb`，Chip Id `0x3639`） | 安卓 dmesg / 刷机包 |
| 总线 | QUPv3 SE11 SPI `spi@a8c000`（reg `0xa8c000`，size `0x4000`） | DTS |
| SPI 引脚 | GPIO48=MISO / 49=MOSI / 50=CLK / 51=CS | DTS |
| 中断 / 复位 | IRQ GPIO23（低有效）/ RESET GPIO22 | DTS |
| 电源 | VDD 1.8V = L2C（RPMh `0x42500`，`{1800mV,1,7}`）；AVDD 3.3V = GPIO132；GPIO154 = TP 电源使能 | DTS / 实测 |
| 分辨率 | 1440×3200（FTS x‑max `0x5a0` / y‑max `0xc80`） | DTS / 固件 |
| Wi‑Fi | WCN6855（FastConnect 6900），PCIe `17cb:1103` @ `0000:01:00.0` | lspci 等价 / DTS |
| 副屏 | 有（后置小屏，节点需禁用防抢资源） | DTS |

## 二、启动链（实机，RAM 启动）

```
fastboot boot star-boot.img        # 必须原生 star fastboot（串号 e95b466e）
      → xnuloader                 # 串号变为 xnuloader
      → oem ramdisk               # 注入 ramdisk（rd.bin，APFS）
      → stage system-zh.apfs       # ~109s，需主机内存 ≥ 2.3GB
      → stage payload              # 注入内核 + cmdline
      → oem run                   # 启动 iOS 16.5 用户态
```

- **必须原生 star fastboot**：xnuloader 态下 `stage` 会报 cannot load。若停在 xnuloader，先 `fastboot -s xnuloader reboot bootloader` 回标准态。
- 默认只写 RAM，`fastboot reboot` 即回安卓。
- loader 仍会用 logdump 保存诊断；`oem log` 可取 `runs/run<时间>.log.txt`。

### 当前 cmdline（基线 + GPU）

```
battery=1 ufs_write=1 var_store=1 ufs=2 wifi=1 switcher_wallpaper=1
gpu=1 metal=1 camtl=1 metal_profile=1
```

GPU 真正接管渲染必须**同时满足** 5 个条件：

1. cmdline `gpu=1`（内核启 GPU 并常驻）
2. cmdline `metal=1`（打开内核 `IOAcceleratorES::setProperties` 门）
3. 运行 `/redwood/bin/metal-enable`（写 `MetalPluginName=RedwoodMetal` / `MetalPluginClassName=RedwoodMetalDevice`），且必须插在 `/etc/rc` 的 `exec /sbin/launchd.ios` 之前（PID1 阶段）
4. cmdline `camtl=1`（否则 `libredwood.dylib` 把 Metal 设备从 WindowServer 摘掉）
5. cmdline `metal_profile=1`（profile 0 只有 Apple1，MPS 需 Apple2；否则 SpringBoard 的 MPSImageGaussianBlur 拿不到 MPSDevice → SIGABRT 循环）

Metal 三件套必须同一次构建：`libredwoodmetal` + `libredwoodair` + `RedwoodMetal.bundle`（lemonade CI `c8033b632ec9`）。混用会导致 `invalid Metal rasterizer state` → 黑屏。

## 三、构建环境

| 项 | 值 |
|---|---|
| 主机 | Windows + WSL2 Arch（`wsl.exe -d Arch -u root`） |
| SDK | theos `iPhoneOS16.5.sdk`（WSL `/root/theos/sdks/`） |
| 交叉工具链 | xnu 交叉编译（mig / ld64 / cctools）+ MacOSX13.3 SDK（由 `xnu-deps/setup.sh` 获取） |
| 安卓侧 | adb / fastboot（`C:\Users\86191\AppData\Local\Android\Sdk\platform-tools\`） |

## 四、构建步骤

```bash
# 1) 内核交叉编译（WSL 内）
cd /tmp/hackios-x/hackios-xnu-kernel-main
./xnu-build.sh -j8 -k
# 产物：xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
# （config_all 符号检查报错良性，看 mach 产物时间戳判断重链）

# 2) 打 payload（改后部署）
cd /path/to/ios-alioth-mini
bash mkpayload-nov.sh          # 部署到 boot/run-payload.bin，校验 md5

# 3) 引导 + 抓日志（Windows 侧）
#    设备先进标准 fastboot（串号 e95b466e），再：
Start-Process powershell -ArgumentList "... boot-one.ps1"
#    ~3.5 分钟出 runs/run*.log.txt + 截图
```

`xnu-build.sh` 会自动把 `xnu-deps/build/ath11k/out/ath11k.o` 链进内核（存在才链）。

## 五、各功能状态

- **电源 / 音量键**：已适配（PMK8350 GPIO），用户实测可用。
- **GPU 渲染**：已确认硬件渲染（HARDWARE）。GPU 常驻、Metal 设备 FD660、WindowServer 加速。
- **显示**：单缓冲 + 60Hz CPU 缩放拷贝 vs 120Hz DSI video 面板，无 vblank 对齐 → 横向分层 / 上下重影 / 撕裂 / 一闪一闪。
  - `fb_hz=120 ca_vbl=1 fb_threads=8` 无效（拷贝 16ms > 面板 8.33ms，beat 不生效）。
  - `fbscale=2` 有效止血（拷贝 7.2ms，~62% 帧能塞进一次扫描），代价是文字发糊。
  - 真正修复需内核双缓冲 + 在 vblank 翻转 MDP 源地址（改动集中在 `MiniIOMobileFramebuffer.cpp` + `redwood_adreno.c`）。
- **触摸**：驱动接到正确总线（STM FTS over QUPv3 SE11 SPI），RPMh VDD 供电链已修（cmd‑db 解析 SM8350 布局：`addr(4)+data(4)+id(4)`，ldoc2 VRM `0x00042500`）。但 probe 读 ID 仍失败：芯片有电（IRQ 拉低）、CS/CLK/MOSI 翻转正确、MISO 恒高、读回全 `ff`。最大嫌疑 = bit‑bang 物理层 vs 原厂 GENI（qup11）硬件 SPI；次嫌疑 = 固件 flash 加载期占用 SPI。下一步评估移植 GENI 硬件 SPI（复用 `redwood_goodix.c` 的 `geni_*` 全套）。
- **Wi‑Fi**：固件（`amss20.bin` / `m3.bin`）、BDF（`bd_k8.elf`，板号 8 命中 DT `board-id=8`、CN 基版）、NV（`nv-modemst1/2`、`nv-fsg`、`nv-fsc`）已从本机抽取并字节校验。但内核侧缺两样：
  1. **PCIe LTSSM** 停在 `Detect.Quiet`（PHY 寄存器写完、`LTSSM_EN` 置位，但 PIPE 时钟没出来）；
  2. **MHI / BHI**：`redwood_pcie.c` / `redwood_wifi.c` 里无 `mhi`/`bhi`/`amss` 实现，即使链路起来也没人把固件喂给芯片。
  ath11k 的 `mhi.c`/`bhi.c` 已在 `xnu-deps/build/ath11k` 可作对照。
- **蓝牙**：无 BT 驱动（无 HCI 传输 / 无 WCN6855 BT 通道）→ 设置显示 Unavailable。
- **电量**：放电准；充电不准（loader `soc_from_voltage` 端电压回退，被 IR 压降抬高）。修在 loader 源码。
- **语言 / 设置重启丢失**：缺 `darwin` GPT 分区 → 无 `/dev/rufs_darwin` → `var_store` 落不了盘。方案见 `语言分区方案-alioth.md`（含已生成的 `gpt-head.new` / `gpt-tail.new`），未刷入。
- **壁纸**：任务切换器底色已修（`switcher_wallpaper=1`）；主屏仍黑——缺 PosterBoard 服务（系统卷 `/System/Library/LaunchDaemons/` 为空，全靠 ramdisk `/redwood/LaunchDaemons/` 那 26 个，其中没有 posterboard）。

## 六、已知问题 / 坑

- **APFS（rd.bin）**：Fletcher‑64 校验。原位改写已有文件必须字节数不变；新增文件必须重建 B‑tree，否则 SpringBoard 每 6 秒崩溃。
- **loader USB 死锁**：`fastboot devices` 看得到但传输 `no link`，软件无解，长按电源 12 秒强重启。
- **多指冻结（内核 panic）**：`drawLayer()` 栈上 `RowJob` 被超时 helper 野指针解引用 → data abort。已结构性修复为常驻环形槽 + 代数校验（`MiniIOMobileFramebuffer.cpp` 的 `fJobRing[8]` + `generation`）。多指路径仍需更多实机覆盖。
- **PowerShell 脚本坑**：`$Args` 是保留变量；`Start-Process` 会因 `http_proxy`/`HTTP_PROXY` 大小写冲突失败；`dd` 的 stderr 会被 `$ErrorActionPreference=Stop` 当致命错。

## 七、恢复 / 退出

- 任何时刻 `fastboot reboot` 即回安卓（RAM 会话，未写分区）。
- 若 loader USB 死锁：长按电源 12 秒强重启，再进标准 fastboot。
- 保留自己的分区备份与可回安卓的启动链后再做实验。

## 八、验证证据（发布前）

- GPU：判定 HARDWARE（GPU 常驻、`RedwoodGPU client opened`、Software renderer 0 次）。
- 触摸：run122358 引脚级采样——输出全部正确、芯片有电（int=0）但 MISO 恒高。
- Wi‑Fi：固件 / BDF / NV 字节校验通过；安卓侧同芯片可连 5GHz / 11ax / 1200Mbps，确认硬件与固件无误，坏在内核链路与下载器。
- 显示：live1823/live1825 实时帧缓冲截图 + 逐帧时序（done−shown），确认撕裂来自单缓冲上屏路径。
