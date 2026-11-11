# GlazeWM Workspace Indicator

用于 Windows 11 的 Windhawk mod：把 GlazeWM 工作区按钮放在 **Windows 开始徽标右侧**。

```text
Windows 徽标   [ chat ━ ] [ web ] [ code ]   搜索 / 任务视图 / 应用图标 …
```

界面使用任务栏内的原生 Windows XAML 控件，Segoe UI Variable 字体、圆角、系统主题色、原生悬停和键盘焦点效果。当前焦点工作区显示强调色下划线；当前屏幕正在显示的工作区有淡背景。没有独立悬浮窗口。

## 显示逻辑

与 [Zebar starter 的 GlazeWM 工作区部分](https://github.com/glzr-io/zebar/blob/main/resources/starter/with-glazewm.html) 一致：

- 每个任务栏显示对应屏幕上已激活的工作区，保留 GlazeWM 返回顺序。
- 默认使用 `displayName ?? name`，可以切换为工作区名称／编号。
- `hasFocus` 与 `isDisplayed` 分别显示；多屏下，正在显示不一定拥有焦点。
- 点击按钮聚焦该工作区。命令使用工作区 UUID，显示名称不会进入命令。

这并不列出配置文件内尚未激活的所有工作区，也不复制 Zebar 的 CPU、天气、窗口标题等其他组件。

## 安装

需要 Windows 11 x64 的原生水平任务栏、Windhawk 和运行中的 GlazeWM 3.x。

1. 在 Windhawk 选择「创建新 mod」。
2. 将 [glazewm-workspace-indicator.wh.cpp](glazewm-workspace-indicator.wh.cpp) 的完整内容替换到编辑器。
3. 点击「编译 Mod」，然后启用。首次加载可能需要下载 Windows 符号。
4. 在 mod 设置中调整显示名称、最大宽度、按钮高度或刷新间隔。

默认连接 `127.0.0.1:6123`。不需要运行 Zebar，不需要 Node.js，不会修改 GlazeWM 配置。设置变更会重新加载 mod。禁用时移除按钮并恢复开始按钮的原始间距。

## 实现与兼容性

通过持久 WebSocket 每 300 ms 查询 `query monitors`，不重复启动 CLI 进程。网络在后台线程运行，接收有 2 秒截止时间，断线时隐藏旧工作区并重连。按钮点击在下一次查询周期发送。这里使用轮询快照，而不是 Zebar 的事件订阅。

通过任务栏的 `TaskbarHost` 找到 XAML 根节点，在开始按钮右侧增加布局间距，在同一任务栏根 Grid 中放入工作区条。宽度上限默认 280 DIP，并限制为任务栏根宽度的 35%；超出后可横向滚动。Windows 的任务栏对齐选项不被修改。

**0.1.0 是实验版本。** 已通过本机 Windhawk 工具链编译与真实 GlazeWM IPC 测试；尚未注入 Explorer 做实际 UI 验证。左右对齐、多屏缩放、自动隐藏和 Windows 更新后的布局仍需按下表验证。依赖未公开的任务栏符号；不支持的机器码布局会拒绝解析，Windhawk 日志中会显示原因。

不支持 Windows 10／ExplorerPatcher 旧式任务栏、垂直任务栏或隐藏开始按钮的布局。会修改开始按钮位置／Margin 的其他 mod（例如 Start button always on the left）可能冲突，首次验证建议关闭它们。

## 开发与验证

```powershell
.\scripts\build.ps1        # 编译与链接检查，产物位于 build/
.\scripts\build.ps1 -Test  # 解析、命令验证、真实本地 WebSocket 和关闭测试
python tests/stalled-peer.py # 模拟服务端卡住，验证超时与取消接收（需要 Python 3）
```

脚本需要 Windhawk 自带编译器；可以使用 `-Windhawk 'D:\Windhawk'` 指定安装目录。测试要求 GlazeWM 正在默认端口运行，不会发送工作区切换命令。`build/` 的 DLL 用于编译检查；安装请让 Windhawk 编辑器编译源文件，以生成 Windhawk 管理的生命周期入口。

实际 UI 验收清单：

| 场景 | 预期 |
| --- | --- |
| 任务栏左对齐／居中 | 指示器紧贴开始按钮右侧；搜索、应用图标无重叠 |
| 两块屏幕、不同缩放比例 | 各自显示对应屏幕工作区；尺寸遵循 DIP |
| 点击／键盘激活按钮 | 聚焦目标工作区；不打开开始菜单 |
| 中文、特殊字符、长名称 | 正常显示、长名称截断、悬停显示完整名称 |
| 浅色／深色／高对比度 | 文字、背景、焦点框清晰可辨 |
| 自动隐藏、分辨率变化、拔插显示器 | 指示器跟随任务栏，位置恢复正确 |
| 停止／重启 GlazeWM | 隐藏过期信息、自动重连 |
| 禁用／重新启用 mod | 原始间距恢复；没有残留按钮或事件回调 |

## 来源与许可

GPL-3.0-or-later，参见 [LICENSE](LICENSE) 与 [NOTICE.md](NOTICE.md)。
# glazewm-workspace-indicator
