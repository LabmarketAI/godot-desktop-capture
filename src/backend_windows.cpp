#ifdef _WIN32

#include "backend_windows.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string _hresult_str(HRESULT hr) {
	char buf[16];
	snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
	return buf;
}

void DXGICaptureBackend::_log(const std::string &msg) {
	if (_log_callback) {
		_log_callback(msg);
	}
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

DXGICaptureBackend::~DXGICaptureBackend() {
	stop();
}

// ---------------------------------------------------------------------------
// Public: start / stop
// ---------------------------------------------------------------------------

bool DXGICaptureBackend::start(int monitor_index, bool capture_cursor,
		int max_fps, DXGIFrameCallback callback, std::string &error_out) {
	if (_running.load()) {
		stop();
	}

	_monitor_index = monitor_index;
	_capture_cursor_enabled = capture_cursor;
	_max_fps = std::max(max_fps, 1);
	_frame_callback = std::move(callback);

	if (!_init(monitor_index, error_out)) {
		_release_d3d();
		return false;
	}

	_running.store(true);
	_thread = std::thread(&DXGICaptureBackend::_capture_loop, this);
	return true;
}

void DXGICaptureBackend::stop() {
	_running.store(false);
	if (_thread.joinable()) {
		_thread.join();
	}
	_release_duplication();
	_release_d3d();
	_cursor_valid = false;
}

// ---------------------------------------------------------------------------
// Public: static helpers
// ---------------------------------------------------------------------------

int DXGICaptureBackend::enumerate_monitor_count() {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory)))) {
		return 0;
	}

	int count = 0;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
			++count;
			output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return count;
}

bool DXGICaptureBackend::get_monitor_size(int index, int32_t &out_w, int32_t &out_h) {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory)))) {
		return false;
	}

	int current = 0;
	bool found = false;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
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
// Private: initialisation
// ---------------------------------------------------------------------------

bool DXGICaptureBackend::_init(int monitor_index, std::string &error_out) {
	// Enumerate all adapter+output pairs to find monitor at monitor_index.
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory)))) {
		error_out = "dxgi_factory_create_failed";
		return false;
	}

	IDXGIAdapter1 *target_adapter = nullptr;
	IDXGIOutput *target_output = nullptr;
	int current = 0;

	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
			if (current == monitor_index) {
				target_adapter = adapter;
				target_output = output;
				break;
			}
			++current;
			output->Release();
		}
		if (target_output) {
			break;
		}
		adapter->Release();
	}
	factory->Release();

	if (!target_output) {
		error_out = "monitor_index_out_of_range";
		return false;
	}

	// Record output dimensions.
	DXGI_OUTPUT_DESC out_desc{};
	target_output->GetDesc(&out_desc);
	_frame_width = out_desc.DesktopCoordinates.right - out_desc.DesktopCoordinates.left;
	_frame_height = out_desc.DesktopCoordinates.bottom - out_desc.DesktopCoordinates.top;

	// Create D3D11 device on the adapter that owns this output.
	D3D_FEATURE_LEVEL feature_level{};
	HRESULT hr = D3D11CreateDevice(
			target_adapter,
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			0,
			nullptr, 0,
			D3D11_SDK_VERSION,
			&_device,
			&feature_level,
			&_context);

	target_adapter->Release();

	if (FAILED(hr)) {
		target_output->Release();
		error_out = "d3d11_device_create_failed";
		return false;
	}

	// Get IDXGIOutput1 for DuplicateOutput.
	IDXGIOutput1 *output1 = nullptr;
	hr = target_output->QueryInterface(__uuidof(IDXGIOutput1),
			reinterpret_cast<void **>(&output1));
	target_output->Release();

	if (FAILED(hr)) {
		error_out = "dxgi_output1_unavailable";
		return false;
	}

	hr = output1->DuplicateOutput(_device, &_duplication);
	output1->Release();

	if (FAILED(hr)) {
		if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
			error_out = "dxgi_duplication_limit_reached";
		} else if (hr == E_ACCESSDENIED) {
			error_out = "permission_denied";
		} else {
			error_out = "dxgi_duplicate_output_failed";
		}
		_log("DesktopCapture: DuplicateOutput failed hr=" + _hresult_str(hr) + " reason=" + error_out);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Private: resource management
