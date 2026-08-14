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

## Highlights

- Launch previews quickly from File Explorer, common file dialogs, and Everything
- Supports text, code, Markdown, images, audio/video, PDF, archives, Office, Adobe, 3D models, and more
- Efficient, reliable long-text preview with flexible image zoom and pan — modern essentials covered
- Pin preview windows and open multiple independent previews — turn files into sticky notes
- Remember window size and position by content type to match your workflow
- Fine-grained customization of behavior and display for personal preference
- Preloading optimized for high-volume, rapid preview workflows
- **No complex shortcuts; Space and your mouse are enough for full access**
- **Rigorous multi-process architecture with mutual supervision, staying responsive under demanding conditions**
- **No heavyweight plugin system; stay lean, reliable, and fast**

## Usage

Download and install Glance from [Releases](https://github.com/ElluIFX/Glance/releases). After launch, select a file and press Space to preview. Settings and Exit are available from the system tray.

### Optional components

Support for complex formats is provided by isolated components kept separate from the Glance core. The installer selects all components by default but allows them to be omitted, while the portable package includes every component and source.

- **PDF preview** — Previews multi-page PDF documents and provides document rendering for the Office component
- **Microsoft Office preview** — Previews Word, PowerPoint, and Excel files; depends on the PDF preview component and the corresponding locally installed Office applications
- **Archive preview** — Directly previews common compressed and archive formats such as ZIP, 7z, RAR, tar, and ISO
- **Adobe document preview** — Previews Photoshop PSD/PSB files and Illustrator AI files with PDF-compatible data without requiring Adobe applications
- **3D model preview** — Previews common 3D files such as STEP, STL, and OBJ
- **Advanced media information** — Shows codecs, bitrates, and stream details on demand, reusing ffprobe from `PATH` or downloading it automatically when unavailable

### Optional dependencies

- **Microsoft Edge WebView2 Runtime** — Used for rendered Markdown, web content, and 3D model previews. Usually included with Windows 11; if missing, download and install it from [here](https://developer.microsoft.com/en-us/microsoft-edge/webview2)

## Development

Released under the [GNU General Public License v3.0](LICENSE). Feature requests, format-support needs, and bug reports or suggestions are welcome at [Issues](https://github.com/ElluIFX/Glance/issues).

> When filing an issue, please export full logs via Diagnostics on the Maintenance page in Settings. If you have privacy concerns, sanitize the logs before submitting.
