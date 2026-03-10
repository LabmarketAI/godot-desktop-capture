#ifdef _WIN32

// Windows.Graphics.Capture requires Windows 10 1903+.
// Override any earlier _WIN32_WINNT/WINVER in this TU only.
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#ifndef NOMINMAX
#define NOMINMAX
#endif

// ---- D3D / DXGI (before WinRT) ----
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// ---- C++/WinRT projections ----
// Must come after windows.h (pulled in by d3d11.h).
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// ---- WGC interop header (IGraphicsCaptureItemInterop + CreateDirect3D11DeviceFromDXGIDevice) ----
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
// Bring the non-WinRT COM interop type into scope.
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

#include "backend_wgc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace wf = winrt::Windows::Foundation;

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string _hr_str(HRESULT hr) {
	char buf[16];
	snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
	return buf;
}

// Get the HMONITOR for a zero-based monitor index using DXGI adapter/output
// enumeration -- same ordering as DXGICaptureBackend.
static HMONITOR _hmonitor_for_index(int index) {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory)))) {
		return NULL;
	}

	HMONITOR result = NULL;
	int current = 0;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0;
			!result && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
			++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0;
				adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
				++o) {
			if (current == index) {
				DXGI_OUTPUT_DESC desc{};
				if (SUCCEEDED(output->GetDesc(&desc))) {
					result = desc.Monitor;
				}
				output->Release();
				break;
			}
			++current;
			output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return result;
}

// Wrap a raw ID3D11Device in a WinRT IDirect3DDevice for the WGC frame pool.
static HRESULT _make_winrt_device(ID3D11Device *d3d, wgd3d::IDirect3DDevice &out) {
	ComPtr<IDXGIDevice> dxgi;
	HRESULT hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
	if (FAILED(hr)) {
		return hr;
	}
	::IInspectable *raw = nullptr;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &raw);
	if (FAILED(hr)) {
		return hr;
	}
	// Transfer ownership to a C++/WinRT projected type (no extra AddRef).
	wf::IInspectable insp;
	winrt::attach_abi(insp, reinterpret_cast<void *>(raw));
	out = insp.as<wgd3d::IDirect3DDevice>();
	return S_OK;
}

// Unwrap a WGC frame surface to the underlying ID3D11Texture2D.
// Primary path: IDirect3DDxgiInterfaceAccess (standard WGC interop header).
// Fallback: IDXGISurface -> ID3D11Texture2D direct QI, which works when the
// VR runtime places frames on a different adapter and the interop header's
// IID is not registered on the surface object.
static HRESULT _texture_from_surface(wgd3d::IDirect3DSurface const &surface,
		ID3D11Texture2D **out, std::string *diag = nullptr) {
	auto *raw = static_cast<::IUnknown *>(winrt::get_abi(surface));
	if (!raw) {
		if (diag) *diag = "get_abi returned null";
		return E_POINTER;
	}

	// Primary: IDirect3DDxgiInterfaceAccess
	{
		ComPtr<IDirect3DDxgiInterfaceAccess> dxgi_access;
		HRESULT hr = raw->QueryInterface(IID_PPV_ARGS(&dxgi_access));
		if (SUCCEEDED(hr)) {
			hr = dxgi_access->GetInterface(IID_PPV_ARGS(out));
			if (SUCCEEDED(hr)) return S_OK;
			if (diag) *diag = "GetInterface(ID3D11Texture2D) hr=" + _hr_str(hr);
			return hr;
		}
	}

	// Fallback: IDXGISurface -> ID3D11Texture2D
	// Needed when the VR compositor places WGC frames on its own D3D device
	// where IDirect3DDxgiInterfaceAccess is not exposed.
	{
		ComPtr<IDXGISurface> dxgi_surf;
		HRESULT hr = raw->QueryInterface(IID_PPV_ARGS(&dxgi_surf));
		if (SUCCEEDED(hr)) {
			hr = dxgi_surf->QueryInterface(IID_PPV_ARGS(out));
			if (SUCCEEDED(hr)) return S_OK;
			if (diag) *diag = "IDXGISurface->ID3D11Texture2D hr=" + _hr_str(hr);
			return hr;
		}
		if (diag) *diag = "QI(IDirect3DDxgiInterfaceAccess) and QI(IDXGISurface) both failed hr=" + _hr_str(hr);
		return hr;
	}
}

