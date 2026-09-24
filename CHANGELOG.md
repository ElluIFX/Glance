# Changelog

All notable changes to Glance are documented in this file.

The project uses date-based versions in `YYYY.MM.DD[.revision]` format.

## [2026.09.24.2] - 2026-09-24

### Added

- Preview fonts with transparent samples, adjustable size and supported variable weights, font metadata, and Windows installation actions for the current user or all users.
- Preview EXE and DLL summaries with file icons, dependencies and resource counts, plus detailed tables for imports, exports, PE structure and .NET members.
- Added CLI `--quiet`, the `main` window alias, window activation and full-window controls, and text-loading completeness reporting.

### Improved

- Keep font controls in one row and metadata in a separate scrollable column; fixed styles and weights remain visible as disabled controls.
- Use equal-width, wrapping columns for executable details and keep the detail toggle in the preview header.
- Allow the CLI to run from a separate directory when Glance is already running.
- Apply the window backdrop to structured JSON previews.

### Fixed

- Separate remembered window sizes and positions by preview provider, including components that share a rendering surface.
- Prevent preview buttons from capturing keyboard focus and consuming Space.
- Refresh update-frequency labels when changing the interface language.
- Release component views when switching files and cancel font rendering without blocking the preview window.

## [2026.09.23] - 2026-09-23

### Added

- Added a lightweight native CLI for opening previews, controlling windows, managing public settings, checking updates, and quitting Glance, with JSON output and documented exit codes.

### Improved

- Preload adjacent PDF pages and cancel stale work when navigating quickly.
- Reuse protected scheduled-task authorization for elevated Core startup to reduce repeated UAC prompts.
- Remove unused Windows App SDK files from release packages and clean obsolete files during upgrades.

### Fixed

- Fixed adaptive window sizing for Office previews and restored remembered sizes for components excluded from adaptive sizing.

## [2026.09.20] - 2026-09-20

### Improved

- Show DOCX previews sooner with an initial document view followed by the complete content while preserving the reading position.
- Render XLSX worksheets in a background worker with virtualized cells and on-demand pictures and charts.
- Load PPTX slides as they become visible and stream embedded audio and video in large presentations on demand.
- Recognize Open XML documents saved with legacy Office extensions and improve cancellation during native Office preview loading.

## [2026.09.15] - 2026-09-15

### Improved

- Preserve readable text around isolated undecodable bytes and display those bytes as red hexadecimal boxes without adding display digits to copied text.
- Automatically follow file updates while scrolled to the bottom; scrolling up keeps the current reading position, and returning to the bottom resumes following.
- Simplified file monitoring settings and descriptions.

### Fixed

- Fixed text refresh scrolling through the document and stopping above the final line, including wrapped lines.
- Fixed log highlighting failing to return after being switched off and on.
- Fixed unreliable JSON path tooltips and removed the artificial `root` prefix.

## [2026.09.14] - 2026-09-14

### Added

- Added optional file monitoring for plain-text previews, with a configurable polling interval, incremental updates, and optional scrolling to the latest content.
- Added log highlighting for timestamps, severity levels, and field names through extensible highlighting rules.
- Added JSON node path tooltips, such as `root.data.items[3]`.

### Improved

- Reduced image preview memory usage with viewport-sized decoding and higher-resolution loading on zoom.
- Reduced PDF thumbnail work and kept document closing off the UI thread.
- Reduced panorama playback frame copying and released full-resolution decoders in the background when returning to proxy playback.
- Reduced CAD draw calls by merging adjacent faces with the same material.
- Bounded archive metadata scans, PSD refinement memory, and concurrent component preparation; obsolete preview requests are cancelled.
- Moved unknown-format probing off the UI thread.

### Fixed

- Preserved the current preview when Everything refreshes its results without changing the selected file.
- Rejected stale PDF operations after switching documents.
- Preserved temporary preview files owned by running Glance instances.

## [2026.09.04] - 2026-09-04

### Added

- Added interactive 3D previews for Insta360 INSV and DJI OSV panorama videos.
- Added synchronized dual 180-degree source view switching with shared playback controls.
- Added live per-format view-angle and overlap settings for panorama stitching.

## [2026.08.25] - 2026-08-25

### Added

- Added progressive structured previews for JSON, JSONL, and NDJSON with depth controls, document statistics, and complete node copying.
- Added `Ctrl+C` copying for selected text in the text previewer, with the selection cleared after copying.

### Changed

- Unified component-provided setting text with the application localization pipeline.

## [2026.08.19] - 2026-08-19

### Changed

- Office previews now use locally installed Preview Handlers for direct Word, PowerPoint, and Excel rendering without the PDF component dependency.
- Renamed the rich-document resolution setting to PDF preview resolution and scoped it to PDF rendering.

### Fixed

- Automatically previews blocked network Office files through temporary local copies and shows a brief warning.
- Keeps preview notices and update dialogs visible above native Office, text, and web preview surfaces.

## [2026.08.18] - 2026-08-18

### Added

- Added fullscreen preview with edge-triggered title and shortcut bars.
- Added an optional setting to toggle fullscreen by double-clicking preview content.

## [2026.08.15.1] - 2026-08-15

### Added

