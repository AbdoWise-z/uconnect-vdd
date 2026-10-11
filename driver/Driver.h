#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <mutex>
#include <queue>

#include "Trace.h"

namespace Microsoft
{
	namespace WRL
	{
		namespace Wrappers
		{
			// Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
			typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
		}
	}
}

namespace Microsoft
{
	namespace IndirectDisp
	{
		struct VirtualMonitorMode {
			DWORD Width;
			DWORD Height;
			DWORD VSync;  // millihertz
		};

		/// <summary>
		/// Manages the creation and lifetime of a Direct3D render device.
		/// </summary>
		struct Direct3DDevice
		{
			Direct3DDevice(LUID AdapterLuid);
			Direct3DDevice();
			HRESULT Init();

			LUID AdapterLuid;
			Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
			Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
			Microsoft::WRL::ComPtr<ID3D11Device> Device;
			Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
		};

		/// <summary>
		/// Manages a thread that consumes buffers from an indirect display swap-chain object.
		/// </summary>
		class SwapChainProcessor
		{
		public:
			SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
			~SwapChainProcessor();

		private:
			static DWORD CALLBACK RunThread(LPVOID Argument);

			void Run();
			void RunCore();

			IDDCX_SWAPCHAIN m_hSwapChain;
			std::shared_ptr<Direct3DDevice> m_Device;
			HANDLE m_hAvailableBufferEvent;
			Microsoft::WRL::Wrappers::Thread m_hThread;
			Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
		};

		class IndirectMonitorContext
		{
		public:
			UINT connectorId = 0;
			LUID adapterLuid{};
			UINT targetId = 0;
			GUID monitorGuid{};
			// The handle that added this monitor; closing it removes the monitor.
			WDFFILEOBJECT owner = nullptr;

			uint8_t* pEdidData = nullptr;
			VirtualMonitorMode preferredMode{};
			IDDCX_ADAPTER m_Adapter{};

			IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor);
			virtual ~IndirectMonitorContext();

			// False when the monitor is departing: the swap chain is refused.
			bool AssignSwapChain(const IDDCX_MONITOR& MonitorObject, const IDDCX_SWAPCHAIN& SwapChain, const LUID& RenderAdapter, const HANDLE& NewFrameEvent);
			void UnassignSwapChain();
			// Before IddCxMonitorDeparture: stops the swap-chain thread, so it
			// cannot call into a swap chain IddCx is tearing down, and refuses
			// new ones.
			void BeginDeparture();

			IDDCX_MONITOR GetMonitor() const;

		private:
			// Takes the processing thread out under the lock and lets it finish
			// outside it: the thread's last act (deleting its swap chain) may
			// call back into IddCx.
			void StopProcessing(bool departing);

			IDDCX_MONITOR m_Monitor;
			std::mutex m_ProcessingLock;  // Assign/Unassign/departure come on different threads
			bool m_Departing = false;
			std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
			Microsoft::WRL::Wrappers::Event m_hCursorEvent;
		};

		/// <summary>
		/// The virtual display adapter: one per device.
		/// </summary>
		class IndirectDeviceContext
		{
		public:
			IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
			virtual ~IndirectDeviceContext();

			void InitAdapter();

			void SetRenderAdapter(const LUID& AdapterLuid);

			// Creates and plugs in a monitor owned by `owner`. The connector
			// slot must already be reserved.
			NTSTATUS CreateMonitor(IndirectMonitorContext*& pMonitorContext, UINT connector, uint8_t* edidData,
				const GUID& containerId, const VirtualMonitorMode& preferredMode, WDFFILEOBJECT owner);

		protected:
			WDFDEVICE m_WdfDevice;
			IDDCX_ADAPTER m_Adapter;
		};
	}
}
