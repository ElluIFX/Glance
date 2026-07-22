<p align="center">
  <img src="src/Glance.App/Assets/AppIcon.png" width="128" height="128" alt="Glance">
</p>

<h1 align="center"><b>Glance · 一瞥</b></h1>

<p align="center"><b>Press Space to preview files on Windows instantly</b></p>

<p align="center">
  <a href="README.md">中文</a>
  ·
  <b>English</b>
</p>

<p align="center">
  <a href="https://github.com/ElluIFX/Glance/releases">Download</a>
  ·
  <a href="https://github.com/ElluIFX/Glance/issues">Issues</a>
  ·
  <a href="LICENSE">GPL-3.0</a>
</p>

---

<p align="center">
Opening a full app just to peek at a file should not be the default. Glance gives you a fast, quiet preview so browsing and filtering files stays direct.
</p>

<p align="center">
Select a file and press Space to open the preview. Press Space again, or let the window lose focus, and it closes.
</p>

<p align="center">
Compared with similar tools, Glance prioritizes stability and reliability — <b>ready whenever you need a preview</b>.
</p>

<p align="center">
  <img src="docs/screenshot.png" alt="Glance Screenshot" style="max-width: 90%; border-radius: 8px;">
</p>

## ✨ Highlights

- Launch previews quickly from File Explorer, common file dialogs, and Everything
- Supports text, code, Markdown, images, audio/video, PDF, archives, Office, and more
- Efficient, reliable long-text preview with flexible image zoom and pan — modern essentials covered
- Pin preview windows and open multiple independent previews — turn files into sticky notes
- Remember window size and position by content type to match your workflow
- Fine-grained customization of behavior and display for personal preference

### 🎯 What matters most

- **Low friction** — No complex shortcuts; Space and your mouse are enough for full access
- **High reliability** — Rigorous multi-process architecture with mutual supervision, staying responsive under demanding conditions
- **Lightweight** — No heavyweight plugin system; stay lean, reliable, and fast

## 🚀 Usage

Download and install Glance from [Releases](https://github.com/ElluIFX/Glance/releases). After launch, select a file and press Space to preview. Settings and Exit are available from the system tray.

#### Optional dependencies

- **Microsoft Edge WebView2 Runtime** — Required for some formats such as Markdown. Usually included with Windows 11; if missing, install it from the [Microsoft site](https://developer.microsoft.com/en-us/microsoft-edge/webview2)
- **Microsoft Office** — Required for Word / Excel / PowerPoint preview; the corresponding components must be installed
- **ffprobe.exe** — Used for extended media metadata. Get it from the [FFmpeg download page](https://ffmpeg.org/download.html), then place it in the Glance install directory or add it to `PATH`; basic audio/video preview still works without it

## 🛠️ Development

Released under the [GNU General Public License v3.0](LICENSE). Feature requests, format-support needs, and bug reports or suggestions are welcome at [Issues](https://github.com/ElluIFX/Glance/issues).

> When filing an issue, please export full logs via Diagnostics on the Maintenance page in Settings. If you have privacy concerns, sanitize the logs before submitting.
