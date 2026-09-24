# Glance CLI

[English](CLI.en.md) · [返回主页](../README.md)

从终端、脚本或文件管理器打开预览、控制窗口和修改设置。

## 开始使用

安装版和绿色版都包含 `Glance.CLI.exe`。以下示例使用 PowerShell，先指定程序路径；绿色版请换成自己的解压目录。

```powershell
$cli = 'C:\Program Files\Glance\Glance.CLI.exe'
& $cli preview 'C:\Documents\report.pdf'
```

Glance 等待预览就绪并输出完整窗口信息。主程序尚未运行时，CLI 会启动同目录的 Glance；已运行时直接发送请求。

用 `-h` 查看当前命令的用法：

```powershell
& $cli -h
& $cli preview -h
& $cli window move -h
```

## 常用操作

| 想做什么 | 命令（接在 `& $cli` 后） |
| --- | --- |
| 打开文件或文件夹 | `preview 'C:\Documents\report.pdf'` |
| 置顶预览 | `preview 'C:\Documents\report.pdf' --topmost` |
| 保留独立预览窗口 | `preview 'C:\Documents\report.pdf' --pin` |
| 显示 5 秒后关闭 | `preview 'C:\Documents\report.pdf' --close-after 5` |
| 查看所有窗口及 ID | `windows` |
| 关闭最近预览窗口 | `window close` |
| 查看程序状态和屏幕编号 | `status` |
| 检查新版本 | `check-update` |
| 退出 Glance | `quit` |

第三方文件管理器可调用下面的命令，将最后一个参数换成选中文件的完整路径：

```text
"C:\Program Files\Glance\Glance.CLI.exe" preview "C:\Documents\report.pdf"
```

## 打开预览

```powershell
# 在同一个窗口中浏览多个文件
& $cli preview 'C:\Pictures\first.jpg' 'C:\Pictures\second.jpg'

# 以指定尺寸居中显示，并钉住窗口
& $cli preview 'C:\Documents\report.pdf' --size 1200 900 --center-offset 0 0 --pin

# 等首屏就绪后返回，5 秒后自动关闭
& $cli preview 'C:\Documents\notes.txt' --close-after 5
```

| 选项 | 作用 |
| --- | --- |
| `--wait` | 等待窗口关闭后返回 |
| `--timeout SECONDS` | 限制等待时长；0 表示请求应用后立即返回当前状态 |
| `--pin` | 钉住预览，保留为独立的置顶窗口 |
| `--topmost` | 置顶本次预览；省略时默认关闭，钉住时自动开启 |
| `--close-after SECONDS` | 首屏就绪后延时关闭，范围 0.001～86400 秒 |
| `--size WIDTH HEIGHT` | 设置窗口外框宽高，单位为物理像素 |
| `--position X Y` | 设置左上角绝对坐标，支持负数 |
| `--center-offset X Y` | 设置窗口中心相对屏幕工作区中心的偏移；`0 0` 为居中 |
| `--monitor INDEX` | 为居中模式选择屏幕，编号见 `status` |

两种定位方式择一使用。居中打开默认选择鼠标所在屏幕；移动已有窗口默认选择窗口所在屏幕。指定位置或尺寸后，当前预览使用该值，后续普通预览仍按原有设置定位。

窗口最小外框尺寸为按 DPI 缩放后的 480 × 320，单边最大 32767 像素。超出范围返回参数错误。

相对路径以终端当前目录为准，带空格的路径需要引号。多个路径组成一个预览序列，从第一项开始；通配符由调用方展开。路径以 `--` 开头时，在前面加分隔符，例如 `preview -- --notes.txt`。

自动关闭计时绑定本次内容：切换文件会取消计时，钉住窗口会保留计时。

### 预览管道输入

`preview -` 读取标准输入直到结束，再预览生成的临时文件。`--name` 指定文件名和后缀，默认 `stdin.txt`。

```powershell
'{"name":"Glance","ready":true}' | & $cli preview - --name result.json
```

二进制使用 `--raw`，字节和换行保持原样。文件名默认 `stdin.bin`；通过后缀选择预览器。下面的例子在 **cmd.exe** 中运行，以保留输入文件的原始字节：

```bat
"C:\Program Files\Glance\Glance.CLI.exe" preview - --raw --name photo.png < "C:\Pictures\photo.png"
```

管道输入独占本次预览；`--name` 只接受文件名。临时文件在预览释放后清理，CLI 提前返回时仍可继续查看内容。

## 控制窗口

每个新窗口都有独立 UUID。省略 `--id` 时操作最近一次 `preview` 返回的窗口，钉住后仍保持该目标；目标已关闭时返回错误。指定其他窗口时，使用 `--id UUID`，UUID 可从 `windows` 查询。

`--id main` 指定当前打开的主预览窗口，结果返回其真实 UUID。主窗口已关闭时返回退出码 `3`。

```powershell
$result = & $cli preview 'C:\Documents\report.pdf' --pin --json | ConvertFrom-Json
$windowId = $result.data.id
& $cli window move --id $windowId --position 100 100
& $cli window resize --id $windowId --size 1200 900
& $cli window set 'C:\Documents\notes.txt' --id $windowId
& $cli window get --id $windowId
& $cli window close --id $windowId
```

