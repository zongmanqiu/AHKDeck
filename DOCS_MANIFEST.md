# 文档同步清单

按改动范围逐项核对，仅记录 AHKDeck 已有模块与验证能力。本文是人工检查清单，不是自动门禁。

| 文档 | 职责 / 同步时机 |
|---|---|
| [README.md](README.md) | 产品介绍、使用与构建入口；启动方式、平台或依赖变化时更新 |
| [目录结构说明.md](目录结构说明.md) | 目录、文件职责与依赖；新增、删除、迁移模块或测试时更新 |
| [开发指南.md](开发指南.md) | 修改流程、线程边界、验证范围；开发方式与门禁变化时更新 |
| [core/README.md](core/README.md) | 基础逻辑职责；数据模型、编码、策略接口变化时更新 |
| [engine/README.md](engine/README.md) | 会话 API、内核生命周期和日志；错误路由、退出或日志位置变化时更新 |
| [agent/README.md](agent/README.md) | 工具接口与 MCP 接入；工具定义、协议或连接生命周期变化时更新 |
| [ui/README.md](ui/README.md) | 当前交互、控件职责与线程接线；菜单、布局、共享控件变化时更新 |
| [scripts/README.md](scripts/README.md) | 随包脚本与菜单桥；脚本或分发规则变化时更新 |
| [build/build_readme.md](build/build_readme.md) | 构建步骤、开关、输出路径；构建脚本变化时更新 |
| [test/ui_gate/README.md](test/ui_gate/README.md) | UI 冒烟入口；测试步骤或参数变化时更新 |
| [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) | 第三方来源、许可与分发义务；依赖或补丁变化时更新 |
| [内核补丁说明](patches/AutoHotkey_H-v2.0.25/README.md) / [FLTK 补丁说明](patches/fltk-1.4.5/FLTK_PATCHES.md) | 补丁范围、理由与验证；修改第三方补丁时更新 |

核对依据：目标与测试注册看 `CMakeLists.txt`，流水线看 `build/*.bat`，依赖看 `build/deps.txt`，行为看对应实现。所有文档使用相对路径，不记录密钥或本机路径，不写固定测试数量、未经实测的性能及全覆盖承诺。
