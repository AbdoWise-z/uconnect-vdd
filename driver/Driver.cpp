/*++

Copyright (c) Microsoft Corporation
Changes: SudoMaker (SudoVDA), uConnect (this fork)

Abstract:

	The uConnect virtual display driver: an indirect display driver whose
	monitors are added and removed at runtime by a client (the uConnect app),
	through the control interface in include/uconnect-vdd.h.

	Every monitor belongs to the handle that added it. Closing that handle,
	or the client going silent for longer than the watchdog allows, removes
	the monitor, so a virtual screen never outlives the program using it.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
#include "edid.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "AdapterOption.h"
#include <uconnect-vdd.h>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

LUID preferredAdapterLuid{};
bool preferredAdapterChanged = false;

// Monitor bookkeeping. Two locks:
//  - opLock serializes plugging and unplugging (IOCTLs, handle cleanup, the
//    watchdog). It is held across IddCx calls.
//  - listLock guards the containers below and is never held across an IddCx
//    call: IddCx calls back into the driver (mode queries look monitors up
//    in the list) while a monitor arrives or departs.
std::mutex opLock;
std::mutex listLock;
std::queue<UINT> freeConnectorSlots;
std::list<IndirectMonitorContext*> monitorCtxList;
std::map<WDFFILEOBJECT, ULONGLONG> ownerLastSeen;  // owners that hold monitors

bool hdrEnabled = false;
DWORD watchdogMs = UCVDD_WATCHDOG_MS;
std::atomic_bool watchdogRunning{false};
std::thread watchdogThread;

DWORD MaxVirtualMonitorCount = 10;
IDDCX_BITS_PER_COMPONENT SDRBITS = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRBITS = IDDCX_BITS_PER_COMPONENT_10;

#pragma region SampleMonitors

static const UINT mode_scale_factors[] = {
	// Put 100 at the first for convenience and fool proof
	100,
	50,
	75,
	125,
	150,
};

// Default modes reported for edid-less monitors. The second mode is set as preferred
static const struct VirtualMonitorMode s_DefaultModes[] = {
	{800, 600, 30000},
	{800, 600, 59940},
	{800, 600, 60000},
	{800, 600, 72000},
	{800, 600, 90000},
	{800, 600, 120000},
	{800, 600, 144000},
	{800, 600, 240000},
	{1280, 720, 30000},
	{1280, 720, 59940},
	{1280, 720, 60000},
	{1280, 720, 72000},
	{1280, 720, 90000},
	{1280, 720, 120000},
	{1280, 720, 144000},
	{1366, 768, 30000},
	{1366, 768, 59940},
	{1366, 768, 60000},
	{1366, 768, 72000},
	{1366, 768, 90000},
	{1366, 768, 120000},
	{1366, 768, 144000},
	{1366, 768, 240000},
	{1920, 1080, 30000},
	{1920, 1080, 59940},
	{1920, 1080, 60000},
	{1920, 1080, 72000},
	{1920, 1080, 90000},
	{1920, 1080, 120000},
	{1920, 1080, 144000},
	{1920, 1080, 240000},
	{2560, 1440, 30000},
	{2560, 1440, 59940},
	{2560, 1440, 60000},
	{2560, 1440, 72000},
	{2560, 1440, 90000},
	{2560, 1440, 120000},
	{2560, 1440, 144000},
	{2560, 1440, 240000},
	{3840, 2160, 30000},
	{3840, 2160, 59940},
	{3840, 2160, 60000},
	{3840, 2160, 72000},
	{3840, 2160, 90000},
	{3840, 2160, 120000},
	{3840, 2160, 144000},
	{3840, 2160, 240000},
};

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
	Mode.totalSize.cx = Mode.activeSize.cx = Width;
	Mode.totalSize.cy = Mode.activeSize.cy = Height;

	// See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
	Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;

	DWORD Denominator = 1000;

	if (hdrEnabled) {
		if (VSync < 1000) {
			VSync *= 1000;
		}
	} else {
		if (VSync % 1000 > 500) {
			VSync = (VSync / 1000) + 1;
		} else {
			VSync /= 1000;
		}
		Denominator = 1;
	}

	Mode.vSyncFreq.Numerator = VSync;
	Mode.vSyncFreq.Denominator = Denominator;
	Mode.hSyncFreq.Numerator = VSync * Height;
	Mode.hSyncFreq.Denominator = Denominator;

	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

	Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height) / Denominator;
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_BITS_PER_COMPONENT ModeBits()
{
	return hdrEnabled ? (IDDCX_BITS_PER_COMPONENT)(SDRBITS | HDRBITS) : SDRBITS;
}

static IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	Mode.BitsPerComponent.Rgb = ModeBits();
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

static IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.BitsPerComponent.Rgb = ModeBits();
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

// The preferred mode of the monitor whose EDID this is, if it is one of ours.
static bool PreferredModeForEdid(const BYTE* edid, VirtualMonitorMode& mode)
{
	std::lock_guard<std::mutex> lg(listLock);
	for (auto* ctx : monitorCtxList) {
		if (ctx->pEdidData && memcmp(edid, ctx->pEdidData, sizeof(edid_base)) == 0) {
			mode = ctx->preferredMode;
			return mode.Width != 0;
		}
	}
	return false;
}

// Copies a client string into an EDID text field: at most 13 printable
// ASCII characters, always terminated.
static void CopyEdidText(char (&out)[EDID_STRING_FIELD_SIZE + 1], const char* in, size_t size)
{
	memset(out, 0, sizeof(out));
	for (size_t i = 0; i < size && i < EDID_STRING_FIELD_SIZE && in[i]; i++) {
		out[i] = (in[i] >= 0x20 && in[i] < 0x7f) ? in[i] : '?';
	}
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD UcVddDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD UcVddDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY UcVddDeviceD0Entry;
EVT_WDF_FILE_CLEANUP UcVddFileCleanup;

EVT_IDD_CX_ADAPTER_INIT_FINISHED UcVddAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES UcVddAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION UcVddParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES UcVddMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES UcVddMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN UcVddMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN UcVddMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO UcVddAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA UcVddMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 UcVddParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 UcVddMonitorQueryModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 UcVddAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP UcVddMonitorSetGammaRamp;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

struct IndirectMonitorContextWrapper
{
	IndirectMonitorContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}

static bool ReadDword(HKEY hKey, const wchar_t* name, DWORD& value)
{
	DWORD size = sizeof(DWORD);
	return RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)&value, &size) == ERROR_SUCCESS;
}

void LoadSettings() {
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\uConnect\\VirtualDisplay", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
		return;
	}

	// The GPU that renders the monitors until a client chooses one.
	wchar_t gpuName[128] = {};
	DWORD bufferSize = sizeof(gpuName) - sizeof(wchar_t);
	if (RegQueryValueExW(hKey, L"gpuName", NULL, NULL, (LPBYTE)gpuName, &bufferSize) == ERROR_SUCCESS) {
		AdapterOption adapterOpt = AdapterOption();
		adapterOpt.selectGPU(gpuName);

		preferredAdapterLuid = adapterOpt.adapterLuid;
		preferredAdapterChanged = adapterOpt.hasTargetAdapter;
	}

	DWORD value = 0;
	if (ReadDword(hKey, L"maxMonitors", value) && value > 0 && value <= 16) {
		MaxVirtualMonitorCount = value;
	}
	if (ReadDword(hKey, L"hdr", value)) {
		hdrEnabled = value != 0;
	}
	if (ReadDword(hKey, L"sdrBits", value) && value == 10) {
		SDRBITS = IDDCX_BITS_PER_COMPONENT_10;
	}
	if (ReadDword(hKey, L"hdrBits", value) && value == 12) {
		HDRBITS = IDDCX_BITS_PER_COMPONENT_12;
	}

	RegCloseKey(hKey);
}

// Unplugs monitors already taken out of the list. Called with opLock held.
static void DepartMonitors(const std::vector<IDDCX_MONITOR>& monitors)
{
	for (auto monitor : monitors) {
		// The monitor context is deleted by WDF once the monitor object goes.
		// A failure means the monitor is already gone (the device going away).
		if (!NT_SUCCESS(IddCxMonitorDeparture(monitor))) {
			continue;
		}
	}
}

// Takes every monitor `owner` holds out of the list. With a null owner, takes
// every monitor. Called with opLock held.
static std::vector<IDDCX_MONITOR> TakeMonitors(WDFFILEOBJECT owner, bool all)
{
	std::vector<IDDCX_MONITOR> taken;
	std::lock_guard<std::mutex> lg(listLock);
	for (auto it = monitorCtxList.begin(); it != monitorCtxList.end();) {
		auto* ctx = *it;
		if (all || ctx->owner == owner) {
			freeConnectorSlots.push(ctx->connectorId);
			taken.push_back(ctx->GetMonitor());
			it = monitorCtxList.erase(it);
		} else {
			++it;
		}
	}
	if (all) {
		ownerLastSeen.clear();
	} else {
		ownerLastSeen.erase(owner);
	}
	return taken;
}

// Owners that went silent lose their monitors.
static void RunWatchdog()
{
	watchdogRunning = true;
	watchdogThread = std::thread([] {
		while (watchdogRunning) {
			Sleep(250);
			const ULONGLONG now = GetTickCount64();
			std::vector<WDFFILEOBJECT> silent;
			{
				std::lock_guard<std::mutex> lg(listLock);
				for (const auto& [owner, seen] : ownerLastSeen) {
					if (now - seen > watchdogMs) {
						silent.push_back(owner);
					}
				}
			}
			if (silent.empty()) {
				continue;
			}
			std::lock_guard<std::mutex> op(opLock);
			for (auto owner : silent) {
				DepartMonitors(TakeMonitors(owner, false));
			}
		}
	});
}

static void StopWatchdog()
{
	if (watchdogRunning.exchange(false) && watchdogThread.joinable()) {
		watchdogThread.join();
	}
}

void SetHighPriority() {
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
{
	LoadSettings();

	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config,
		UcVddDeviceAdd
	);

	Config.EvtDriverUnload = UcVddDriverUnload;

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	RunWatchdog();

	SetHighPriority();

	return Status;
}

_Use_decl_annotations_
void UcVddDriverUnload(_In_ WDFDRIVER) {
	StopWatchdog();
	std::lock_guard<std::mutex> op(opLock);
	DepartMonitors(TakeMonitors(nullptr, true));
}

// The last handle to a file object closed: its monitors go with it. This is
// what makes a crashed or killed client lose its screens at once.
_Use_decl_annotations_
VOID UcVddFileCleanup(WDFFILEOBJECT FileObject)
{
	std::lock_guard<std::mutex> op(opLock);
	DepartMonitors(TakeMonitors(FileObject, false));
}

EVT_IDD_CX_DEVICE_IO_CONTROL UcVddIoDeviceControl;

_Use_decl_annotations_
NTSTATUS UcVddDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

	UNREFERENCED_PARAMETER(Driver);

	// Register for power callbacks - in this sample only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = UcVddDeviceD0Entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	// Monitors belong to the handle that added them.
	WDF_FILEOBJECT_CONFIG FileConfig;
	WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, UcVddFileCleanup);
	WdfDeviceInitSetFileObjectConfig(pDeviceInit, &FileConfig, WDF_NO_OBJECT_ATTRIBUTES);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue.
	IddConfig.EvtIddCxDeviceIoControl = UcVddIoDeviceControl;

	IddConfig.EvtIddCxAdapterInitFinished = UcVddAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = UcVddMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = UcVddMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = UcVddMonitorUnassignSwapChain;

	if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		IddConfig.EvtIddCxAdapterQueryTargetInfo = UcVddAdapterQueryTargetInfo;
		IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = UcVddMonitorSetDefaultHdrMetadata;
		IddConfig.EvtIddCxParseMonitorDescription2 = UcVddParseMonitorDescription2;
		IddConfig.EvtIddCxMonitorQueryTargetModes2 = UcVddMonitorQueryModes2;
		IddConfig.EvtIddCxAdapterCommitModes2 = UcVddAdapterCommitModes2;
		IddConfig.EvtIddCxMonitorSetGammaRamp = UcVddMonitorSetGammaRamp;
	} else {
		// Windows 10: no HDR callbacks, so no HDR.
		hdrEnabled = false;
		IddConfig.EvtIddCxParseMonitorDescription = UcVddParseMonitorDescription;
		IddConfig.EvtIddCxMonitorQueryTargetModes = UcVddMonitorQueryModes;
		IddConfig.EvtIddCxAdapterCommitModes = UcVddAdapterCommitModes;
	}

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		// Automatically cleanup the context when the WDF object is about to be deleted
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext)
		{
			pContext->Cleanup();
		}
	};

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	Status = WdfDeviceCreateDeviceInterface(
		Device,
		&UCVDD_INTERFACE_GUID,
		NULL
	);

	if (!NT_SUCCESS(Status)) {
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);

	// Create a new device context object and attach it to the WDF device object
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);

	return Status;
}

_Use_decl_annotations_
NTSTATUS UcVddDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	UNREFERENCED_PARAMETER(PreviousState);

	// This function is called by WDF to start the device in the fully-on power state.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();

	return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
	AdapterLuid = {};
}

HRESULT Direct3DDevice::Init()
{
	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		return hr;
	}

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		return hr;
	}

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		return hr;
	}

	return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
	// Alert the swap-chain processing thread to terminate
	SetEvent(m_hTerminateEvent.Get());

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate
		WaitForSingleObject(m_hThread.Get(), INFINITE);
	}
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &AvTask);

	RunCore();

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	WdfObjectDelete((WDFOBJECT)m_hSwapChain);
	m_hSwapChain = nullptr;

	if (AvTaskHandle)
	{
		AvRevertMmThreadCharacteristics(AvTaskHandle);
	}
}

void SwapChainProcessor::RunCore()
{
	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		return;
	}

	// Acquire and release buffers in a loop. The frames themselves are not
	// used here: the client captures the monitor through the OS (desktop
	// duplication), like any other screen.
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		IDXGIResource* pSurface;

		if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
			IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
			BufferInArgs.Size = sizeof(BufferInArgs);
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}

		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles [] =
			{
				m_hAvailableBufferEvent,
				m_hTerminateEvent.Get()
			};
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
			if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			// We have new frame to process, the surface has a reference on it that the driver has to release
			AcquiredBuffer.Attach(pSurface);
			AcquiredBuffer.Reset();

			// Indicate to OS that we have finished inital processing of the frame, it is a hint that
			// OS could start preparing another frame
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}
		}
		else
		{
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice)
{
	m_Adapter = {};
	std::lock_guard<std::mutex> lg(listLock);
	freeConnectorSlots = {};
	for (UINT i = 0; i < MaxVirtualMonitorCount; i++) {
		freeConnectorSlots.push(i);
	}
}

IndirectDeviceContext::~IndirectDeviceContext()
{
	// The device is going away and WDF deletes its monitors with it.
	std::lock_guard<std::mutex> lg(listLock);
	monitorCtxList.clear();
	ownerLastSeen.clear();
}

void IndirectDeviceContext::InitAdapter()
{
	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	if (hdrEnabled && IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
	}

	// Declare basic feature support for the adapter (required)
	AdapterCaps.MaxMonitorsSupported = MaxVirtualMonitorCount;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"uConnect Virtual Display Adapter";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"uConnect";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"uConnect VDD";

	// Declare your hardware and firmware versions (required)
	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = UCVDD_PROTOCOL_MAJOR;
	Version.MinorVer = UCVDD_PROTOCOL_MINOR;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	if (NT_SUCCESS(Status))
	{
		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		pContext->pContext = this;
	}
}

void IndirectDeviceContext::SetRenderAdapter(const LUID& AdapterLuid) {
	IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{AdapterLuid};
	IddCxAdapterSetRenderAdapter(m_Adapter, &inArgs);
}

NTSTATUS IndirectDeviceContext::CreateMonitor(IndirectMonitorContext*& pMonitorContext, UINT connector, uint8_t* edidData,
	const GUID& containerId, const VirtualMonitorMode& preferredMode, WDFFILEOBJECT owner) {
	pMonitorContext = nullptr;

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = connector;

	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	MonitorInfo.MonitorDescription.DataSize = sizeof(edid_base);
	MonitorInfo.MonitorDescription.pData = edidData;
	MonitorInfo.MonitorContainerId = containerId;

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	// Create a monitor object with the specified monitor descriptor
	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (!NT_SUCCESS(Status))
	{
		free(edidData);
		return Status;
	}

	// Create a new monitor context object and attach it to the Idd monitor object
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
	auto* ctx = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);
	pMonitorContextWrapper->pContext = ctx;

	ctx->monitorGuid = containerId;
	ctx->connectorId = connector;
	ctx->pEdidData = edidData;
	ctx->preferredMode = preferredMode;
	ctx->m_Adapter = m_Adapter;
	ctx->owner = owner;

	// In the list before arrival: the OS asks for its modes while it arrives.
	{
		std::lock_guard<std::mutex> lg(listLock);
		monitorCtxList.push_back(ctx);
	}

	// Tell the OS that the monitor has been plugged in
	IDARG_OUT_MONITORARRIVAL ArrivalOut;
	Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
	if (!NT_SUCCESS(Status)) {
		{
			std::lock_guard<std::mutex> lg(listLock);
			monitorCtxList.remove(ctx);
		}
		WdfObjectDelete((WDFOBJECT)MonitorCreateOut.MonitorObject);
		return Status;
	}

	ctx->adapterLuid = ArrivalOut.OsAdapterLuid;
	ctx->targetId = ArrivalOut.OsTargetId;
	pMonitorContext = ctx;
	return Status;
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
	m_Monitor(Monitor)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
	m_ProcessingThread.reset();
	{
		// Normally taken out of the list before departing; this covers WDF
		// deleting the monitor on its own (the device going away).
		std::lock_guard<std::mutex> lg(listLock);
		monitorCtxList.remove(this);
	}
	if (pEdidData && pEdidData != edid_base) {
		free(pEdidData);
	}
}

IDDCX_MONITOR IndirectMonitorContext::GetMonitor() const {
	return m_Monitor;
}

void IndirectMonitorContext::AssignSwapChain(const IDDCX_MONITOR& MonitorObject, const IDDCX_SWAPCHAIN& SwapChain, const LUID& RenderAdapter, const HANDLE& NewFrameEvent)
{
	m_ProcessingThread.reset();

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init()))
	{
		// It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
		// swap-chain and try again.
		WdfObjectDelete(SwapChain);
		return;
	}

	// Create a new swap-chain processing thread
	m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));

	// A hardware cursor keeps the pointer out of the frames; clients get it
	// separately (desktop duplication reports it on its own).
	if (!m_hCursorEvent.IsValid())
	{
		m_hCursorEvent.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	}
	if (!m_hCursorEvent.IsValid())
	{
		return;
	}

	IDDCX_CURSOR_CAPS cursorInfo = {};
	cursorInfo.Size = sizeof(cursorInfo);
	cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
	cursorInfo.AlphaCursorSupport = true;
	// Larger cursors (accessibility sizes) fall back to a software cursor.
	cursorInfo.MaxX = 256;
	cursorInfo.MaxY = 256;

	IDARG_IN_SETUP_HWCURSOR hwCursor = {};
	hwCursor.CursorInfo = cursorInfo;
	hwCursor.hNewCursorDataAvailable = m_hCursorEvent.Get();

	if (!NT_SUCCESS(IddCxMonitorSetupHardwareCursor(MonitorObject, &hwCursor))) {
		// Without it the OS draws the cursor into the frames, which still works.
		return;
	}
}

void IndirectMonitorContext::UnassignSwapChain()
{
	// Stop processing the last swap-chain
	m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS UcVddAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	if (NT_SUCCESS(pInArgs->AdapterInitStatus)) {
		if (preferredAdapterChanged) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(AdapterObject, &inArgs);
			preferredAdapterChanged = false;
		}
	}

	return pInArgs->AdapterInitStatus;
}

_Use_decl_annotations_
NTSTATUS UcVddAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	// Nothing to configure: the swap-chain is taken care of by IddCx.
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

// Monitor modes for one of our EDIDs: the defaults, plus the preferred mode
// at several scales and at double refresh. Shared by both versions of the
// callback, which differ only in their mode structure.
template <typename ModeT, typename MakeMode>
static NTSTATUS ParseMonitorModes(const IDDCX_MONITOR_DESCRIPTION& description, UINT inputCount, ModeT* pModes,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs, MakeMode makeMode)
{
	if (description.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	VirtualMonitorMode preferred{};
	const bool havePreferred = PreferredModeForEdid((const BYTE*)description.pData, preferred);

	pOutArgs->MonitorModeBufferOutputCount = (UINT)std::size(s_DefaultModes);
	if (havePreferred) {
		pOutArgs->MonitorModeBufferOutputCount += (UINT)std::size(mode_scale_factors) * 2;
	}

	if (inputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (inputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}

	float vsyncMultiplier = 1;
	if (havePreferred && preferred.VSync) {
		float fVSync = (float)preferred.VSync / 1000;
		vsyncMultiplier = fVSync / roundf(fVSync);
		if (vsyncMultiplier > 1) {
			vsyncMultiplier = 1;
		}
	}

	for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
		auto vsyncTarget = s_DefaultModes[ModeIndex].VSync;
		if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
			vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
		}
		pModes[ModeIndex] = makeMode(s_DefaultModes[ModeIndex].Width, s_DefaultModes[ModeIndex].Height, vsyncTarget);
	}

	if (havePreferred) {
		for (UINT idx = 0; idx < std::size(mode_scale_factors); idx++) {
			auto factor = mode_scale_factors[idx];
			auto width = preferred.Width * factor / 100;
			auto height = preferred.Height * factor / 100;
			pModes[std::size(s_DefaultModes) + idx * 2] = makeMode(width, height, preferred.VSync);
			pModes[std::size(s_DefaultModes) + idx * 2 + 1] = makeMode(width, height, preferred.VSync * 2);
		}
		pOutArgs->PreferredMonitorModeIdx = (UINT)std::size(s_DefaultModes);
	} else {
		pOutArgs->PreferredMonitorModeIdx = 1;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
	return ParseMonitorModes(pInArgs->MonitorDescription, pInArgs->MonitorModeBufferInputCount, pInArgs->pMonitorModes, pOutArgs,
		[](DWORD w, DWORD h, DWORD v) { return CreateIddCxMonitorMode(w, h, v, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR); });
}

_Use_decl_annotations_
NTSTATUS UcVddParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	return ParseMonitorModes(pInArgs->MonitorDescription, pInArgs->MonitorModeBufferInputCount, pInArgs->pMonitorModes, pOutArgs,
		[](DWORD w, DWORD h, DWORD v) { return CreateIddCxMonitorMode2(w, h, v, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR); });
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);

	pOutArgs->DefaultMonitorModeBufferOutputCount = (UINT)std::size(s_DefaultModes);
	pOutArgs->PreferredMonitorModeIdx = 1;

	if (pInArgs->DefaultMonitorModeBufferInputCount == 0) {
		return STATUS_SUCCESS;
	} else if (pInArgs->DefaultMonitorModeBufferInputCount < pOutArgs->DefaultMonitorModeBufferOutputCount) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++) {
		pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
			s_DefaultModes[ModeIndex].Width,
			s_DefaultModes[ModeIndex].Height,
			s_DefaultModes[ModeIndex].VSync,
			IDDCX_MONITOR_MODE_ORIGIN_DRIVER
		);
	}

	return STATUS_SUCCESS;
}

// Target modes: the processing capability for a monitor, intersected by the
// OS with its monitor modes. Shared by both callback versions.
template <typename ModeT, typename MakeMode>
static NTSTATUS QueryTargetModes(IDDCX_MONITOR MonitorObject, UINT inputCount, ModeT* pTargetModes,
	IDARG_OUT_QUERYTARGETMODES* pOutArgs, MakeMode makeMode)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	const VirtualMonitorMode preferred = pMonitorContextWrapper->pContext
		? pMonitorContextWrapper->pContext->preferredMode : VirtualMonitorMode{};

	pOutArgs->TargetModeBufferOutputCount = (UINT)std::size(s_DefaultModes);
	if (preferred.Width) {
		pOutArgs->TargetModeBufferOutputCount += (UINT)std::size(mode_scale_factors) * 2;
	}

	if (inputCount < pOutArgs->TargetModeBufferOutputCount) {
		return inputCount != 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}

	float vsyncMultiplier = 1;
	if (preferred.VSync) {
		float fVSync = (float)preferred.VSync / 1000;
		vsyncMultiplier = fVSync / roundf(fVSync);
		if (vsyncMultiplier > 1) {
			vsyncMultiplier = 1;
		}
	}

	UINT index = 0;
	for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
		auto vsyncTarget = s_DefaultModes[i].VSync;
		if (vsyncMultiplier != 1 && !(vsyncTarget % 1000)) {
			vsyncTarget = (DWORD)(vsyncTarget * vsyncMultiplier);
		}
		pTargetModes[index++] = makeMode(s_DefaultModes[i].Width, s_DefaultModes[i].Height, vsyncTarget);
	}

	if (preferred.Width) {
		for (UINT idx = 0; idx < std::size(mode_scale_factors); idx++) {
			auto factor = mode_scale_factors[idx];
			auto width = preferred.Width * factor / 100;
			auto height = preferred.Height * factor / 100;
			pTargetModes[index++] = makeMode(width, height, preferred.VSync);
			pTargetModes[index++] = makeMode(width, height, preferred.VSync * 2);
		}
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	return QueryTargetModes(MonitorObject, pInArgs->TargetModeBufferInputCount, pInArgs->pTargetModes, pOutArgs,
		[](DWORD w, DWORD h, DWORD v) { return CreateIddCxTargetMode(w, h, v); });
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorQueryModes2(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	return QueryTargetModes(MonitorObject, pInArgs->TargetModeBufferInputCount, pInArgs->pTargetModes, pOutArgs,
		[](DWORD w, DWORD h, DWORD v) { return CreateIddCxTargetMode2(w, h, v); });
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);

	if (preferredAdapterChanged) {
		if (memcmp(&pInArgs->RenderAdapterLuid, &preferredAdapterLuid, sizeof(LUID))) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(pMonitorContextWrapper->pContext->m_Adapter, &inArgs);
			return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
		}
		preferredAdapterChanged = false;
	}

	pMonitorContextWrapper->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	pMonitorContextWrapper->pContext->UnassignSwapChain();
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);
	if (hdrEnabled) {
		pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;
		pOutArgs->DitheringSupport.Rgb = HDRBITS;
	} else {
		pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_NONE;
		pOutArgs->DitheringSupport.Rgb = SDRBITS;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS UcVddMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

#pragma endregion

#pragma region Control interface

static bool ValidMode(const UCVDD_ADD_PARAMS& params)
{
	return params.Width >= UCVDD_MIN_WIDTH && params.Width <= UCVDD_MAX_WIDTH
		&& params.Height >= UCVDD_MIN_HEIGHT && params.Height <= UCVDD_MAX_HEIGHT
		&& params.RefreshMilliHz >= UCVDD_MIN_REFRESH_MHZ && params.RefreshMilliHz <= UCVDD_MAX_REFRESH_MHZ;
}

static NTSTATUS AddMonitor(WDFDEVICE Device, WDFFILEOBJECT owner, const UCVDD_ADD_PARAMS& params, UCVDD_ADD_RESULT& result)
{
	if (!ValidMode(params)) {
		return STATUS_INVALID_PARAMETER;
	}

	UINT connector = 0;
	{
		std::lock_guard<std::mutex> lg(listLock);
		UINT owned = 0;
		for (auto* ctx : monitorCtxList) {
			if (ctx->monitorGuid == params.MonitorId) {
				if (ctx->owner != owner) {
					return STATUS_OBJECT_NAME_COLLISION;
				}
				// Already added by this handle: the same monitor again.
				result.AdapterLuid = ctx->adapterLuid;
				result.TargetId = ctx->targetId;
				return STATUS_SUCCESS;
			}
			if (ctx->owner == owner) {
				owned++;
			}
		}
		if (owned >= UCVDD_MAX_MONITORS_PER_HANDLE || freeConnectorSlots.empty()) {
			return STATUS_TOO_MANY_NODES;
		}
		connector = freeConnectorSlots.front();
		freeConnectorSlots.pop();
		ownerLastSeen[owner] = GetTickCount64();
	}

	char name[EDID_STRING_FIELD_SIZE + 1];
	char serial[EDID_STRING_FIELD_SIZE + 1];
	CopyEdidText(name, params.Name, sizeof(params.Name));
	CopyEdidText(serial, params.Serial, sizeof(params.Serial));

	uint8_t* edidData = generate_edid(params.MonitorId.Data1, serial, name);
	if (!edidData || edidData == edid_base) {
		std::lock_guard<std::mutex> lg(listLock);
		freeConnectorSlots.push(connector);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	IndirectMonitorContext* pMonitorContext = nullptr;
	const VirtualMonitorMode preferredMode = {params.Width, params.Height, params.RefreshMilliHz};
	NTSTATUS Status = pDeviceContextWrapper->pContext->CreateMonitor(pMonitorContext, connector, edidData,
		params.MonitorId, preferredMode, owner);
	if (!NT_SUCCESS(Status)) {
		std::lock_guard<std::mutex> lg(listLock);
		freeConnectorSlots.push(connector);
		return Status;
	}

	result.AdapterLuid = pMonitorContext->adapterLuid;
	result.TargetId = pMonitorContext->targetId;
	return STATUS_SUCCESS;
}

static NTSTATUS RemoveMonitor(WDFFILEOBJECT owner, const GUID& id)
{
	IDDCX_MONITOR monitor = nullptr;
	{
		std::lock_guard<std::mutex> lg(listLock);
		auto it = std::find_if(monitorCtxList.begin(), monitorCtxList.end(),
			[&](IndirectMonitorContext* ctx) { return ctx->monitorGuid == id; });
		if (it == monitorCtxList.end()) {
			return STATUS_NOT_FOUND;
		}
		if ((*it)->owner != owner) {
			return STATUS_ACCESS_DENIED;
		}
		monitor = (*it)->GetMonitor();
		freeConnectorSlots.push((*it)->connectorId);
		monitorCtxList.erase(it);
		const bool ownsMore = std::any_of(monitorCtxList.begin(), monitorCtxList.end(),
			[&](IndirectMonitorContext* ctx) { return ctx->owner == owner; });
		if (!ownsMore) {
			ownerLastSeen.erase(owner);
		}
	}
	DepartMonitors({monitor});
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID UcVddIoDeviceControl(
	WDFDEVICE Device,
	WDFREQUEST Request,
	size_t OutputBufferLength,
	size_t InputBufferLength,
	ULONG IoControlCode
)
{
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;
	WDFFILEOBJECT owner = WdfRequestGetFileObject(Request);

	// Any request from an owner keeps its monitors alive.
	if (owner) {
		std::lock_guard<std::mutex> lg(listLock);
		auto it = ownerLastSeen.find(owner);
		if (it != ownerLastSeen.end()) {
			it->second = GetTickCount64();
		}
	}

	switch (IoControlCode) {
	case IOCTL_UCVDD_GET_VERSION: {
		UCVDD_VERSION* output = nullptr;
		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(UCVDD_VERSION), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}
		output->Major = UCVDD_PROTOCOL_MAJOR;
		output->Minor = UCVDD_PROTOCOL_MINOR;
		output->Patch = 0;
		output->WatchdogMs = watchdogMs;
		output->MaxMonitors = MaxVirtualMonitorCount;
		bytesReturned = sizeof(UCVDD_VERSION);
		break;
	}
	case IOCTL_UCVDD_ADD: {
		if (!owner) {
			// A monitor needs an owner whose handle can close.
			Status = STATUS_INVALID_DEVICE_REQUEST;
			break;
		}
		UCVDD_ADD_PARAMS* params = nullptr;
		UCVDD_ADD_RESULT* output = nullptr;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(UCVDD_ADD_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}
		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(UCVDD_ADD_RESULT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}
		const UCVDD_ADD_PARAMS copy = *params;  // input and output share one system buffer
		UCVDD_ADD_RESULT result{};
		{
			std::lock_guard<std::mutex> op(opLock);
			Status = AddMonitor(Device, owner, copy, result);
		}
		if (NT_SUCCESS(Status)) {
			*output = result;
			bytesReturned = sizeof(UCVDD_ADD_RESULT);
		}
		break;
	}
	case IOCTL_UCVDD_REMOVE: {
		UCVDD_REMOVE_PARAMS* params = nullptr;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(UCVDD_REMOVE_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}
		std::lock_guard<std::mutex> op(opLock);
		Status = RemoveMonitor(owner, params->MonitorId);
		break;
	}
	case IOCTL_UCVDD_PING:
		Status = STATUS_SUCCESS;
		break;
	case IOCTL_UCVDD_SET_RENDER_ADAPTER: {
		UCVDD_RENDER_ADAPTER_PARAMS* params = nullptr;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(UCVDD_RENDER_ADAPTER_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);

		preferredAdapterLuid = params->AdapterLuid;
		pDeviceContextWrapper->pContext->SetRenderAdapter(params->AdapterLuid);
		preferredAdapterChanged = true;
		break;
	}
	default:
		break;
	}

	WdfRequestCompleteWithInformation(Request, Status, bytesReturned);
}

#pragma endregion