// ---------------------------------------------------------------------------

void DXGICaptureBackend::_release_duplication() {
	if (_duplication) {
		_duplication->Release();
		_duplication = nullptr;
	}
}

void DXGICaptureBackend::_release_d3d() {
	if (_staging) {
		_staging->Release();
		_staging = nullptr;
		_staging_w = 0;
		_staging_h = 0;
	}
	if (_context) {
		_context->Release();
		_context = nullptr;
	}
	if (_device) {
		_device->Release();
		_device = nullptr;
	}
}

bool DXGICaptureBackend::_ensure_staging(int32_t w, int32_t h) {
	if (_staging && _staging_w == w && _staging_h == h) {
		return false;
	}
	if (_staging) {
		_staging->Release();
		_staging = nullptr;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	if (FAILED(_device->CreateTexture2D(&desc, nullptr, &_staging))) {
		return false;
	}
	_staging_w = w;
	_staging_h = h;
	return true;
}

// ---------------------------------------------------------------------------
// Private: reinitialise duplication after ACCESS_LOST
// ---------------------------------------------------------------------------

bool DXGICaptureBackend::_reinit_duplication() {
	_release_duplication();

	// Retry up to 10 times with 200 ms delays (total ~2 s) to handle transient
	// desktop lock / session switch scenarios.
	for (int attempt = 0; attempt < 10; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));

		// Re-enumerate to locate the output (adapter index is stable).
		IDXGIFactory1 *factory = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
					reinterpret_cast<void **>(&factory)))) {
			continue;
		}

		IDXGIOutput1 *output1 = nullptr;
		int current = 0;
		IDXGIAdapter1 *adapter = nullptr;
		for (UINT a = 0; !output1 && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
			IDXGIOutput *output = nullptr;
			for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
				if (current == _monitor_index) {
					output->QueryInterface(__uuidof(IDXGIOutput1),
							reinterpret_cast<void **>(&output1));
					output->Release();
					break;
				}
				++current;
				output->Release();
			}
			adapter->Release();
		}
		factory->Release();

		if (!output1) {
			continue;
		}

		HRESULT hr = output1->DuplicateOutput(_device, &_duplication);
		output1->Release();
		if (SUCCEEDED(hr)) {
			_log("DesktopCapture: _reinit_duplication succeeded on attempt " + std::to_string(attempt + 1));
			return true;
		}
		_log("DesktopCapture: _reinit_duplication attempt " + std::to_string(attempt + 1) + " failed hr=" + _hresult_str(hr));
	}
	_log("DesktopCapture: _reinit_duplication exhausted all retries");
	return false;
}

// ---------------------------------------------------------------------------
// Private: cursor shape helpers
// ---------------------------------------------------------------------------

bool DXGICaptureBackend::_update_cursor_shape(IDXGIOutputDuplication *dup,
		const DXGI_OUTDUPL_FRAME_INFO &info) {
	if (info.PointerShapeBufferSize == 0) {
		return false;
	}

	_cursor_shape.data.resize(info.PointerShapeBufferSize);
	UINT required = 0;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info{};

	HRESULT hr = dup->GetFramePointerShape(
			info.PointerShapeBufferSize,
			_cursor_shape.data.data(),
			&required,
			&shape_info);

	if (FAILED(hr)) {
		return false;
	}

	_cursor_shape.type = static_cast<DXGI_OUTDUPL_POINTER_SHAPE_TYPE>(shape_info.Type);
	_cursor_shape.width = static_cast<int32_t>(shape_info.Width);
	_cursor_shape.height = static_cast<int32_t>(shape_info.Height);
	_cursor_shape.pitch = static_cast<int32_t>(shape_info.Pitch);
	_cursor_shape.hot_x = static_cast<int32_t>(shape_info.HotSpot.x);
	_cursor_shape.hot_y = static_cast<int32_t>(shape_info.HotSpot.y);

	// For monochrome cursors the bitmap is Height*2 rows: AND mask then XOR mask.
	// Adjust so _cursor_shape.height refers to the visible half only.
	if (_cursor_shape.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
		_cursor_shape.height /= 2;
	}

	_cursor_valid = true;
	return true;
}

