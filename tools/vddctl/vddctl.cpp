// vddctl: installs the uConnect virtual display driver and exercises its
// control interface. A development and acceptance tool; the uConnect app has
// its own copy of the install and control code.
//
//   vddctl install <path\to\UConnectVDD.inf>   (elevated)
//   vddctl uninstall                            (elevated)
//   vddctl version
//   vddctl list
//   vddctl add <width> <height> <hz> [seconds]  adds a monitor, keeps it for
//                                               `seconds` (default 10), removes it
//   vddctl selftest                             the acceptance checks
//
// Builds with MSVC or MinGW: g++ -std=c++17 -O2 -municode vddctl.cpp -I../../include
//   -lsetupapi -lnewdev -ldxgi -ld3d11 -lole32
//   -static-libgcc -static-libstdc++ -o vddctl.exe

#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <dxgi1_2.h>
#include <d3d11.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <uconnect-vdd.h>

namespace {

std::wstring lastError(DWORD code = GetLastError()) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out = text ? text : L"";
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r')) out.pop_back();
    return out + L" (" + std::to_wstring(code) + L")";
}

// ---- Installation ----------------------------------------------------------

// Devices whose hardware IDs include ours.
template <typename F>
void forEachOurDevice(F f) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<BYTE*>(ids), sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) continue;
        for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
            if (_wcsicmp(id, UCVDD_HARDWARE_ID) == 0) { f(set, dev); break; }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

int install(const wchar_t* infArg) {
    wchar_t inf[MAX_PATH];
    if (!GetFullPathNameW(infArg, MAX_PATH, inf, nullptr)) { fwprintf(stderr, L"Bad path: %ls\n", infArg); return 1; }

    bool exists = false;
    forEachOurDevice([&](HDEVINFO, SP_DEVINFO_DATA&) { exists = true; });
    if (!exists) {
        // A root-enumerated device for the driver to bind to.
        GUID classGuid; wchar_t className[MAX_CLASS_NAME_LEN];
        if (!SetupDiGetINFClassW(inf, &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) {
            fwprintf(stderr, L"Cannot read the INF class: %ls\n", lastError().c_str()); return 1;
        }
        HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
        SP_DEVINFO_DATA dev{sizeof(dev)};
        wchar_t hwid[64] = {};
        wcscpy(hwid, UCVDD_HARDWARE_ID);  // a multi-string: the second terminator is already there
        const DWORD hwidBytes = DWORD((wcslen(hwid) + 2) * sizeof(wchar_t));
        if (!SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &dev) ||
            !SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<BYTE*>(hwid), hwidBytes) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev)) {
            fwprintf(stderr, L"Cannot create the device: %ls\n", lastError().c_str());
            SetupDiDestroyDeviceInfoList(set);
            return 1;
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, UCVDD_HARDWARE_ID, inf, INSTALLFLAG_FORCE, &reboot)) {
        fwprintf(stderr, L"Cannot install the driver: %ls\n", lastError().c_str()); return 1;
    }
    wprintf(L"Installed%ls.\n", reboot ? L" (restart needed)" : L"");
    return 0;
}

int uninstall() {
    std::vector<std::wstring> infs;
    int removed = 0;
    forEachOurDevice([&](HDEVINFO set, SP_DEVINFO_DATA& dev) {
        wchar_t infPath[MAX_PATH] = {}; DEVPROPTYPE type;
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_DriverInfPath, &type,
                reinterpret_cast<BYTE*>(infPath), sizeof(infPath), nullptr, 0)) infs.push_back(infPath);
        // Not in every MinGW import library, so looked up at run time.
        using Uninstall = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
        static const auto uninstallDevice = reinterpret_cast<Uninstall>(
            reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"newdev.dll"), "DiUninstallDevice")));
        BOOL reboot = FALSE;
        if (uninstallDevice && uninstallDevice(nullptr, set, &dev, 0, &reboot)) removed++;
        else fwprintf(stderr, L"Cannot remove a device: %ls\n", lastError().c_str());
    });
    for (const auto& oem : infs) {
        if (!SetupUninstallOEMInfW(oem.c_str(), SUOI_FORCEDELETE, nullptr))
            fwprintf(stderr, L"Cannot remove %ls from the driver store: %ls\n", oem.c_str(), lastError().c_str());
    }
    wprintf(L"Removed %d device(s), %zu driver package(s).\n", removed, infs.size());
    return 0;
}

// ---- Control interface -----------------------------------------------------