| 命令 | 作用 |
| --- | --- |
| `window get` | 查看路径、加载状态、位置、尺寸及钉住状态 |
| `window set PATH [PATH ...]` | 切换预览文件或序列，保留 UUID、窗口尺寸、位置及钉住和置顶状态；支持 `--wait` |
| `window move --position X Y` | 移到绝对坐标 |
| `window move --center-offset X Y` | 相对屏幕中心定位，可配合 `--monitor INDEX` |
| `window resize --size WIDTH HEIGHT` | 修改窗口外框尺寸 |
| `window activate` | 将窗口带到前台并激活；系统拒绝时返回退出码 `8` |
| `window fullwindow on` / `window fullwindow off` | 进入或退出全屏预览，退出后恢复原窗口位置和尺寸 |
| `window topmost on` / `window topmost off` | 切换置顶 |
| `window pin on` / `window pin off` | 钉住窗口 / 关闭已钉住窗口 |
| `window close` | 关闭预览，Glance 保持运行 |
| `window line N` | 定位纯文本第 N 行，行号从 1 开始 |
| `window page N` | 定位 PDF 第 N 页，页码从 1 开始 |
| `window seek POSITION` | 定位媒体时间，接受秒数或 `HH:MM:SS[.fff]` |
| `window next` / `window previous` | 切换文件序列或画廊中的相邻文件 |
| `window play` / `window pause` | 播放或暂停媒体 |
| `window volume N` | 当前窗口音量，范围 0～100 |
| `window mute on` / `window mute off` | 当前窗口静音状态 |

以上命令都接受 `--id UUID`。UUID 在钉住后保持不变，Glance 重启后失效。`windows` 只列出正在显示的预览窗口，`main: true` 表示主预览窗口，独立窗口为 `main: false`。连续 `preview` 复用主预览窗口；钉住后，下次 `preview` 使用新窗口。关闭的窗口无法查询或控制，再次打开时可能复用原 UUID。

## 修改设置

先查找键及允许值，再修改。键区分大小写，直接复制 `settings list` 的结果即可。

```powershell
& $cli settings list TextPreview
& $cli settings get TextPreview/FontSize
& $cli settings set TextPreview/WordWrap true
& $cli settings set TextPreview/FontSize 14
& $cli settings reset TextPreview/WordWrap
```

`settings list` 列出全部公开设置；追加前缀可缩小范围。每项提供当前值、默认值、类型、允许范围和生效方式：`immediate` 为立即生效，`next_preview` 为下次预览生效。已加载组件的设置也会出现在列表中。

布尔值接受 `true/false`、`on/off`、`1/0`；枚举使用列表中的数值。数组使用 JSON，例如：

```powershell
& $cli settings set Footer/FieldOrder '[0,1,5,2,3,4]'
```

`settings reset KEY` 恢复单项默认值。`Appearance/Language` 的空值表示系统语言，`TextPreview/FontFamily` 的空值表示自动选取字体；这两项也可通过 `reset` 恢复默认。

## 在脚本中使用

加 `--json` 后，标准输出为单个 UTF-8 JSON 对象。用退出码判断成功，再读取 `data`：

```powershell
$result = & $cli preview 'C:\Documents\report.pdf' --pin --json |
    ConvertFrom-Json
if ($LASTEXITCODE -ne 0) {
    throw $result.error.message
}
& $cli window close --id $result.data.id
```

| 字段 | 含义 |
| --- | --- |
| `schema_version` | 输出结构版本，目前为 `1` |
| `ok` | 请求是否成功 |
| `command` | 命令名，例如 `preview`、`window.get` |
| `data` | 成功结果；失败时为 `null` |
| `error` | 失败信息，含 `code`、`name`、`message`；成功时为 `null` |


### 通用选项与等待时间

窗口结果中的 `fullwindow` 表示是否全屏。纯文本结果中的 `line_count` 为已加载文本的行数，`content_complete` 表示全文是否已加载完成。

| 选项 | 作用 |
| --- | --- |
| `-h` / `--help` | 显示当前命令帮助；也支持 `help COMMAND` |
| `--json` | 输出 JSON，适合脚本处理 |
| `--quiet` | 静音 stdout，保留 stderr 和退出码；与 `--json` 同用时，错误 JSON 输出到 stderr |
| `--no-start` | 只连接正在运行的 Glance |
| `--timeout SECONDS` | 窗口命令等待上限，范围 0～86400 秒 |
| `--wait` | `preview` 和保持窗口存在的 `window` 命令等待目标关闭 |
| `--version` | 显示版本 |

默认等待内容就绪或控制操作完成。`--timeout 0` 在请求应用后返回，正数超时返回当前状态及 `wait_completed: false`；正常完成返回 `wait_completed: true`。`--wait` 等待窗口关闭；与 `--timeout` 组合时采用同一总等待上限。

连接最多等待 15 秒，普通请求传输与执行最多 10 秒，更新检查最多 60 秒。这些故障会返回错误。管道输入收集时间独立于内容等待。CLI 退出或 Ctrl+C 中断等待后，已接受的预览和控制操作继续运行。

### 退出码

| 代码 | 含义 |
| --- | --- |
| `0` | 成功，也包括没有更新或程序已退出 |
| `1` | 内部错误 |
| `2` | 参数、设置值或请求格式错误 |
| `3` | 文件、窗口、设置或屏幕不存在 |
| `4` | 主程序不可用或启动失败 |
| `5` | 超时或连接中断，操作结果可能未知 |
| `6` | 协议或响应不兼容 |
| `7` | 权限不足 |
| `8` | 状态冲突，例如关闭已钉住窗口的置顶 |
| `9` | 预览加载失败 |
| `10` | 预览被替换、关闭或请求取消 |
| `11` | 更新检查失败 |
| `130` | Ctrl+C / Ctrl+Break 中断 |