void DXGICaptureBackend::_composite_cursor(uint8_t *rgba, int32_t frame_w,
		int32_t frame_h, DXGI_OUTDUPL_POINTER_POSITION cursor_pos) {
	if (!_cursor_valid || !cursor_pos.Visible) {
		return;
	}

	const int32_t cur_x = cursor_pos.Position.x - _cursor_shape.hot_x;
	const int32_t cur_y = cursor_pos.Position.y - _cursor_shape.hot_y;

	if (_cursor_shape.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
		// 32-bit BGRA pre-multiplied alpha cursor — blend over frame.
		for (int32_t cy = 0; cy < _cursor_shape.height; ++cy) {
			const int32_t fy = cur_y + cy;
			if (fy < 0 || fy >= frame_h) {
				continue;
			}
			for (int32_t cx = 0; cx < _cursor_shape.width; ++cx) {
				const int32_t fx = cur_x + cx;
				if (fx < 0 || fx >= frame_w) {
					continue;
				}
				const uint8_t *src = _cursor_shape.data.data() +
						cy * _cursor_shape.pitch + cx * 4;
				uint8_t *dst = rgba + (fy * frame_w + fx) * 4;
				// src is BGRA pre-multiplied; dst is RGBA.
				const uint8_t b = src[0];
				const uint8_t g = src[1];
				const uint8_t r = src[2];
				const uint8_t a = src[3];
				const uint8_t inv_a = 255 - a;
				dst[0] = r + (dst[0] * inv_a + 127) / 255;
				dst[1] = g + (dst[1] * inv_a + 127) / 255;
				dst[2] = b + (dst[2] * inv_a + 127) / 255;
				dst[3] = 255;
			}
		}
	} else if (_cursor_shape.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
		// 1-bit AND + XOR masks.  AND=0,XOR=0 → black; AND=0,XOR=1 → white;
		// AND=1,XOR=0 → transparent; AND=1,XOR=1 → inverse.
		const uint8_t *and_mask = _cursor_shape.data.data();
		const uint8_t *xor_mask = and_mask + _cursor_shape.height * _cursor_shape.pitch;

		for (int32_t cy = 0; cy < _cursor_shape.height; ++cy) {
			const int32_t fy = cur_y + cy;
			if (fy < 0 || fy >= frame_h) {
				continue;
			}
			for (int32_t cx = 0; cx < _cursor_shape.width; ++cx) {
				const int32_t fx = cur_x + cx;
				if (fx < 0 || fx >= frame_w) {
					continue;
				}
				const int byte_idx = cy * _cursor_shape.pitch + cx / 8;
				const int bit = 7 - (cx % 8);
				const bool and_bit = (and_mask[byte_idx] >> bit) & 1;
				const bool xor_bit = (xor_mask[byte_idx] >> bit) & 1;
				uint8_t *dst = rgba + (fy * frame_w + fx) * 4;
				if (!and_bit && !xor_bit) {
					dst[0] = dst[1] = dst[2] = 0;
					dst[3] = 255;
				} else if (!and_bit && xor_bit) {
					dst[0] = dst[1] = dst[2] = 255;
					dst[3] = 255;
				} else if (and_bit && xor_bit) {
					dst[0] ^= 255;
					dst[1] ^= 255;
					dst[2] ^= 255;
				}
				// and=1 xor=0 → transparent, leave dst unchanged
			}
		}
	}
	// MASKED type is treated as COLOR above since the bit layout is compatible.
}

// ---------------------------------------------------------------------------
// Private: capture loop
// ---------------------------------------------------------------------------