std::wstring interfacePath() {
    std::wstring path;
    HDEVINFO set = SetupDiGetClassDevsW(&UCVDD_INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return path;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &UCVDD_INTERFACE_GUID, 0, &iface)) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

struct Driver {
    HANDLE handle = INVALID_HANDLE_VALUE;
    Driver() {
        const auto path = interfacePath();
        if (!path.empty())
            handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
    }
    ~Driver() { if (ok()) CloseHandle(handle); }
    Driver(const Driver&) = delete;
    bool ok() const { return handle != INVALID_HANDLE_VALUE; }
    bool ioctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize) const {
        DWORD returned = 0;
        return DeviceIoControl(handle, code, const_cast<void*>(in), inSize, out, outSize, &returned, nullptr);
    }
    bool version(UCVDD_VERSION& v) const { return ioctl(IOCTL_UCVDD_GET_VERSION, nullptr, 0, &v, sizeof(v)); }
    bool add(const UCVDD_ADD_PARAMS& p, UCVDD_ADD_RESULT& r) const { return ioctl(IOCTL_UCVDD_ADD, &p, sizeof(p), &r, sizeof(r)); }
    bool remove(const GUID& id) const { UCVDD_REMOVE_PARAMS p{id}; return ioctl(IOCTL_UCVDD_REMOVE, &p, sizeof(p), nullptr, 0); }
    bool ping() const { return ioctl(IOCTL_UCVDD_PING, nullptr, 0, nullptr, 0); }
};

UCVDD_ADD_PARAMS params(int width, int height, int hz, const GUID& id, const char* name = "vddctl") {
    UCVDD_ADD_PARAMS p{};
    p.MonitorId = id;
    p.Width = width; p.Height = height; p.RefreshMilliHz = hz * 1000;
    strncpy(p.Name, name, sizeof(p.Name) - 1);
    snprintf(p.Serial, sizeof(p.Serial), "%08lX", id.Data1);
    return p;
}

GUID newGuid() { GUID g; CoCreateGuid(&g); return g; }

// ---- Display configuration -------------------------------------------------

struct Target {
    LUID adapter{};
    UINT32 id = 0;
    std::wstring gdiName, monitorName;
    bool active = false;
    UINT32 width = 0, height = 0;
};

std::vector<Target> targets() {
    std::vector<Target> out;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& path = paths[i];
        Target t;
        t.adapter = path.targetInfo.adapterId;
        t.id = path.targetInfo.id;
        t.active = path.flags & DISPLAYCONFIG_PATH_ACTIVE;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) t.monitorName = target.monitorFriendlyDeviceName;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) t.gdiName = source.viewGdiDeviceName;
        const UINT32 sourceMode = path.sourceInfo.modeInfoIdx;
        if (sourceMode < modeCount && modes[sourceMode].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            t.width = modes[sourceMode].sourceMode.width;
            t.height = modes[sourceMode].sourceMode.height;
        }
        out.push_back(t);
    }
    return out;
}

bool sameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

const Target* findTarget(const std::vector<Target>& all, const UCVDD_ADD_RESULT& r) {
    for (const auto& t : all) if (sameLuid(t.adapter, r.AdapterLuid) && t.id == r.TargetId) return &t;
    return nullptr;
}

// Waits until `done` holds, up to `ms`; returns how long it took, or -1.
long long waitFor(int ms, const std::function<bool()>& done) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (done()) return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return -1;
        Sleep(50);
    }
}

bool setMode(const std::wstring& gdiName, int width, int height, int hz) {
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = width; mode.dmPelsHeight = height; mode.dmDisplayFrequency = hz;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsExW(gdiName.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL;
}

// Desktop duplication of the output named `gdiName`: the capture path the app uses.
bool duplicateOnce(const std::wstring& gdiName, std::wstring& detail) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) { detail = L"no DXGI factory"; return false; }
    bool ok = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !ok && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; !ok && adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (gdiName == desc.DeviceName) {
                ID3D11Device* device = nullptr;
                IDXGIOutput1* output1 = nullptr;
                IDXGIOutputDuplication* dup = nullptr;
                DXGI_ADAPTER_DESC1 adesc{}; adapter->GetDesc1(&adesc);
                HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
                if (SUCCEEDED(hr)) hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
                if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(device, &dup);
                if (SUCCEEDED(hr)) {
                    DXGI_OUTDUPL_FRAME_INFO info{}; IDXGIResource* resource = nullptr;
                    hr = dup->AcquireNextFrame(1000, &info, &resource);
                    if (SUCCEEDED(hr)) {
                        ok = true;
                        DXGI_OUTDUPL_DESC dd{}; dup->GetDesc(&dd);
                        detail = L"duplicated " + std::to_wstring(dd.ModeDesc.Width) + L"x" + std::to_wstring(dd.ModeDesc.Height)
                            + L" on " + adesc.Description + (info.PointerPosition.Visible ? L", pointer visible" : L"");
                        resource->Release();
                        dup->ReleaseFrame();
                    }
                }
                if (!ok) detail = L"duplication failed, HRESULT 0x" + std::to_wstring(unsigned(hr));
                if (dup) dup->Release();
                if (output1) output1->Release();
                if (device) device->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    if (!ok && detail.empty()) detail = L"no DXGI output named " + gdiName;
    return ok;
}

