# Glance CLI

[English](CLI.en.md) · [Glance](../README.md)

`Glance.CLI.exe` 供终端、脚本和文件管理器打开预览、控制窗口及管理设置。安装版和绿色版均包含该程序，请从 Glance 目录运行，或使用其完整路径。

```powershell
.\Glance.CLI.exe preview 'C:\Documents\report.pdf'
.\Glance.CLI.exe preview 'C:\Documents\report.pdf' --pin --wait --json
.\Glance.CLI.exe windows
```

CLI 优先连接正在运行的同目录主程序；连接不可用时尝试启动主程序。`--no-start` 可仅连接现有实例。帮助、版本和参数校验在 CLI 本地完成。第三方文件管理器可调用 `Glance.CLI.exe preview "选中文件的完整路径"`。

## 预览与窗口

```text
preview PATH... [--wait] [--close-after SECONDS] [--pin]
  [--topmost on|off] [--size WIDTH HEIGHT]
  [--position X Y | --center-offset DX DY [--monitor INDEX]]
window get [--id ID]
window move [--id ID] --position X Y
window move [--id ID] --center-offset DX DY [--monitor INDEX]
window resize [--id ID] --size WIDTH HEIGHT
window close [--id ID]
window topmost on|off [--id ID]
window pin on|off [--id ID]
windows
```

- `preview` 接受一个或多个文件或文件夹。多路径在同一窗口组成预览序列，从第一项开始；相对路径按终端当前目录解析。路径中有空格时使用引号；用 `--` 分隔以 `--` 开头的路径。通配符应由调用方展开，输入采用文件系统路径。
- 默认成功表示预览请求已应用，文本输出仅为窗口实际 ID。`--wait` 等待首屏就绪；加载失败、预览被替换或关闭会返回错误。通用信息预览也属于有效结果，JSON 的 `fallback` 字段会标明。
- `--close-after` 接受正数秒，可含小数，从首屏就绪后计时。关闭计时绑定本次预览，切换文件后取消；钉住后继续作用于原窗口。
- 默认 `--id 0` 指向当前动态预览窗口。实际 ID 在钉住后保持不变，程序重启后失效。JSON 中 ID 和预览代次使用字符串。
- 钉住会自动置顶并分离为独立窗口，随后创建新的动态窗口。取消钉住会关闭独立窗口；钉住期间保持置顶。
- `windows` 包含隐藏的动态窗口。`window get` 与 `windows` 返回路径序列、当前索引、加载状态、边界、钉住和置顶状态。

### 坐标和尺寸

所有坐标及尺寸使用物理像素，尺寸指整个窗口外框。`--position` 指虚拟桌面左上坐标，允许负数；`--center-offset` 指窗口中心相对屏幕工作区中心的偏移。用 `status` 查询屏幕索引及工作区。

新预览的中心偏移默认使用鼠标所在屏幕；移动现有窗口时默认其所在屏幕。`--monitor` 与中心偏移模式配合使用。显式位置和尺寸优先于自动适应及窗口记忆，并仅作用于当前预览。尺寸下限随 DPI 缩放，逻辑下限为 480 × 320；超出范围返回参数错误。

```powershell
.\Glance.CLI.exe preview 'C:\Documents\report.pdf' --size 1200 900 --center-offset 0 0 --pin
.\Glance.CLI.exe window move --id 12345 --position -1200 100
.\Glance.CLI.exe window close --id 12345
```

示例中的 `12345` 需替换为预览命令返回的 ID。

## 设置与管理

```text
status
settings list [PREFIX]
settings get KEY
settings set KEY VALUE
settings reset KEY
check-update
quit
```

设置树由主程序的公开设置及已加载组件动态提供。键使用现有分组和值名，大小写以 `settings list` 输出为准。每项包含类型、当前值、默认值、范围、可选值和生效方式：`immediate` 表示立即应用，`next_preview` 表示下次预览生效。

```powershell
.\Glance.CLI.exe settings list TextPreview --json
.\Glance.CLI.exe settings set TextPreview/MonitorFile true
.\Glance.CLI.exe settings set TextPreview/RefreshIntervalMs 1000
.\Glance.CLI.exe settings reset TextPreview/MonitorFile
.\Glance.CLI.exe settings set Footer/FieldOrder '[0,1,5,2,3,4]'
```

布尔值接受 `true/false`、`on/off`、`1/0`；枚举保留原生数值。`Appearance/Language` 的空字符串表示系统语言，`TextPreview/FontFamily` 的空字符串表示自动选择默认字体。内部缓存、窗口记忆及授权任务属于内部状态。

`check-update` 仅检查并返回版本、更新可用性及发布/下载链接；无更新也返回成功。`quit` 等待 App 和 Core 退出，程序已关闭时直接成功。

## 输出与退出码

默认文本输出便于手动使用；自动化建议添加 `--json`。JSON 模式 stdout 只输出一个 UTF-8 对象，字段为 `schema_version`、`ok`、`command`、`data`、`error`。文本模式错误写入 stderr。

```json
{"schema_version":1,"ok":false,"command":"window.get","data":null,"error":{"code":3,"name":"window_not_found","message":"Window ID not found"}}
```

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功，包括未发现更新或程序已经退出 |
| 1 | 内部错误 |
| 2 | 参数、设置值或请求格式错误 |
| 3 | 文件、窗口、设置或屏幕不存在 |
| 4 | 主程序不可用或启动失败 |
| 5 | 超时或连接中断，操作结果可能未知 |
| 6 | 协议或响应不兼容 |
| 7 | 权限不足 |
| 8 | 状态冲突，例如取消钉住窗口的置顶 |
| 9 | 预览加载失败 |
| 10 | 预览被替换、关闭或请求取消 |
| 11 | 更新检查失败 |
| 130 | Ctrl+C / Ctrl+Break 中断 |

普通命令默认等待 10 秒，建立冷启动连接允许 15 秒，`preview --wait` 默认 30 秒，检查更新默认 60 秒。全局 `--timeout SECONDS` 可覆盖连接和命令各自的等待上限。超时后应查询窗口状态，再决定是否重试修改命令。

CLI 退出或中断等待后，已接受的预览和自动关闭计时继续运行。`--help` 列出命令，`--version` 输出产品版本。
