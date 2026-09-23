# Glance CLI

[中文](CLI.md) · [Glance](../README.en.md)

`Glance.CLI.exe` lets terminals, scripts, and file managers open previews, control windows, and manage settings. Both installer and portable distributions include it. Run it from the Glance directory or use its full path.

```powershell
.\Glance.CLI.exe preview 'C:\Documents\report.pdf'
.\Glance.CLI.exe preview 'C:\Documents\report.pdf' --pin --wait --json
.\Glance.CLI.exe windows
```

The CLI first connects to the running application from the same installation, starting it when unavailable. Use `--no-start` to connect only to an existing instance. Help, version queries, and argument validation run locally. File managers can invoke `Glance.CLI.exe preview "full path to the selected file"`.

## Previews and windows

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

- `preview` accepts one or more files or folders as a sequence in one window, starting with the first item. Relative paths resolve against the caller's working directory. Quote paths containing spaces and use `--` before paths beginning with `--`. Callers expand wildcards; inputs are filesystem paths.
- Success normally means the preview request has been applied. Text output contains only the actual window ID. `--wait` waits for the first content to become ready and reports loading failures or replacement/closure. A generic information preview is a valid result, identified by `fallback` in JSON.
- `--close-after` takes positive seconds, including fractions, measured from the first ready content. The timer belongs to that preview and is cancelled when its content changes. Pinning preserves the timer on the original window.
- The default `--id 0` addresses the current dynamic preview window. Actual IDs survive pinning and expire when the application restarts. JSON encodes IDs and preview generations as strings.
- Pinning makes a window topmost and detaches it; a new dynamic window is created. Unpinning closes the detached window. Pinned windows remain topmost.
- `windows` includes the hidden dynamic window. Window descriptions include paths, current index, loading state, bounds, pinning, and topmost state.

### Position and size

All coordinates and sizes use physical pixels. Size refers to the outer window frame. `--position` specifies the top-left corner in virtual desktop coordinates, including negative values. `--center-offset` specifies the window center relative to a monitor's work-area center. `status` lists monitor indices and work areas.

A new preview defaults to the monitor containing the mouse pointer; moving an existing window defaults to its current monitor. `--monitor` applies to center-offset mode. Explicit geometry overrides automatic fitting and remembered placement for the current preview. The minimum size scales with DPI from a logical 480 × 320; invalid sizes return an argument error.

```powershell
.\Glance.CLI.exe preview 'C:\Documents\report.pdf' --size 1200 900 --center-offset 0 0 --pin
.\Glance.CLI.exe window move --id 12345 --position -1200 100
.\Glance.CLI.exe window close --id 12345
```

Replace `12345` with the ID returned by a preview command.

## Settings and management

```text
status
settings list [PREFIX]
settings get KEY
settings set KEY VALUE
settings reset KEY
check-update
quit
```

The application supplies its public settings and loaded component settings dynamically. Keys retain existing group and value names; use the spelling returned by `settings list`. Descriptions include type, current value, default, limits, choices, and effect: `immediate` or `next_preview`.

```powershell
.\Glance.CLI.exe settings list TextPreview --json
.\Glance.CLI.exe settings set TextPreview/MonitorFile true
.\Glance.CLI.exe settings set TextPreview/RefreshIntervalMs 1000
.\Glance.CLI.exe settings reset TextPreview/MonitorFile
.\Glance.CLI.exe settings set Footer/FieldOrder '[0,1,5,2,3,4]'
```

Booleans accept `true/false`, `on/off`, and `1/0`; enumerations retain native numeric values. An empty `Appearance/Language` selects the system language, and an empty `TextPreview/FontFamily` selects a default font automatically. Caches, remembered window placement, and authorization tasks remain internal state.

`check-update` only reports versions, update availability, and release/download links. No available update is also success. `quit` waits for App and Core to exit and succeeds immediately when already stopped.

## Output and exit codes

Text is the default format. Use `--json` for automation: stdout contains one UTF-8 object with `schema_version`, `ok`, `command`, `data`, and `error`. Text-mode errors go to stderr.

```json
{"schema_version":1,"ok":false,"command":"window.get","data":null,"error":{"code":3,"name":"window_not_found","message":"Window ID not found"}}
```

| Exit code | Meaning |
| --- | --- |
| 0 | Success, including no available update or an already stopped application |
| 1 | Internal error |
| 2 | Invalid argument, setting value, or request |
| 3 | File, window, setting, or monitor not found |
| 4 | Application unavailable or failed to start |
| 5 | Timeout or disconnected transport; the operation outcome may be unknown |
| 6 | Incompatible protocol or response |
| 7 | Access denied |
| 8 | State conflict, such as disabling topmost on a pinned window |
| 9 | Preview loading failed |
| 10 | Preview replaced/closed or request cancelled |
| 11 | Update check failed |
| 130 | Interrupted with Ctrl+C or Ctrl+Break |

Ordinary commands allow 10 seconds, cold connection establishment allows 15 seconds, `preview --wait` allows 30 seconds, and update checks allow 60 seconds. Global `--timeout SECONDS` overrides the connection and command limits separately. After a timeout, query the window state before retrying a modifying command.

Accepted previews and close timers continue after the CLI exits or its wait is interrupted. Use `--help` for the command list and `--version` for the product version.