// ---------------------------------------------------------------------------
// Destructor / stop
// ---------------------------------------------------------------------------

WGCCaptureBackend::~WGCCaptureBackend() {
	stop();
}

void WGCCaptureBackend::stop() {
	_running.store(false);
	if (_thread.joinable()) {
		_thread.join();
	}
}

void WGCCaptureBackend::_log(const std::string &msg) {
	if (_log_callback) {
		_log_callback(msg);
	}
}

// ---------------------------------------------------------------------------
// Static helpers -- DXGI enumeration, same ordering as DXGICaptureBackend
// ---------------------------------------------------------------------------

int WGCCaptureBackend::enumerate_monitor_count() {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory)))) {
		return 0;
	}
	int count = 0;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0;
			factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
			++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0;
				adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
				++o) {
			++count;
			output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return count;
}

bool WGCCaptureBackend::get_monitor_size(int index, int32_t &out_w, int32_t &out_h) {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory)))) {
		return false;
	}
	int current = 0;
	bool found = false;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0;
			!found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
			++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0;
				adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
				++o) {
			if (current == index) {
				DXGI_OUTPUT_DESC desc{};
				if (SUCCEEDED(output->GetDesc(&desc))) {
					out_w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
					out_h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
					found = true;
				}
				output->Release();
				break;
			}
			++current;
			output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return found;
}

// ---------------------------------------------------------------------------
// start() -- spawns capture thread, waits for WGC init result
// ---------------------------------------------------------------------------

