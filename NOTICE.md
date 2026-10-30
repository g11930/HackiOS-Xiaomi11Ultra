# NOTICE — 来源与署名

`HackiOS-Xiaomi11Ultra` 是 HackiOS 项目在小米 11 Ultra（star，SM8350）上的研究性移植源码快照。它基于以下上游与第三方工作，按各自许可证分发：

## 上游项目

- **HackiOS（OOSEX GANG）** — XNU 内核适配、EFI loader、用户空间注入与 APFS 镜像构建的起点。
  - 内核：<https://github.com/oosexgang/hackios-xnu-kernel>
  - 用户空间：<https://github.com/oosexgang/hackios-xnu-userland>
  - 原始基线设备为 POCO X5 Pro（redwood，Snapdragon 778G）。star 移植在其之上做设备专属适配，不代表上游官方支持。

- **Apple XNU** — `xnu-8796.121.2`（Darwin 22.5.0 / iOS 16.5）内核源码，按 **Apple Public Source License 2.0（APSL 2.0）** 分发，见仓库根 `LICENSE`。

- **Project‑Silicium / Mu‑Silicium** — UEFI 移植基础（EDK2 / Mu 生态）。具体许可证以其仓库与文件头为准。

## 第三方组件

- **Mesa / Gallium** — 用户空间图形与 Metal 运行时的参考实现，按 **MIT** 许可证分发。
- **lemonade CI metal‑runtime** — Metal 运行时三件套（`libredwoodmetal` + `libredwoodair` + `RedwoodMetal.bundle`），提交 `c8033b632ec9`（metal‑runtime‑v1）。
- **ath11k（Linux）** — Wi‑Fi 驱动代码，按 Linux 内核 ** dual GPLv2 / BSD** 许可；本仓库仅含驱动代码，未含固件。
- **theos / iPhoneOS SDK、MacOSX SDK、cctools / ld64 / mig** — 交叉编译与构建工具链，按各自许可证；均不随本仓库分发，需本地准备。

## 设备专属工作

star 移植涉及的代码（redwood 驱动适配、`redwood_star.c`、`xnuloader`、UEFI 设备适配、构建脚本、文档）以本仓库文件头与上文许可证为准。

## 商标

iOS、iPhone、Apple 是 Apple Inc. 的商标；Xiaomi、Redmi、Snapdragon、Adreno、FastConnect 是各自权利人的商标。本项目为独立研究项目，与 Apple、Xiaomi 或 Qualcomm 无官方关联。