int list() {
    for (const auto& t : targets())
        wprintf(L"%-14ls %-16ls %ux%u  adapter %08lX:%08lX target %u\n", t.gdiName.c_str(), t.monitorName.c_str(),
            t.width, t.height, t.adapter.HighPart, t.adapter.LowPart, t.id);
    return 0;
}

int version() {
    Driver d;
    if (!d.ok()) { fwprintf(stderr, L"The driver is not installed or not running.\n"); return 1; }
    UCVDD_VERSION v{};
    if (!d.version(v)) { fwprintf(stderr, L"Version query failed: %ls\n", lastError().c_str()); return 1; }
    wprintf(L"Protocol %u.%u.%u, watchdog %u ms, up to %u monitors\n", v.Major, v.Minor, v.Patch, v.WatchdogMs, v.MaxMonitors);
    return 0;
}

int add(int width, int height, int hz, int seconds) {
    Driver d;
    if (!d.ok()) { fwprintf(stderr, L"The driver is not installed or not running.\n"); return 1; }
    const GUID id = newGuid();
    UCVDD_ADD_RESULT r{};
    if (!d.add(params(width, height, hz, id), r)) { fwprintf(stderr, L"Add failed: %ls\n", lastError().c_str()); return 1; }
    std::wstring gdi;
    waitFor(5000, [&] { auto all = targets(); auto* t = findTarget(all, r); if (t) gdi = t->gdiName; return t != nullptr; });
    wprintf(L"Added %ls (target %u); keeping it %d s.\n", gdi.empty() ? L"(not active)" : gdi.c_str(), r.TargetId, seconds);
    fflush(stdout);
    for (int i = 0; i < seconds; ++i) { Sleep(1000); d.ping(); }
    d.remove(id);
    return 0;
}

// ---- Self test --------------------------------------------------------------

int failures = 0;
void check(bool ok, const std::wstring& what, const std::wstring& detail = {}) {
    wprintf(L"[%ls] %ls%ls%ls\n", ok ? L"PASS" : L"FAIL", what.c_str(), detail.empty() ? L"" : L": ", detail.c_str());
    fflush(stdout);
    if (!ok) failures++;
}

// The child half of the crash test: add a monitor, report it, then wait to be killed.
int crashChild() {
    Driver d;
    UCVDD_ADD_RESULT r{};
    if (!d.ok() || !d.add(params(1280, 720, 60, newGuid(), "crash"), r)) return 1;
    wprintf(L"%08lX %08lX %u\n", r.AdapterLuid.HighPart, r.AdapterLuid.LowPart, r.TargetId);
    fflush(stdout);
    for (;;) { Sleep(500); d.ping(); }
}

