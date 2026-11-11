# GlazeWM Workspace Indicator

A Windhawk mod that adds GlazeWM workspace buttons immediately to the right of the Windows 11 Start button.

```text
Start   [ chat ━ ] [ web ] [ code ]   Search / Task View / Apps …
```

The indicator uses native Windows XAML controls inside the taskbar, with system theme colors, rounded buttons, tooltips, and keyboard focus support. It connects directly to GlazeWM; no Zebar, Node.js, or helper process is required.

**Status:** Experimental (0.1.0). Compilation and local GlazeWM IPC have been tested, but the mod has not yet been validated inside Explorer. See [Compatibility](#compatibility) before enabling it.

## Features

- Each taskbar shows the active workspaces on its monitor, in the order returned by GlazeWM.
- Labels use `displayName` when present, falling back to `name`. Workspace names can also be selected explicitly.
- An accent underline marks the focused workspace; a subtle background marks a displayed workspace. These states can differ across monitors.
- Clicking a button focuses its workspace using its UUID.
- The strip scrolls horizontally when its contents exceed the available width.
- Stale workspace buttons disappear when GlazeWM disconnects and return after reconnection.

Only active workspaces returned by GlazeWM are shown, including those currently hidden. Workspaces that exist only in the configuration are not listed. The mod does not change the GlazeWM configuration.

## Requirements

- Windows 11 x64 with the native horizontal taskbar and a visible Start button.
- Windhawk with its compiler installed.
- GlazeWM 3.x running locally.

## Installation

1. Open Windhawk and select **Create a new mod**.
2. Replace the editor contents with the complete [glazewm-workspace-indicator.wh.cpp](glazewm-workspace-indicator.wh.cpp) file.
3. Compile the mod and enable it. The first load may require Windows symbol downloads.
4. Open the mod settings to customize labels, dimensions, or the connection settings.

By default, the mod connects to `127.0.0.1:6123`. Changing settings reloads the mod. Disabling it removes the buttons and restores the Start button's original spacing.

## Configuration

Settings are available in Windhawk's mod settings panel.

| Setting | Default | Description |
| --- | --- | --- |
| GlazeWM IPC port | `6123` | Port used to connect to GlazeWM on localhost. |
| Refresh interval | `300` ms | Snapshot polling interval, clamped to 100–5000 ms. |
| Workspace label | Display name | Use `displayName` with a `name` fallback, or always use the workspace name / number. |
| Maximum strip width | `280` DIPs | Width limit before horizontal scrolling; also limited to 35% of the taskbar root width. |
| Button height | `32` DIPs | Workspace button height in device-independent pixels. |

## Compatibility

The mod relies on undocumented Windows taskbar symbols and layout details, which can change with Windows updates. Unsupported machine-code layouts are rejected and reported in the Windhawk log.

Windows 10, ExplorerPatcher's legacy taskbar, vertical taskbars, and layouts that hide Start are unsupported. Other mods that reposition Start or change its margins may conflict; disable them during initial testing.

Left and centered taskbar alignment, mixed-DPI monitors, auto-hide, and behavior across Windows updates still need manual UI validation. Compilation and IPC tests do not verify taskbar rendering.

## Development

The build script uses the compiler bundled with Windhawk, installed at `C:\Program Files\Windhawk` by default. Run these commands from the repository root in PowerShell:

```powershell
# Compile and link the mod; output is written to build/.
.\scripts\build.ps1

# Build and run parser, command validation, live IPC, and shutdown tests.
.\scripts\build.ps1 -Test

# Test stalled responses, stale-state cleanup, and receive cancellation.
# Requires Python 3 and the test executable built above.
python tests/stalled-peer.py
```

To use a different Windhawk installation, pass `-Windhawk 'D:\Windhawk'` to the build script.

The `-Test` run requires GlazeWM on the default port. It queries the running instance without sending workspace-switch commands. The stalled-peer test starts its own local mock server.

The DLL in `build/` is for compilation checks. Install through the Windhawk editor so Windhawk generates its managed lifecycle entry points.

### How it works

A background thread polls `query monitors` over a persistent local WebSocket. Receive operations have a two-second deadline; failed connections clear stale state and are retried. Focus commands are queued for the next polling cycle. The mod uses snapshot polling rather than event subscriptions.

The UI locates the XAML root through `TaskbarHost`, reserves space beside Start, and inserts the workspace strip into the taskbar's root grid. It leaves the Windows taskbar alignment setting unchanged.

### Manual UI checklist

| Scenario | Expected behavior |
| --- | --- |
| Left / centered taskbar | Buttons sit beside Start without overlapping Search or app icons. |
| Multiple monitors with different scaling | Each taskbar shows its monitor's workspaces at the correct scale. |
| Mouse / keyboard activation | The target workspace receives focus without opening Start. |
| Unicode, special characters, and long labels | Labels render correctly; long names are trimmed and available in tooltips. |
| Light, dark, and high-contrast themes | Text, backgrounds, and focus indicators remain readable. |
| Auto-hide, resolution changes, and monitor hot-plug | The strip follows the taskbar and recovers its position. |
| GlazeWM stop / restart | Stale information disappears and reconnects automatically. |
| Mod disable / re-enable | Original spacing is restored, with no leftover buttons or callbacks. |

## Contributing

Bug reports and pull requests are welcome. For taskbar issues, include your Windows build, Windhawk and GlazeWM versions, monitor scaling, taskbar alignment, other taskbar mods, and relevant Windhawk logs. Include build and test results with code changes, and describe any manual UI checks performed.

## License and attribution

Licensed under **GPL-3.0-or-later**. See [LICENSE](LICENSE) for the license text and [NOTICE.md](NOTICE.md) for upstream credits and references.
