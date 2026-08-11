# Changelog

All notable changes to Glance are documented in this file.

The project uses date-based versions in `YYYY.MM.DD[.revision]` format.

## [2026.08.11.1] - 2026-08-11

### Added

- Added an optional source extension framework and Everything source for previewing the focused result with gallery navigation and synchronized selection.
- Added source location and health status to the Add-ons settings page.

### Changed

- Portable builds now include every supported component and source, while the installer keeps them selectable under separate add-on groups.
- Release packaging now distributes add-ons through the installer and complete portable archive.

## [2026.08.11] - 2026-08-11

### Added

- Added gallery navigation for images, audio, and video using the current File Explorer order, synchronized selection, continuous scrolling, and adjacent-image preloading.
- Added an optional advanced media information component with on-demand, verified FFprobe preparation after user confirmation.

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
