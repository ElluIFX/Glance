# Changelog

All notable changes to Glance are documented in this file.

The project uses date-based versions in `YYYY.MM.DD[.revision]` format.

## [2026.07.25.1] - 2026-07-25

### Fixed

- Prepared the preview window before its first visible frame to prevent black title and status bars during opening.
- Prevented stale WebView2 content from flashing while reopening or switching Markdown previews.
- Reduced Markdown preview latency and improved README rendering with GitHub-style layout, relative images, and embedded HTML.

## [2026.07.25] - 2026-07-25

### Changed

- Added left-button panning for enlarged PDF previews and Office documents converted to PDF.
- Selected the default text preview font from common installed Windows monospace fonts.
- Simplified the Office background preparation status text.

### Fixed

- Reduced accumulated WebView2 processes by sharing one environment and user data directory across previews.
- Closed inactive WebView2 controls after one minute and released them immediately when preview windows are destroyed.

## [2026.07.24.1] - 2026-07-24

### Changed

- Replaced incremental Office rendering and application-managed cache queues with background PDF preparation keyed by file identity.
- Office source documents are copied with shared access and released promptly while completed previews remain reusable from the system temporary directory.

### Fixed

- Preserved Space input while renaming files or typing in other Explorer text fields.
- Prevented duplicate Office conversions when a second request arrives as the first conversion completes.

## [2026.07.24] - 2026-07-24

### Added

- Detailed audio and video information with optional extended metadata from `ffprobe`.
- Configurable Office preview caching with bounded queue size and expiration.
- An option to reverse the media seek wheel direction.

### Changed

- Reduced the default text preview font size.
- Cached WebView availability after startup to avoid repeated runtime probing.
- Extended installer support to Windows 10.

### Fixed

- Improved text preview responsiveness, scrollbar theme stability, and opacity consistency.
- Cleared stale Office navigation content while switching documents.

## [2026.07.23.2] - 2026-07-23

### Fixed

- Restored Scintilla and Lexilla runtime dependencies during clean local and GitHub Actions builds.

## [2026.07.23.1] - 2026-07-23

### Added

- Local HTML, XHTML, MHTML, SVG, and related web document previews with Preview and Code modes.
- Manual update checks from the About page.
- Additional syntax highlighting themes for text and source previews.
- Optional removal of settings, diagnostics, and cached previews during uninstall.

### Changed

- Replaced the text rendering path with Scintilla and Lexilla for unified large-file loading, wrapping, line numbers, selection, and syntax highlighting.
- Reduced idle preview memory and bounded Scintilla layout threads on high-core-count systems.
- Install and upgrade now request a graceful Glance shutdown before replacing files, with Restart Manager retained as a compatibility fallback.

### Fixed

- Stabilized encrypted preview interaction, archive error fallback, and preview window lifecycle transitions.
- Prevented legacy installations that do not recognize the shutdown command from blocking an upgrade.
- Ensured locked user cache files are scheduled for deletion on restart when they cannot be removed during uninstall.

## [2026.07.23] - 2026-07-23

### Added

- Fast Space-key previews from File Explorer, the Windows desktop, common file dialogs, and Everything 1.4 or 1.5.
- Preview providers for text, source code, Markdown, images, audio, video, PDF, archives, folders, Word, Excel, and PowerPoint.
- Incremental large-text loading with encoding detection, line numbers, syntax highlighting, themes, wrapping, and font-size controls.
- Isolated PDFium rendering and isolated Office conversion, including on-demand Word page rendering.
- Image zoom, pan, rotation, fit, metadata, and system file icon support.
- Archive and folder tree views with sorting, metadata, password prompts, and bounded enumeration.
- Pinned and always-visible preview windows, independent preview instances, per-type size and position memory, and media auto fit.
- English and Simplified Chinese interfaces with live language and theme switching.
- Configurable footer metadata, media playback behavior, diagnostics export, runtime availability status, and settings reset.
- Mutual App/Core supervision and independent selection-worker recovery.
- Self-contained x64 installer, portable archive, debug symbols, SHA-256 checksums, and automated GitHub Release packaging.

### Changed

- Preview operations release original file handles promptly so files remain available to other applications.
- The release package excludes optional `ffprobe.exe`; media preview remains available without extended technical metadata.
- The Windows App SDK payload is restricted to components required by current functionality.

### Fixed

- Recovered global preview input after Core or App failures without retaining blocked Space-key input.
- Improved preview switching, pinned-window lifecycle, adaptive sizing, image rotation, large-text rendering, and archive enumeration stability.
- Prevented stalled Shell selection queries and expensive directory rendering from taking down the preview workflow.

[2026.07.25]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.25
[2026.07.24.1]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.24.1
[2026.07.24]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.24
[2026.07.23.2]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.23.2
[2026.07.23.1]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.23.1
[2026.07.23]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.23
