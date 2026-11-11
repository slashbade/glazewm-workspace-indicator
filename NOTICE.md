# Attribution

This project is distributed under GPL-3.0-or-later.

The TaskbarHost discovery, taskbar symbol declarations and UI-thread dispatch
bridge are adapted from these Windhawk mods:

- [Start button always on the left](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-start-button-position.wh.cpp), by m417z, published under GNU GPL v3.
- [Hide Start Button](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/hide-start-button.wh.cpp), by ptrkhh, itself based on the above mod.

The workspace display semantics follow [Zebar's starter widget](https://github.com/glzr-io/zebar/blob/main/resources/starter/with-glazewm.html) and [GlazeWM provider](https://github.com/glzr-io/zebar/blob/main/packages/client-api/src/providers/glazewm/create-glazewm-provider.ts). No Zebar code or assets are bundled.

[Taskbar Desktop Indicator](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-desktop-indicator.wh.cpp), by Simon Benedict, was reviewed as a reference; its clock / Windows virtual desktop implementation is not used.
