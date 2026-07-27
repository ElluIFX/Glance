<p align="center">
  <img src="src/Glance.App/Assets/AppIcon.png" width="128" height="128" alt="Glance">
</p>

<h1 align="center"><b>Glance · 一瞥</b></h1>

<p align="center"><b>按下空格，即刻预览 Windows 中的文件</b></p>

<p align="center">
  <b>中文</b>
  ·
  <a href="README.en.md">English</a>
</p>

<p align="center">
  <a href="https://github.com/ElluIFX/Glance/releases">下载</a>
  ·
  <a href="https://github.com/ElluIFX/Glance/issues">反馈问题</a>
  ·
  <a href="LICENSE">GPL-3.0</a>
</p>

---

<p align="center">
查看文件内容不应总要启动完整应用，Glance 提供快速、安静的文件预览体验，让浏览和筛选文件更直接
</p>

<p align="center">
选中文件并按下空格，即可打开预览，再次按下空格，或窗口失去焦点，预览随即关闭
</p>

<p align="center">
相比同类软件更注重稳定性与可靠性，在你需要预览文件时，<b>Glance 保证随叫随到</b>
</p>

<p align="center">
  <img src="docs/screenshot.png" alt="Glance Screenshot" style="max-width: 90%; border-radius: 8px;">
</p>

## 软件特色

- 可从资源管理器、文件选择窗口和 Everything 快速发起预览
- 支持文本、代码、Markdown、图片、音视频、PDF、压缩包以及更多复杂格式
- 高效可靠的长文本预览，灵活的图片缩放与拖动，现代预览能力齐备
- 钉住预览窗口，并可同时打开多个独立预览，让文件成为便签
- 按内容类型记忆窗口尺寸与位置，贴合使用习惯
- 细致自定义行为与显示，满足个性化需求
- **摈弃复杂快捷键 ，只需空格与鼠标即可高效访问全部功能**
- **严谨的多进程架构，互为监督，在复杂工况下保持可靠响应**
- **拒绝繁杂插件系统，保持轻量、可靠、高效**

## 使用

从 [Releases](https://github.com/ElluIFX/Glance/releases) 下载并安装 Glance。运行后，选中文件并按下空格即可预览；设置与退出入口位于系统托盘

### 可选组件

复杂格式支持以独立组件提供，与 Glance 主程序保持隔离，在安装时可自由裁剪

- **Microsoft Office 预览** — 支持预览 Word、PowerPoint 与 Excel 文件，需要本机安装对应的 Office 应用
- **Adobe 文档预览** — 支持预览 PS、AI 工程文件，不依赖本机安装 Adobe 应用
- **3D 模型预览** — 支持预览常见 3D 文件，如 STEP、STL、OBJ 等

### 可选依赖

- **Microsoft Edge WebView2 Runtime** — Markdown 等部分格式预览所需，Windows 11 通常已内置，若缺失可从[此处](https://developer.microsoft.com/en-us/microsoft-edge/webview2)下载安装
- **ffprobe.exe** — 允许显示扩展媒体元数据，可从 [FFmpeg 官网](https://ffmpeg.org/download.html) 获取，放入 Glance 安装目录或加入 `PATH`，缺失不影响基础音视频预览

## 开发

遵循 [GNU General Public License v3.0](LICENSE) 开源。功能请求、格式支持需求，以及问题与建议，请提交至 [Issues](https://github.com/ElluIFX/Glance/issues)。

> 提交问题时，请通过设置中的维护页使用诊断功能导出完整日志；如有隐私顾虑，请先脱敏后再提交。