bool WGCCaptureBackend::start(int monitor_index, bool capture_cursor,
		int max_fps, std::function<void(const uint8_t *, int32_t, int32_t)> callback,
		std::string &error_out) {
	if (_running.load()) {
		stop();
	}
	_frame_callback = std::move(callback);

	std::promise<std::pair<bool, std::string>> ready_promise;
	auto ready_future = ready_promise.get_future();

	_running.store(true);
	_thread = std::thread(
			[this, monitor_index, capture_cursor, max_fps,
					prom = std::move(ready_promise)]() mutable {
				// ---- WinRT MTA apartment ----
				bool com_ok = false;
				try {
					winrt::init_apartment(winrt::apartment_type::multi_threaded);
					com_ok = true;
				} catch (winrt::hresult_error const &e) {
					_log("DesktopCapture[WGC]: winrt::init_apartment failed hr=" +
							_hr_str(e.code()));
					prom.set_value({ false, "wgc_winrt_init_failed" });
					_running.store(false);
					return;
				}

				// ---- D3D11 device with BGRA support ----
				ComPtr<ID3D11Device> d3d_device;
				ComPtr<ID3D11DeviceContext> d3d_context;
				{
					D3D_FEATURE_LEVEL fl{};
					HRESULT hr = D3D11CreateDevice(
							nullptr,
							D3D_DRIVER_TYPE_HARDWARE,
							nullptr,
							D3D11_CREATE_DEVICE_BGRA_SUPPORT,
							nullptr, 0,
							D3D11_SDK_VERSION,
							&d3d_device, &fl, &d3d_context);
					if (FAILED(hr)) {
						_log("DesktopCapture[WGC]: D3D11CreateDevice failed hr=" + _hr_str(hr));
						prom.set_value({ false, "wgc_d3d11_create_failed" });
						_running.store(false);
						winrt::uninit_apartment();
						return;
					}
				}

				// ---- Wrap D3D11 device as WinRT IDirect3DDevice ----
				wgd3d::IDirect3DDevice winrt_device{ nullptr };
				{
					HRESULT hr = _make_winrt_device(d3d_device.Get(), winrt_device);
					if (FAILED(hr)) {
						_log("DesktopCapture[WGC]: D3D11→WinRT device interop failed hr=" +
								_hr_str(hr));
						prom.set_value({ false, "wgc_d3d11_interop_failed" });
						_running.store(false);
						winrt::uninit_apartment();
						return;
					}
				}

				// ---- HMONITOR for the requested index ----
				HMONITOR hmonitor = _hmonitor_for_index(monitor_index);
				if (!hmonitor) {
					_log("DesktopCapture[WGC]: monitor index " +
							std::to_string(monitor_index) + " not found");
					prom.set_value({ false, "monitor_index_out_of_range" });
					_running.store(false);
					winrt::uninit_apartment();
					return;
				}

				// ---- GraphicsCaptureItem for the monitor ----
				wgc::GraphicsCaptureItem item{ nullptr };
				try {
					auto interop = winrt::get_activation_factory<
							wgc::GraphicsCaptureItem,
							IGraphicsCaptureItemInterop>();
					winrt::check_hresult(interop->CreateForMonitor(
							hmonitor,
							winrt::guid_of<wgc::GraphicsCaptureItem>(),
							winrt::put_abi(item)));
				} catch (winrt::hresult_error const &e) {
					_log("DesktopCapture[WGC]: GraphicsCaptureItem create failed hr=" +
							_hr_str(e.code()));
					prom.set_value({ false, "wgc_capture_item_failed" });
					_running.store(false);
					winrt::uninit_apartment();
					return;
				}

				// ---- Frame pool ----
				wgc::Direct3D11CaptureFramePool frame_pool{ nullptr };
				try {
					frame_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
							winrt_device,
							wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
							2,
							item.Size());
				} catch (winrt::hresult_error const &e) {
					_log("DesktopCapture[WGC]: frame pool create failed hr=" +
							_hr_str(e.code()));
					prom.set_value({ false, "wgc_frame_pool_failed" });
					_running.store(false);
					winrt::uninit_apartment();
					return;
				}

				// ---- Capture session ----
				wgc::GraphicsCaptureSession session{ nullptr };
				try {
					session = frame_pool.CreateCaptureSession(item);
					session.IsCursorCaptureEnabled(capture_cursor);
					// Disable yellow recording border on Win11 22H2+ (best-effort).
					try {
						session.IsBorderRequired(false);
					} catch (...) {}
					session.StartCapture();
				} catch (winrt::hresult_error const &e) {
					_log("DesktopCapture[WGC]: session start failed hr=" + _hr_str(e.code()));
					prom.set_value({ false, "wgc_session_start_failed" });
					_running.store(false);
					try { frame_pool.Close(); } catch (...) {}
					winrt::uninit_apartment();
					return;
				}

				_log("DesktopCapture[WGC]: capture started on monitor " +
						std::to_string(monitor_index));
				prom.set_value({ true, "" });

				// ---- Polling loop -- wrapped in top-level try/catch ----
				// Any C++/WinRT exception that slips past the inner IIFE catch
				// is caught here rather than letting it reach std::terminate().
				try {
					const int64_t frame_interval_us = 1'000'000 / std::max(max_fps, 1);
					ComPtr<ID3D11Texture2D> staging;
					int32_t staging_w = 0, staging_h = 0;
					std::vector<uint8_t> rgba_buf;

					// If no frame arrives within this window after StartCapture(),
					// the VR runtime has likely suspended DWM desktop composition.
					// Emit an error so GDScript can react (e.g. restart the capture).
					bool got_first_frame = false;
					const auto first_frame_deadline =
							std::chrono::steady_clock::now() + std::chrono::seconds(5);

					while (_running.load()) {
						auto frame_start = std::chrono::steady_clock::now();

						// IIFE -- exceptions caught inside so the throttle always runs.
						[&]() {
							try {
								wgc::Direct3D11CaptureFrame frame =
										frame_pool.TryGetNextFrame();
								if (!frame) {
									return; // No new frame -- sleep and retry.
								}
								got_first_frame = true;

								// Get underlying D3D11 texture via non-throwing raw QI.
								ComPtr<ID3D11Texture2D> frame_tex;
								std::string tex_diag;
								HRESULT hr = _texture_from_surface(frame.Surface(),
										&frame_tex, &tex_diag);
								if (FAILED(hr)) {
									_log("DesktopCapture[WGC]: surface→texture failed: " +
											tex_diag);
									try { frame.Close(); } catch (...) {}
									return;
								}

								D3D11_TEXTURE2D_DESC fd{};
								frame_tex->GetDesc(&fd);
								const int32_t w = static_cast<int32_t>(fd.Width);
								const int32_t h = static_cast<int32_t>(fd.Height);

								// Ensure staging matches frame dimensions.
								if (!staging || staging_w != w || staging_h != h) {
									staging.Reset();
									D3D11_TEXTURE2D_DESC desc{};
									desc.Width = fd.Width;
									desc.Height = fd.Height;
									desc.MipLevels = 1;
									desc.ArraySize = 1;
									desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
									desc.SampleDesc.Count = 1;
									desc.Usage = D3D11_USAGE_STAGING;
									desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
									if (FAILED(d3d_device->CreateTexture2D(
												    &desc, nullptr, &staging))) {
										try { frame.Close(); } catch (...) {}
										return;
									}
									staging_w = w;
									staging_h = h;
								}

								d3d_context->CopyResource(staging.Get(), frame_tex.Get());
								try { frame.Close(); } catch (...) {}

								D3D11_MAPPED_SUBRESOURCE mapped{};
								if (FAILED(d3d_context->Map(staging.Get(), 0,
											    D3D11_MAP_READ, 0, &mapped))) {
									return;
								}

								rgba_buf.resize(static_cast<size_t>(w * h) * 4);
								const uint8_t *src_row =
										static_cast<const uint8_t *>(mapped.pData);
								uint8_t *dst_row = rgba_buf.data();
								for (int32_t row = 0; row < h; ++row) {
									const uint8_t *s = src_row;
									uint8_t *d = dst_row;
									for (int32_t col = 0; col < w; ++col) {
										d[0] = s[2]; // R
										d[1] = s[1]; // G
										d[2] = s[0]; // B
										d[3] = 255; // A (WGC frames are fully opaque)
										s += 4;
										d += 4;
									}
									src_row += mapped.RowPitch;
									dst_row += w * 4;
								}
								d3d_context->Unmap(staging.Get(), 0);

								if (_frame_callback) {
									_frame_callback(rgba_buf.data(), w, h);
								}
							} catch (winrt::hresult_error const &e) {
								_log("DesktopCapture[WGC]: frame loop hr=" +
										_hr_str(e.code()) + " -- stopping capture");
								if (_error_callback) {
									_error_callback("device_lost");
								}
								_running.store(false);
							} catch (...) {
								_log("DesktopCapture[WGC]: unexpected exception in frame loop -- stopping capture");
								if (_error_callback) {
									_error_callback("device_lost");
								}
								_running.store(false);
							}
						}(); // end IIFE

						// First-frame timeout: if the VR runtime suspended DWM,
						// TryGetNextFrame() will return null indefinitely with no error.
						// Detect this and surface it as a diagnosable signal.
						if (!got_first_frame &&
								std::chrono::steady_clock::now() > first_frame_deadline) {
							_log("DesktopCapture[WGC]: no frames received within 5 s of "
									"StartCapture() -- DWM composition may be suspended by "
									"the VR runtime. Stopping capture.");
							if (_error_callback) {
								_error_callback("wgc_no_frames_timeout");
							}
							_running.store(false);
							break;
						}

						// Throttle to max_fps.
						auto elapsed =
								std::chrono::duration_cast<std::chrono::microseconds>(
										std::chrono::steady_clock::now() - frame_start)
										.count();
						int64_t sleep_us = frame_interval_us - elapsed;
						if (sleep_us > 0) {
							std::this_thread::sleep_for(
									std::chrono::microseconds(sleep_us));
						}
					}
				} catch (winrt::hresult_error const &e) {
					_log("DesktopCapture[WGC]: unhandled hr=" + _hr_str(e.code()) +
							" in capture thread");
				} catch (...) {
					_log("DesktopCapture[WGC]: unknown exception in capture thread");
				}

				// ---- Cleanup ----
				try { session.Close(); } catch (...) {}
				try { frame_pool.Close(); } catch (...) {}

				_log("DesktopCapture[WGC]: capture thread exiting");
				if (com_ok) {
					winrt::uninit_apartment();
				}
			});

	auto [ok, err] = ready_future.get();
	if (!ok) {
		error_out = err;
		if (_thread.joinable()) {
			_thread.join();
		}
		return false;
	}
	return true;
}

#endif // _WIN32