- Added periodic automatic update checks with configurable intervals, deferred prompts, and per-version skipping.
- Added optional Windows acrylic backgrounds with adjustable acrylic opacity on supported systems.
- Added right-click JSON copying for raw EXIF and advanced media metadata.
- Added a temporary same-extension navigation mode to the gallery button.

### Changed

- Consolidated image zoom controls into a smooth slider with right-click reset and active transform indicators.
- Dependent settings now appear with a short animation when their parent feature is enabled.

### Fixed

- Prepared the settings window before its first visible frame to prevent a black opening frame.

## [2026.08.15] - 2026-08-15

### Added

- Added optional HEIC, AVIF, and camera RAW preview components.
- Added detailed grouped EXIF information and an optional capture-time field in the preview footer, including metadata from component-provided image formats.
- Added previews for files exposed through MTP devices and other non-filesystem Shell sources.
- Added an option to restrict gallery navigation to files with the same extension.

### Changed

- Refined the Add-ons and Maintenance settings layout, removed nested status-list scrolling, and added a WebView2 download link when the runtime is unavailable.
- Update prompts now prioritize downloading and installing the available release.
- Simplified the optional ffprobe download prompts and status text.

### Fixed

- Stabilized file materialization from MTP devices that expose their contents through Shell streams.

## [2026.08.11.2] - 2026-08-11

### Added

- Added an optional source extension framework and Everything source for previewing the focused result with gallery navigation and synchronized selection.
- Added source location and health status to the Add-ons settings page.

### Changed

- Portable builds now include every supported component and source, while the installer keeps them selectable under separate add-on groups.
- Release packaging now distributes add-ons through the installer and complete portable archive.

## [2026.08.11] - 2026-08-11

### Added

- Added gallery navigation for images, audio, and video using the current File Explorer order, synchronized selection, continuous scrolling, and adjacent-image preloading.
- Added an optional advanced media information component with on-demand, verified ffprobe preparation after user confirmation.

### Fixed

- Sorted component status entries alphabetically and restored component size estimates in the installer.
- Kept long names readable in multi-file previews by compacting their middle while preserving the beginning and extension.

## [2026.08.06.3] - 2026-08-06

### Added

- Added an optional archive preview component for ZIP, 7z, RAR, tar, ISO, and other common archive formats without full extraction.

### Changed

- Moved PDF preview and PDFium into an optional component with dependency-aware Office installation and health reporting.
- Moved rich-document rendering settings to Document Preview and registered them through the PDF component.

### Fixed

- Restored permission information in the preview footer.

## [2026.08.06.2] - 2026-08-06

### Added

- Added a built-in updater with streamed downloads, progress reporting, cancellation, SHA-256 verification, and silent installation.

### Changed

- Installer upgrades preserve the previously selected optional components and select newly introduced components by default.
- Installed builds prioritize direct updates, while portable builds prioritize the Release page and retain direct installation as an option.
- Successful automatic updates restart Glance under the original user account.

## [2026.08.06] - 2026-08-06

### Added

- Added counterclockwise image rotation, horizontal and vertical flipping, and fine-grained right-click zoom controls.
- Added an optional image zoom map with click-and-drag viewport navigation.
- Added right-click file copying to the footer's copy-path command.

### Changed

- Improved process watchdog timing, stalled selection recovery, component cache maintenance, and component host cleanup.

### Fixed

- Reset image zoom and transform state when switching or reopening images.
- Added an effective transaction timeout for stalled PDF rendering and corrected pipe and Adobe host handle cleanup.

## [2026.07.27] - 2026-07-27

### Added

- Added configurable media information to the preview footer, including image bit depth.
- Added nested folder navigation with Space or double-click, Escape navigation to the parent folder, and restoration of the previous selection and scroll position.
- Added background system thumbnails for folder entries with Fluent icons retained as fallback.
- Added an isolated optional component framework with component status reporting, installer selection, and standalone component archives.
- Added progressive Photoshop document previews, PDF-compatible Illustrator previews, and configurable rich-document rendering resolution.
- Added interactive 3D previews for common mesh formats and STEP, IGES, and BREP CAD files.

### Changed

- Moved Microsoft Office preview support into an optional component while retaining its existing conversion cache and isolated host.
- The installer selects all optional components by default, while the portable package contains only the core application.
- Component health now reflects required host capabilities such as WebView2 availability.

### Fixed

- Reset PDF and converted document zoom and scroll position after the new document completes its final layout.

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

[2026.08.15]: https://github.com/ElluIFX/Glance/releases/tag/v2026.08.15
[2026.08.06.3]: https://github.com/ElluIFX/Glance/releases/tag/v2026.08.06.3
[2026.08.06.2]: https://github.com/ElluIFX/Glance/releases/tag/v2026.08.06.2
[2026.08.06]: https://github.com/ElluIFX/Glance/releases/tag/v2026.08.06
[2026.07.27]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.27
[2026.07.25.1]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.25.1
[2026.07.25]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.25
[2026.07.24.1]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.24.1
[2026.07.24]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.24
[2026.07.23.2]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.23.2
[2026.07.23.1]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.23.1
[2026.07.23]: https://github.com/ElluIFX/Glance/releases/tag/v2026.07.23
