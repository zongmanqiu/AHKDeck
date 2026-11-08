# AHKDeck

**[⬇️ 下载 0.2.0 (Gitee)](https://gitee.com/qiuzongman/AHKDeck/releases/download/0.2.0/AHKDeck_0.2.0.zip)**
 ｜ **[⬇️ 下载 0.2.0 (GitHub)](https://github.com/zongmanqiu/AHKDeck/releases/download/0.2.0/AHKDeck_0.2.0.zip)**
 ｜ [Gitee主仓](https://gitee.com/qiuzongman/AHKDeck)
 ｜ [GitHub镜像](https://github.com/zongmanqiu/AHKDeck)

> Windows x64 的 AHK v2 脚本工作台，使用 C++17、FLTK 与定制 AutoHotkey_H 内核。
脚本会话在宿主进程内运行；真正退出宿主时一并停止。标题栏关闭通常只是隐藏到托盘，脚本继续运行。

## 使用

- 将脚本放入程序旁的 `Scripts/`，在列表中运行、停止、重启或编辑。
- 内置编辑器提供保存、查找替换；当前编辑脚本的运行回执与内核错误显示在控制台，各会话日志写入程序旁的 `Logs/`。
- AI 接入使用 MCP stdio：外部客户端负责模型和修正循环，本程序提供脚本工具与可见编辑区，不内建聊天面板。
- 每次启动会清理程序旁 `.temp/`、`Backups/`、`Logs/` 里超过 30 天的旧文件；天数在 `settings.json` 的
  `housekeeping.max_age_days`（`0` 关闭），不在界面上暴露。
- 仅运行可信脚本。工具路径校验不等于脚本执行沙箱；与官方 AHK 的一致性由具体对拍用例验证，不作完全兼容保证。

在解压后的程序目录运行：

```bat
AHKDeck.exe
AHKDeck.exe --version
AHKDeck.exe --run Scripts\鼠标左键双击最小间隔.ahk
AHKDeck.exe --journal --run Scripts\鼠标左键双击最小间隔.ahk
AHKDeck.exe --mcp-stdio
AHKDeck.exe --quit 目标实例PID
```

`--journal` 输出逐行 JSON 事件；`--run` 每次运行一个脚本，可在脚本路径后传参。
`--quit` 可指定实例 PID，不指定则通知所有工作台退出。MCP 接入细节见 [agent/README.md](agent/README.md)。

## 构建

需要 Windows x64、MSVC C++ 工具链与 Windows SDK、CMake（至少 3.16）、NMake、MSBuild，以及下载工具 curl/tar。
MCP 门禁另外需要 Python；未找到 Python 时 CMake 不注册这些测试。

当前构建脚本要求源码位于工作区的 `.main/` 子目录；`.3rd/`、`.temp/`、`.build/` 与其同级，分别存放依赖、构建中间物和成品。它们是相对目录约定，不绑定本机位置。

从该工作区执行：

```bat
.main\build\full.bat
```

流程：工具链检查 → 下载依赖 → 应用补丁 → 构建 → 测试，失败即停。
日常增量构建用 `.main\build\4_build.bat`，验证用 `.main\build\5_test.bat`。
产品包含 `AHKDeck.exe` 和 `AutoHotkey64.dll`，不是单文件分发；脚本同步到 `.build/Scripts/`。
发行包由 [6_package.ps1](build/6_package.ps1) 用 7z 生成，白名单见 [打包清单](build/打包清单.md)。
依赖版本以 [build/deps.txt](build/deps.txt) 为准，详细步骤见 [构建说明](build/build_readme.md)。

## 文档导航

| 文档 | 职责 |
|---|---|
| [目录结构说明](目录结构说明.md) | 文件放哪里、模块职责与依赖 |
| [开发指南](开发指南.md) | 修改流程、线程边界与验证 |
| [文档清单](DOCS_MANIFEST.md) | 改动后应同步哪些文档 |
| [UI 说明](ui/README.md) | 当前交互与共享控件职责 |
| [引擎说明](engine/README.md) | 会话、错误记录与生命周期 |
| [脚本说明](scripts/README.md) | 随包脚本与菜单桥 |

许可见 [LICENSE](LICENSE)；第三方组件与分发说明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