int selftest(const wchar_t* self) {
    {
        Driver d;
        // No interface at all means no running driver: say so, not a stale error code.
        const DWORD openError = GetLastError();
        check(d.ok(), L"open the control interface", d.ok() ? L""
            : interfacePath().empty() ? L"no uConnect display device is running: is the driver installed (scripts\\dev-install.ps1)?"
            : lastError(openError));
        if (!d.ok()) return 1;
        UCVDD_VERSION v{};
        check(d.version(v) && v.Major == UCVDD_PROTOCOL_MAJOR, L"protocol version",
            std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor));

        // Validation.
        UCVDD_ADD_RESULT r{};
        check(!d.add(params(100, 100, 60, newGuid()), r), L"too small a mode is refused");
        check(!d.add(params(1920, 1080, 1000, newGuid()), r), L"too high a refresh rate is refused");

        // Add, find, set a mode, duplicate, remove.
        const GUID id = newGuid();
        const bool added = d.add(params(2560, 1440, 60, id, "selftest"), r);
        check(added, L"add 2560x1440@60", added ? L"" : lastError());
        if (added) {
            std::wstring gdi;
            const auto ms = waitFor(5000, [&] { auto all = targets(); auto* t = findTarget(all, r); if (t) gdi = t->gdiName; return t != nullptr; });
            check(ms >= 0, L"monitor becomes an active display", ms >= 0 ? gdi + L" after " + std::to_wstring(ms) + L" ms" : L"");
            if (ms >= 0) {
                check(setMode(gdi, 2560, 1440, 60), L"set its mode to 2560x1440@60");
                std::wstring detail;
                check(duplicateOnce(gdi, detail), L"desktop duplication captures it", detail);
            }

            UCVDD_ADD_RESULT again{};
            check(d.add(params(2560, 1440, 60, id), again) && again.TargetId == r.TargetId, L"adding the same id again returns the same monitor");
            {
                Driver other;
                UCVDD_ADD_RESULT r2{};
                const bool taken = !other.add(params(2560, 1440, 60, id), r2) && GetLastError() == ERROR_ALREADY_EXISTS;
                check(taken, L"another handle cannot take the same id");
                const bool denied = !other.remove(id) && GetLastError() == ERROR_ACCESS_DENIED;
                check(denied, L"another handle cannot remove it");
            }
            check(d.remove(id), L"remove it");
            const auto gone = waitFor(3000, [&] { auto all = targets(); return findTarget(all, r) == nullptr; });
            check(gone >= 0, L"it leaves the display configuration", std::to_wstring(gone) + L" ms");
        }

        // A handle that closes takes its monitors with it.
        {
            UCVDD_ADD_RESULT r3{};
            {
                Driver temp;
                check(temp.add(params(1920, 1080, 60, newGuid(), "close"), r3), L"add on a second handle");
                waitFor(5000, [&] { auto all = targets(); return findTarget(all, r3) != nullptr; });
            }
            const auto gone = waitFor(3000, [&] { auto all = targets(); return findTarget(all, r3) == nullptr; });
            check(gone >= 0, L"closing the handle removes its monitor", std::to_wstring(gone) + L" ms");
        }

        // A silent handle loses its monitors to the watchdog.
        {
            UCVDD_ADD_RESULT r4{};
            check(d.add(params(1920, 1080, 60, newGuid(), "silent"), r4), L"add, then stop pinging");
            waitFor(5000, [&] { auto all = targets(); return findTarget(all, r4) != nullptr; });
            const auto gone = waitFor(UCVDD_WATCHDOG_MS + 3000, [&] { auto all = targets(); return findTarget(all, r4) == nullptr; });
            check(gone >= 0, L"the watchdog removes a silent owner's monitor", std::to_wstring(gone) + L" ms");
        }
    }

    // A killed process loses its monitors at once.
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readPipe, writePipe;
        CreatePipe(&readPipe, &writePipe, &sa, 0);
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe; si.hStdError = GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" crash-child";
        const bool started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(writePipe);
        check(started, L"start a child that adds a monitor");
        if (started) {
            char line[128] = {}; DWORD got = 0;
            ReadFile(readPipe, line, sizeof(line) - 1, &got, nullptr);
            UCVDD_ADD_RESULT r5{};
            unsigned long high = 0, low = 0; unsigned target = 0;
            const bool parsed = sscanf(line, "%lx %lx %u", &high, &low, &target) == 3;
            r5.AdapterLuid.HighPart = LONG(high); r5.AdapterLuid.LowPart = low; r5.TargetId = target;
            const auto shown = parsed ? waitFor(5000, [&] { auto all = targets(); return findTarget(all, r5) != nullptr; }) : -1;
            check(shown >= 0, L"the child's monitor is active");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            const auto gone = waitFor(UCVDD_WATCHDOG_MS + 3000, [&] { auto all = targets(); return findTarget(all, r5) == nullptr; });
            check(gone >= 0 && gone < 1000, L"killing the child removes its monitor within 1 s", std::to_wstring(gone) + L" ms");
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        CloseHandle(readPipe);
    }

    wprintf(L"%ls\n", failures ? L"FAILED" : L"All checks passed.");
    return failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring command = argc > 1 ? argv[1] : L"";
    if (command == L"install" && argc > 2) return install(argv[2]);
    if (command == L"uninstall") return uninstall();
    if (command == L"version") return version();
    if (command == L"list") return list();
    if (command == L"add" && argc > 4) return add(_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[4]), argc > 5 ? _wtoi(argv[5]) : 10);
    if (command == L"selftest") return selftest(argv[0]);
    if (command == L"crash-child") return crashChild();
    fwprintf(stderr, L"usage: vddctl install <inf> | uninstall | version | list | add <w> <h> <hz> [seconds] | selftest\n");
    return 2;
}