void DXGICaptureBackend::_capture_loop() {
	const int64_t frame_interval_us = 1'000'000 / _max_fps;

	// Pixel conversion buffer — allocated once and grown as needed.
	std::vector<uint8_t> rgba_buf;

	while (_running.load()) {
		auto frame_start = std::chrono::steady_clock::now();

		if (!_duplication) {
			// Lost the duplication handle — nothing to do until reinit succeeds.
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		// --- Acquire next frame ---
		DXGI_OUTDUPL_FRAME_INFO frame_info{};
		IDXGIResource *desktop_resource = nullptr;

		// Timeout of 0 ms: return immediately if no new frame.
		HRESULT hr = _duplication->AcquireNextFrame(0, &frame_info, &desktop_resource);

		if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
			// No new frame this tick — throttle and try again.
			goto throttle;
		}

		if (hr == DXGI_ERROR_ACCESS_LOST) {
			_log("DesktopCapture: AcquireNextFrame ACCESS_LOST (0x887A0026), attempting reinit");
			if (desktop_resource) {
				desktop_resource->Release();
			}
			if (!_reinit_duplication()) {
				_log("DesktopCapture: reinit failed, stopping capture thread");
				if (_error_callback) {
					_error_callback("device_lost");
				}
				_running.store(false);
			}
			continue;
		}

		if (FAILED(hr)) {
			_log("DesktopCapture: AcquireNextFrame non-recoverable failure hr=" + _hresult_str(hr) + ", stopping capture thread");
			if (desktop_resource) {
				desktop_resource->Release();
			}
			if (_error_callback) {
				_error_callback("device_lost");
			}
			// Non-recoverable error.
			_running.store(false);
			continue;
		}

		{
			// --- Update cursor shape if changed ---
			if (_capture_cursor_enabled && frame_info.PointerShapeBufferSize > 0) {
				_update_cursor_shape(_duplication, frame_info);
			}

			// --- Copy desktop surface to staging texture ---
			ID3D11Texture2D *desktop_tex = nullptr;
			hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D),
					reinterpret_cast<void **>(&desktop_tex));
			desktop_resource->Release();
			desktop_resource = nullptr;

			if (FAILED(hr)) {
				_duplication->ReleaseFrame();
				goto throttle;
			}

			D3D11_TEXTURE2D_DESC tex_desc{};
			desktop_tex->GetDesc(&tex_desc);
			const int32_t w = static_cast<int32_t>(tex_desc.Width);
			const int32_t h = static_cast<int32_t>(tex_desc.Height);

			_ensure_staging(w, h);

			if (_staging) {
				_context->CopyResource(_staging, desktop_tex);
			}
			desktop_tex->Release();

			_duplication->ReleaseFrame();

			if (!_staging) {
				goto throttle;
			}

			// --- Map and convert BGRA → RGBA ---
			D3D11_MAPPED_SUBRESOURCE mapped{};
			hr = _context->Map(_staging, 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(hr)) {
				goto throttle;
			}

			const int32_t pixel_count = w * h;
			rgba_buf.resize(static_cast<size_t>(pixel_count) * 4);

			const uint8_t *src_row = static_cast<const uint8_t *>(mapped.pData);
			uint8_t *dst_row = rgba_buf.data();
			for (int32_t row = 0; row < h; ++row) {
				const uint8_t *s = src_row;
				uint8_t *d = dst_row;
				for (int32_t col = 0; col < w; ++col) {
					// DXGI: B G R A  →  Godot: R G B A
					d[0] = s[2]; // R
					d[1] = s[1]; // G
					d[2] = s[0]; // B
					d[3] = s[3]; // A
					s += 4;
					d += 4;
				}
				src_row += mapped.RowPitch;
				dst_row += w * 4;
			}

			_context->Unmap(_staging, 0);

			// --- Composite cursor ---
			if (_capture_cursor_enabled) {
				_composite_cursor(rgba_buf.data(), w, h,
						frame_info.PointerPosition);
			}

			// --- Invoke frame callback ---
			if (_frame_callback) {
				_frame_callback(rgba_buf.data(), w, h);
			}
		}

	throttle : {
		auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - frame_start)
							   .count();
		int64_t sleep_us = frame_interval_us - elapsed;
		if (sleep_us > 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
		}
	}
	}
}

#endif // _WIN32
