# uConnect virtual display driver

The Windows half of uConnect's **extended display** feature: an indirect
display driver that gives Windows an extra monitor, which the uConnect app
captures and streams to another device. Windows sees an ordinary monitor;
only uConnect drives it.

It is a fork of [SudoVDA](https://github.com/SudoMaker/SudoVDA) (itself
built on Microsoft's indirect display sample). The fork:

- **Owns monitors by handle.** A monitor belongs to the handle that added it.
  Closing that handle, or the process ending in any way, removes the monitor
  at once; a handle that sends nothing for 3 s loses its monitors. A virtual
  screen never outlives the program using it.
- **Has its own identity** (`Root\uConnectVDD`, its own interface GUID and
  registry key), so it installs alongside SudoVDA or Apollo.
- **Restricts access** to SYSTEM, Administrators and interactive users.
- **Validates input:** modes within 640×480–7680×4320 and 24–240 Hz, EDID
  strings bounded, at most 4 monitors per handle, ids unique across handles.
- **Is SDR by default.** HDR modes are advertised only when the `hdr`
  registry value is set.
- **Builds from NuGet**: no installed WDK needed.

## Control interface

`include/uconnect-vdd.h` is the whole contract: plain C, the same layout
under MSVC and MinGW. Open the device interface `UCVDD_INTERFACE_GUID`, then:

| IOCTL | in / out | |
|---|---|---|
| `IOCTL_UCVDD_GET_VERSION` | – / `UCVDD_VERSION` | protocol version, watchdog, monitor limit |
| `IOCTL_UCVDD_ADD` | `UCVDD_ADD_PARAMS` / `UCVDD_ADD_RESULT` | add a monitor (id, width, height, refresh in mHz, EDID name and serial); returns the adapter LUID and target id to find it in `QueryDisplayConfig` |
| `IOCTL_UCVDD_REMOVE` | `UCVDD_REMOVE_PARAMS` / – | remove one of this handle's monitors |
| `IOCTL_UCVDD_PING` | – / – | keep this handle's monitors alive (send about once a second) |
| `IOCTL_UCVDD_SET_RENDER_ADAPTER` | `UCVDD_RENDER_ADAPTER_PARAMS` / – | the GPU that renders monitors added afterwards |

## Building

Requirements: Visual Studio 2022 or later (Build Tools are enough) with the
MSVC v143 x64 tools, and `nuget.exe`.

```powershell
nuget restore packages.config -PackagesDirectory packages
msbuild UConnectVDD.sln -p:Configuration=Release -p:Platform=x64
```

The output is `x64\Release\UConnectVDD\` (`UConnectVDD.inf`,
`UConnectVDD.dll`, `uconnectvdd.cat`), test-signed with a certificate the
WDK generates (`x64\Release\UConnectVDD.cer`).

Without the Spectre-mitigated MSVC libraries installed, add
`-p:Driver_SpectreMitigation=false`. A Visual Studio without the WDK
extension may not know the `WindowsUserModeDriver10.0` toolset; CI builds on
`windows-2025-vs2026`, which does.

`tools\vddctl` is a small test tool (MSVC or MinGW, see the top of
`vddctl.cpp`).

## Installing for development

From an elevated PowerShell, after building the driver and `vddctl`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev-install.ps1
tools\vddctl\vddctl.exe selftest
powershell -ExecutionPolicy Bypass -File scripts\dev-uninstall.ps1
```

`dev-install.ps1` trusts the build's test certificate on this machine, so no
test-signing mode is needed (this is a user-mode driver); `dev-uninstall.ps1`
removes the device, the driver package and the certificate.

`vddctl selftest` runs the acceptance checks: validation, adding a monitor
and finding it, setting its mode, capturing it with desktop duplication,
ownership between handles, and removal on close, on silence (watchdog) and
on a killed process.

## Registry settings

`HKLM\SOFTWARE\uConnect\VirtualDisplay`, read when the driver starts:

- `gpuName` (string): the GPU that renders monitors until a client picks one.
- `maxMonitors` (DWORD, 1–16, default 10).
- `hdr` (DWORD): 1 advertises HDR modes (Windows 11).
- `sdrBits` (DWORD): 10 for 10-bit SDR. `hdrBits` (DWORD): 12 for 12-bit HDR.

## License

See [LICENSE](LICENSE): Microsoft's sample code is MS-PL, SudoMaker's changes
MIT/CC0, uConnect's changes MIT.

## Acknowledgements

- [SudoVDA](https://github.com/SudoMaker/SudoVDA) by SudoMaker.
- **MikeTheTech** (Virtual Display Driver), **Roshkins** (IddSampleDriver),
  **Baloukj** (8/10-bit support), **Anakngtokwa**, **zjoasan** and **Bud**.
- **Microsoft**: the
  [indirect display driver sample](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay).
- **AKATrevorJay** for the [EDID generator](https://github.com/akatrevorjay/edid-generator).
