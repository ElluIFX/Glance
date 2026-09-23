# Glance CLI

[中文](CLI.md) · [Home](../README.en.md)

Open previews, control windows, and change settings from your terminal, scripts, or file manager.

## Get started

Both installer and portable editions include `Glance.CLI.exe`. These examples use PowerShell. Set the executable path first; for a portable installation, use your extraction directory.

```powershell
$cli = 'C:\Program Files\Glance\Glance.CLI.exe'
& $cli preview 'C:\Documents\report.pdf'
```

Glance waits for the preview to be ready and prints full window details. The CLI connects to the running app or starts Glance from the same directory when needed.

Use `-h` at any level to see the relevant commands and options:

```powershell
& $cli -h
& $cli preview -h
& $cli window move -h
```

## Common tasks

| Task | Command (after `& $cli`) |
| --- | --- |
| Open a file or folder | `preview 'C:\Documents\report.pdf'` |
| Keep a preview on top | `preview 'C:\Documents\report.pdf' --topmost` |
| Keep a separate preview window | `preview 'C:\Documents\report.pdf' --pin` |
| Close after displaying for 5 seconds | `preview 'C:\Documents\report.pdf' --close-after 5` |
| List windows and their IDs | `windows` |
| Close the most recent preview | `window close` |
| Show app status and monitor indices | `status` |
| Check for a new release | `check-update` |
| Exit Glance | `quit` |

A file manager can invoke this command, replacing the final argument with the selected file's full path:

```text
"C:\Program Files\Glance\Glance.CLI.exe" preview "C:\Documents\report.pdf"
```

## Open a preview

```powershell
# Browse several files in one window
& $cli preview 'C:\Pictures\first.jpg' 'C:\Pictures\second.jpg'

# Set a size, center the window, and keep it pinned
& $cli preview 'C:\Documents\report.pdf' --size 1200 900 --center-offset 0 0 --pin

# Wait for the first content, then close after 5 seconds
& $cli preview 'C:\Documents\notes.txt' --close-after 5
```

| Option | Effect |
| --- | --- |
| `--wait` | Wait until the preview window closes |
| `--timeout SECONDS` | Bound the wait; 0 returns current state once the request is applied |
| `--pin` | Keep a separate, topmost preview window |
| `--topmost` | Keep this preview on top; defaults to off, enabled automatically by pinning |
| `--close-after SECONDS` | Close 0.001–86400 seconds after the first content is ready |
| `--size WIDTH HEIGHT` | Set outer window dimensions in physical pixels |
| `--position X Y` | Set the absolute top-left position; negative values are allowed |
| `--center-offset X Y` | Offset the window center from the work-area center; `0 0` centers it |
| `--monitor INDEX` | Choose a monitor for centering; see `status` for indices |

Choose one position mode. Centering a new preview defaults to the mouse monitor; moving an existing window defaults to its current monitor. Explicit position or size applies to the current preview. Later ordinary previews use the existing placement preferences.

The minimum outer size is a DPI-scaled 480 × 320. Each dimension is limited to 32767 pixels. Invalid sizes return an argument error.

Relative paths use your terminal's working directory. Quote paths containing spaces. Multiple paths form a sequence starting with the first item; callers expand wildcards. For paths starting with `--`, use the separator: `preview -- --notes.txt`.

The close timer belongs to the current content: switching files cancels it, while pinning preserves it.

### Preview standard input

`preview -` reads stdin to EOF, then previews a temporary file. Use `--name` to select a filename and extension; the default is `stdin.txt`.

```powershell
'{"name":"Glance","ready":true}' | & $cli preview - --name result.json
```

Use `--raw` for binary input. Bytes and line endings stay unchanged. The default filename is `stdin.bin`; specify an extension to select a previewer. Run this example in **cmd.exe** to preserve the source bytes:

```bat
"C:\Program Files\Glance\Glance.CLI.exe" preview - --raw --name photo.png < "C:\Pictures\photo.png"
```

Standard input occupies the entire preview request. `--name` accepts a filename. The temporary file remains available after the CLI returns and is cleaned up when the preview releases it.

## Control a window

Each new window has a unique UUID. Omitting `--id` targets the window most recently returned by `preview`, including after pinning. A closed target returns an error. Use `--id UUID` to select another window; find its UUID with `windows`.

```powershell
$result = & $cli preview 'C:\Documents\report.pdf' --pin --json | ConvertFrom-Json
$windowId = $result.data.id
& $cli window move --id $windowId --position 100 100
& $cli window resize --id $windowId --size 1200 900
& $cli window set 'C:\Documents\notes.txt' --id $windowId
& $cli window get --id $windowId
& $cli window close --id $windowId
```

