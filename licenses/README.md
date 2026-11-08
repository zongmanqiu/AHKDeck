# 第三方许可原件

本目录存放随产品分发的**上游许可原文**（逐字节复制，未改写）。产品自身的许可与组件说明在上一级目录：
`LICENSE`（GPL-2.0 全文）与 `THIRD-PARTY-NOTICES.md`（组件清单、钉定版本、补丁与落位）。

| 文件 | 对应组件 | 说明 |
|---|---|---|
| `GPL-2.0.txt` | AutoHotkey_H v2.0.25（进程内引擎） | 复制自上游 `license.txt`，裸 GPLv2 全文，与本产品 `LICENSE` 同文 |
| `LGPL-2.0-with-FLTK-exception.txt` | FLTK 1.4.5（GUI 框架） | 复制自上游 `COPYING`（2001-12-11 版）：LGPL-2.0 正文 ＋ FLTK 例外条款 |

## 为什么是"原件"而不是摘要

GPL-2.0 第 1 条要求分发程序时**一并给出许可证副本**；LGPL 同样要求许可证文本随附。
摘要（`THIRD-PARTY-NOTICES.md`）说明"用了什么、什么版本、改了什么"，但不能替代原文。

## 源码在哪里

- 本产品（含内嵌引擎）按 GPL-2.0 发布源码：仓库
  `https://gitee.com/qiuzongman/AHKDeck`（GitHub 镜像 `https://github.com/zongmanqiu/AHKDeck`）。
- 上游快照与钉定 commit 见 `THIRD-PARTY-NOTICES.md`；对内核与 FLTK 的补丁原文在仓库
  `.main\patches\` 下，逐条说明见各自的 README。
- FLTK 以**静态链接**使用；上游许可的例外条款明确静态链接不构成衍生作品。
