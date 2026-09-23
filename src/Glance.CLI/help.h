#pragma once
#include "glance/contracts/cli_protocol.h"
#include <string>
#include <string_view>

namespace glance::cli
{
    inline std::string help_text(std::string_view topic)
    {
        struct Page
        {
            std::string_view topic;
            std::string_view description;
            std::string_view usage;
            std::string_view details;
            std::string_view examples;
        };
        static constexpr Page pages[]{
            { "", "Preview files and control Glance from your terminal.",
              "Glance.CLI.exe [-h] [--version] COMMAND ...",
              "commands:\n"
              "  preview                 Open one or more files or folders\n"
              "  windows                 List preview windows and their IDs\n"
              "  window                  Control windows and preview content\n"
              "  settings                List, read, change, or reset public settings\n"
              "  status                  Show app version, connection, and monitors\n"
              "  check-update            Check for a newer release\n"
              "  quit                    Exit Glance and its Core process\n"
              "  help                    Show help for a command\n",
              "Glance.CLI.exe preview \"C:\\Documents\\report.pdf\"\n"
              "  Glance.CLI.exe preview photo.jpg --pin --size 1000 700\n"
              "  Glance.CLI.exe preview --help" },
            { "preview", "Open files or folders in a preview window.",
              "Glance.CLI.exe preview [-h] [options] PATH [PATH ...]",
              "arguments:\n"
              "  PATH                    File or folder; multiple paths form a sequence\n"
              "\npreview options:\n"
              "  --name NAME             Standard-input filename (default: stdin.txt)\n"
              "  --raw                   Binary stdin; default filename: stdin.bin\n"
              "  --pin                   Keep a separate, topmost preview window\n"
              "  --topmost               Keep the window on top (default: off)\n"
              "  --close-after SECONDS    Close after content is ready (0.001 to 86400)\n"
              "  --size WIDTH HEIGHT     Outer window size in physical pixels\n"
              "  --position X Y          Absolute top-left position\n"
              "  --center-offset X Y     Offset from the monitor work-area center\n"
              "  --monitor INDEX         Monitor for centering (default: mouse monitor)\n"
              "  --                      Treat all remaining arguments as paths\n"
              "\nnotes:\n"
              "  Waits for readiness and returns full window details.\n"
              "  PATH '-' reads stdin to EOF into a temporary file.\n"
              "  Use --raw --name image.png for binary input; bytes stay unchanged.\n"
              "  Multiple paths form one preview sequence.\n"
              "  Relative paths use your current directory. Quote paths with spaces.\n"
              "  Position modes are exclusive; negative coordinates are allowed.\n"
              "  Centering defaults to the monitor containing the mouse pointer.\n"
              "  Explicit geometry overrides automatic sizing for this preview.\n"
              "  The close timer is cancelled when the preview content changes.\n",
              "Glance.CLI.exe preview \"C:\\Documents\\report.pdf\"\n"
              "  Glance.CLI.exe preview photo.jpg --size 1000 700 --center-offset 0 0\n"
              "  Glance.CLI.exe preview notes.txt --pin --wait --close-after 5\n"
              "  Glance.CLI.exe preview first.jpg second.jpg --json" },
            { "window", "Control an existing preview window.",
              "Glance.CLI.exe window COMMAND [options]",
              "commands:\n"
              "  get                     Show paths, state, and window bounds\n"
              "  set                     Switch the previewed file or sequence\n"
              "  move                    Set position or center offset\n"
              "  resize                  Set the outer window size\n"
              "  close                   Close the preview\n"
              "  topmost                 Turn always-on-top on or off\n"
              "  pin                     Keep a preview open, or close a pinned preview\n"
              "  line N                  Go to a plain-text line (1-based)\n"
              "  page N                  Go to a PDF page (1-based)\n"
              "  seek POSITION           Seek media by seconds or HH:MM:SS[.fff]\n"
              "  next / previous         Select an adjacent file in the sequence\n"
              "  play / pause            Control media playback\n"
              "  volume N                Set window volume from 0 to 100\n"
              "  mute {on,off}           Set window mute state\n"
              "\noptions:\n"
              "  --id UUID               Target window (default: last preview UUID)\n"
              "\nwindow IDs:\n"
              "  Use 'windows' to list IDs or read data.id from preview --json.\n"
              "  UUIDs survive pinning and expire when Glance restarts.\n"
              "  A closed default target returns an error.\n",
              "Glance.CLI.exe windows\n"
              "  Glance.CLI.exe window close\n"
              "  Glance.CLI.exe window move --help" },
            { "window get", "Show a window's paths, loading state, and bounds.",
              "Glance.CLI.exe window get [--id UUID]",
              "options:\n  --id UUID               Target window (default: last preview UUID)\n",
              "Glance.CLI.exe window get --json" },
            { "window close", "Close a preview; Glance remains running.",
              "Glance.CLI.exe window close [--id UUID]",
              "options:\n  --id UUID               Target window (default: last preview UUID)\n",
              "Glance.CLI.exe window close" },
            { "window set", "Switch files in an existing window, preserving its UUID and geometry.",
              "Glance.CLI.exe window set PATH [PATH ...] [--id UUID] [--wait]",
              "arguments:\n  PATH                    File or folder; multiple paths form a sequence\n"
              "\noptions:\n"
              "  --id UUID               Target window (default: last preview UUID)\n"
              "\n  Preserves pinning and topmost state; cancels the old close timer.\n",
              "Glance.CLI.exe window set notes.txt" },
            { "window move", "Move a window using physical pixel coordinates.",
              "Glance.CLI.exe window move --position X Y [--id UUID]\n"
              "  Glance.CLI.exe window move --center-offset X Y [options]",
              "options:\n"
              "  --id UUID               Target window (default: last preview UUID)\n"
              "  --position X Y          Absolute top-left corner; negatives allowed\n"
              "  --center-offset X Y     Offset from the work-area center\n"
              "  --monitor INDEX         Monitor for centering (default: current)\n"
              "\n  Choose one position mode. Use 'status' to list monitor indices.\n",
              "Glance.CLI.exe window move --position -1200 100\n"
              "  Glance.CLI.exe window move --center-offset 0 0 --monitor 0" },
            { "window resize", "Resize the outer window frame in physical pixels.",
              "Glance.CLI.exe window resize --size WIDTH HEIGHT [--id UUID]",
              "options:\n"
              "  --id UUID               Target window (default: last preview UUID)\n"
              "  --size WIDTH HEIGHT     Minimum: DPI-scaled 480 x 320; max: 32767\n",
              "Glance.CLI.exe window resize --size 1200 900" },
            { "window topmost", "Set whether a preview stays above other windows.",
              "Glance.CLI.exe window topmost {on,off} [--id UUID]",
              "options:\n  --id UUID               Target window (default: last preview UUID)\n"
              "\n  Pinned windows must remain topmost.\n",
              "Glance.CLI.exe window topmost on" },
            { "window pin", "Keep the current preview in a separate window.",
              "Glance.CLI.exe window pin {on,off} [--id UUID]",
              "options:\n  --id UUID               Target window (default: last preview UUID)\n"
              "\n  'on' makes the window topmost and creates a new dynamic preview.\n"
              "  'off' closes the pinned window. Returns full window details.\n",
              "Glance.CLI.exe window pin on\n"
              "  Glance.CLI.exe window pin off" },
            { "window line", "Go to a plain-text line, loading more text as needed.",
              "Glance.CLI.exe window line N [--id UUID] [options]",
              "arguments:\n  N                       Line number starting at 1\n",
              "Glance.CLI.exe window line 120" },
            { "window page", "Go to a PDF page and wait for rendering.",
              "Glance.CLI.exe window page N [--id UUID] [options]",
              "arguments:\n  N                       Page number starting at 1\n",
              "Glance.CLI.exe window page 8" },
            { "window seek", "Seek within the current media file.",
              "Glance.CLI.exe window seek POSITION [--id UUID] [options]",
              "arguments:\n  POSITION                Seconds or HH:MM:SS[.fff]\n",
              "Glance.CLI.exe window seek 00:01:30" },
            { "window next", "Preview the next file in the sequence or gallery.",
              "Glance.CLI.exe window next [--id UUID] [options]", "", "Glance.CLI.exe window next" },
            { "window previous", "Preview the previous file in the sequence or gallery.",
              "Glance.CLI.exe window previous [--id UUID] [options]", "", "Glance.CLI.exe window previous" },
            { "window play", "Play the current media file.",
              "Glance.CLI.exe window play [--id UUID] [options]", "", "Glance.CLI.exe window play" },
            { "window pause", "Pause the current media file.",
              "Glance.CLI.exe window pause [--id UUID] [options]", "", "Glance.CLI.exe window pause" },
            { "window volume", "Set the current window's media volume.",
              "Glance.CLI.exe window volume N [--id UUID] [options]",
              "arguments:\n  N                       Volume from 0 to 100\n", "Glance.CLI.exe window volume 50" },
            { "window mute", "Set the current window's media mute state.",
              "Glance.CLI.exe window mute {on,off} [--id UUID] [options]", "", "Glance.CLI.exe window mute on" },
            { "windows", "List all preview windows, including the hidden dynamic window.",
              "Glance.CLI.exe windows [options]",
              "output:\n  Each window includes its ID, paths, state, and bounds.\n",
              "Glance.CLI.exe windows --json" },
            { "settings", "Discover and change Glance's public settings.",
              "Glance.CLI.exe settings COMMAND [arguments] [options]",
              "commands:\n"
              "  list [PREFIX]           List settings, optionally filtered by key prefix\n"
              "  get KEY                 Read a setting and its allowed values\n"
              "  set KEY VALUE           Change a setting\n"
              "  reset KEY               Restore a setting's default\n"
              "\nvalues:\n"
              "  Copy exact keys from 'settings list'. Booleans accept true/false,\n"
              "  on/off, or 1/0. Each setting reports its type, limits, and effect.\n",
              "Glance.CLI.exe settings list TextPreview\n"
              "  Glance.CLI.exe settings set TextPreview/WordWrap true\n"
              "  Glance.CLI.exe settings reset TextPreview/WordWrap" },
            { "settings list", "List settings with their values, defaults, and limits.",
              "Glance.CLI.exe settings list [PREFIX] [options]",
              "arguments:\n  PREFIX                  Key prefix (default: all public settings)\n",
              "Glance.CLI.exe settings list TextPreview --json" },
            { "settings get", "Read one setting, including its type and allowed values.",
              "Glance.CLI.exe settings get KEY [options]",
              "arguments:\n  KEY                     Exact key returned by 'settings list'\n",
              "Glance.CLI.exe settings get TextPreview/FontSize" },
            { "settings set", "Change a public setting.",
              "Glance.CLI.exe settings set KEY VALUE [options]",
              "arguments:\n"
              "  KEY                     Exact key returned by 'settings list'\n"
              "  VALUE                   Value matching the setting's type and allowed range\n"
              "\n  Booleans: true/false, on/off, or 1/0. Enumerations use numeric IDs.\n"
              "  The result reports whether the setting applies immediately or\n"
              "  on the next preview. Use 'settings reset' to restore its default.\n",
              "Glance.CLI.exe settings set TextPreview/WordWrap true\n"
              "  Glance.CLI.exe settings set TextPreview/FontSize 14" },
            { "settings reset", "Restore one setting's default value.",
              "Glance.CLI.exe settings reset KEY [options]",
              "arguments:\n  KEY                     Exact key returned by 'settings list'\n",
              "Glance.CLI.exe settings reset TextPreview/WordWrap" },
            { "status", "Show the app version, Core connection, and monitor work areas.",
              "Glance.CLI.exe status [options]",
              "output:\n  Monitor indices can be used with --center-offset and --monitor.\n",
              "Glance.CLI.exe status --no-start --json" },
            { "check-update", "Check for a newer release and show its download links.",
              "Glance.CLI.exe check-update [options]",
              "notes:\n"
              "  Checks only; downloading and installation remain separate actions.\n"
              "  Finding no update is success. Default command timeout: 60 seconds.\n",
              "Glance.CLI.exe check-update --json" },
            { "quit", "Exit Glance and wait for App and Core to stop.",
              "Glance.CLI.exe quit [options]",
              "notes:\n  Also succeeds when Glance is already stopped.\n",
              "Glance.CLI.exe quit" },
            { "help", "Show the overview or help for a specific command.",
              "Glance.CLI.exe help [COMMAND] [SUBCOMMAND]",
              "notes:\n  Help is local and can be read while Glance is stopped.\n",
              "Glance.CLI.exe help preview\n"
              "  Glance.CLI.exe window move --help" },
        };
        for (const auto& page : pages)
        {
            if (page.topic != topic) continue;
            std::string result = "usage: "; result += page.usage;
            result += "\n\n"; result += page.description;
            result += "\n\n"; result += page.details;
            result += "\ncommon options:\n"
                "  -h, --help              Show this help message and exit\n"
                "  --json                  Output one JSON result for scripts\n"
                "  --no-start              Require an already running Glance\n"
                "  --version               Show version and exit\n";
            if (topic == "preview" || topic == "window" || topic.starts_with("window "))
            {
                result += "  --timeout SECONDS       Return current state after 0 to 86400 seconds\n";
                if (topic != "window close") result += "  --wait                  Wait until the target window closes\n";
                result += "\n  Default: wait for readiness and return full window details.\n"
                    "  --timeout 0 returns immediately after applying the request.\n"
                    "  A timed wait returns current state with wait_completed: false.\n";
            }
            result += "\nexamples:\n  "; result += page.examples;
            if (topic.empty()) result += "\n\nRun 'Glance.CLI.exe COMMAND -h' for command-specific help.";
            result += "\n\nDocs: https://github.com/ElluIFX/Glance/blob/main/docs/CLI.en.md";
            return result;
        }
        throw Error(2, "unknown_command", "Unknown help topic: " + std::string(topic));
    }
}