| Command | Effect |
| --- | --- |
| `window get` | Show paths, loading state, position, size, and pinning state |
| `window set PATH [PATH ...]` | Replace the file or sequence, preserving UUID, geometry, pinning, and topmost state; accepts `--wait` |
| `window move --position X Y` | Move to absolute coordinates |
| `window move --center-offset X Y` | Position relative to a monitor center; accepts `--monitor INDEX` |
| `window resize --size WIDTH HEIGHT` | Change the outer window size |
| `window topmost on` / `window topmost off` | Change always-on-top state |
| `window pin on` / `window pin off` | Pin a window / close a pinned window |
| `window close` | Close the preview and leave Glance running |
| `window line N` | Go to plain-text line N, starting at 1 |
| `window page N` | Go to PDF page N, starting at 1 |
| `window seek POSITION` | Seek media by seconds or `HH:MM:SS[.fff]` |
| `window next` / `window previous` | Select an adjacent file in the sequence or gallery |
| `window play` / `window pause` | Play or pause media |
| `window volume N` | Set this window's volume, from 0 to 100 |
| `window mute on` / `window mute off` | Set this window's mute state |

All these commands accept `--id UUID`. UUIDs survive pinning and expire when Glance restarts. `windows` also lists the hidden dynamic window. Repeated `preview` commands reuse the ordinary preview window; after pinning, the next `preview` uses a new window.

**Pinning makes the window topmost and creates a new dynamic preview window. Unpinning closes the original window. Pinned windows must remain topmost.**

Content controls apply to the matching preview type. Out-of-range positions return errors. Volume and mute apply only to the current window.

The eye button in the plain-text status bar controls file monitoring: left-click to enable or pause one-second checks, or right-click to refresh once. Each new file starts with monitoring off.

## Change settings

Find the key and its allowed values before changing it. Keys are case-sensitive; copy them from `settings list`.

```powershell
& $cli settings list TextPreview
& $cli settings get TextPreview/FontSize
& $cli settings set TextPreview/WordWrap true
& $cli settings set TextPreview/FontSize 14
& $cli settings reset TextPreview/WordWrap
```

`settings list` returns all public settings; add a prefix to narrow the list. Each setting includes its current value, default, type, limits, and effect: `immediate` or `next_preview`. Settings from loaded components appear here too.

Booleans accept `true/false`, `on/off`, or `1/0`. Enumerations use the listed numeric values. Arrays use JSON, for example:

```powershell
& $cli settings set Footer/FieldOrder '[0,1,5,2,3,4]'
```

`settings reset KEY` restores one default. An empty `Appearance/Language` selects the system language; an empty `TextPreview/FontFamily` selects a font automatically. Use `reset` to restore these defaults too.

## Use in scripts

Add `--json` to receive one UTF-8 JSON object on stdout. Check the exit code before reading `data`:

```powershell
$result = & $cli preview 'C:\Documents\report.pdf' --pin --json |
    ConvertFrom-Json
if ($LASTEXITCODE -ne 0) {
    throw $result.error.message
}
& $cli window close --id $result.data.id
```

| Field | Meaning |
| --- | --- |
| `schema_version` | Output schema version, currently `1` |
| `ok` | Whether the request succeeded |
| `command` | Command name, such as `preview` or `window.get` |
| `data` | Success result; `null` on failure |
| `error` | Failure details: `code`, `name`, `message`; `null` on success |

Window IDs and preview generations are strings. Generic file information counts as ready content and is identified by `fallback: true`.

Text mode prints complete key-value results; errors go to stderr. Window results include UUID, paths, state, bounds, pinning, and topmost state, plus available text-line, PDF-page, or media playback information.

### Common options and timeouts

| Option | Effect |
| --- | --- |
| `-h` / `--help` | Show help for the current command; `help COMMAND` also works |
| `--json` | Output JSON for scripts |
| `--no-start` | Connect only to a running Glance |
| `--timeout SECONDS` | Bound a window command's wait to 0–86400 seconds |
| `--wait` | Wait for the target to close with `preview` or a `window` command that keeps it open |
| `--version` | Show the version |

By default, commands wait for content readiness or control completion. `--timeout 0` returns after the request is applied. A positive timeout returns current state with `wait_completed: false` when the wait expires; completion returns `wait_completed: true`. `--wait` waits for window closure, including a hidden dynamic preview. Combining it with `--timeout` applies one total waiting limit.

Connection allows 15 seconds, ordinary request transport and execution allow 10 seconds, and update checks allow 60 seconds. Failures at these stages return errors. Reading stdin is separate from content waiting. Accepted previews and controls continue when the CLI exits or Ctrl+C interrupts its wait.

`check-update` reports versions and release/download links; downloading and installation are separate user actions. `quit` waits for App and Core to exit and also succeeds when Glance is already stopped.

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success, including no update or an already stopped app |
| `1` | Internal error |
| `2` | Invalid argument, setting value, or request |
| `3` | File, window, setting, or monitor not found |
| `4` | App unavailable or failed to start |
| `5` | Timeout or connection lost; the operation outcome may be unknown |
| `6` | Incompatible protocol or response |
| `7` | Access denied |
| `8` | State conflict, such as disabling topmost on a pinned window |
| `9` | Preview loading failed |
| `10` | Preview replaced/closed or request cancelled |
| `11` | Update check failed |
| `130` | Interrupted with Ctrl+C or Ctrl+Break |
