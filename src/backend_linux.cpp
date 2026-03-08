#ifdef __linux__

#include "backend_linux.h"

// PipeWire / SPA headers — included for types and static-inline helpers.
// Functions are loaded via dlopen; the build only passes --cflags (no -l flags).
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

// D-Bus headers — types only; all functions loaded via dlopen.
#include <dbus/dbus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <glob.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// ============================================================
// Section 1 — Dynamic library loading
// ============================================================

// ---- D-Bus ----
// Each entry: X(return_type, symbol_name, (arg_types...))
#define DBUS_FUNCS(X) \
	X(void, dbus_error_init, (DBusError *)) \
	X(void, dbus_error_free, (DBusError *)) \
	X(dbus_bool_t, dbus_error_is_set, (const DBusError *)) \
	X(DBusConnection *, dbus_bus_get, (DBusBusType, DBusError *)) \
	X(void, dbus_bus_add_match, (DBusConnection *, const char *, DBusError *)) \
	X(void, dbus_connection_unref, (DBusConnection *)) \
	X(void, dbus_connection_flush, (DBusConnection *)) \
	X(dbus_bool_t, dbus_connection_read_write, (DBusConnection *, int)) \
	X(DBusMessage *, dbus_connection_pop_message, (DBusConnection *)) \
	X(DBusMessage *, dbus_connection_send_with_reply_and_block, \
			(DBusConnection *, DBusMessage *, int, DBusError *)) \
	X(DBusMessage *, dbus_message_new_method_call, \
			(const char *, const char *, const char *, const char *)) \
	X(void, dbus_message_unref, (DBusMessage *)) \
	X(int, dbus_message_get_type, (DBusMessage *)) \
	X(const char *, dbus_message_get_member, (DBusMessage *)) \
	X(const char *, dbus_message_get_path, (DBusMessage *)) \
	X(dbus_bool_t, dbus_message_iter_init, (DBusMessage *, DBusMessageIter *)) \
	X(void, dbus_message_iter_init_append, (DBusMessage *, DBusMessageIter *)) \
	X(int, dbus_message_iter_get_arg_type, (DBusMessageIter *)) \
	X(void, dbus_message_iter_get_basic, (DBusMessageIter *, void *)) \
	X(dbus_bool_t, dbus_message_iter_next, (DBusMessageIter *)) \
	X(void, dbus_message_iter_recurse, (DBusMessageIter *, DBusMessageIter *)) \
	X(dbus_bool_t, dbus_message_iter_append_basic, (DBusMessageIter *, int, const void *)) \
	X(dbus_bool_t, dbus_message_iter_open_container, \
			(DBusMessageIter *, int, const char *, DBusMessageIter *)) \
	X(dbus_bool_t, dbus_message_iter_close_container, \
			(DBusMessageIter *, DBusMessageIter *))

struct DynDbus {
	void *handle = nullptr;
#define FIELD(ret, name, args) ret(*name) args = nullptr;
	DBUS_FUNCS(FIELD)
#undef FIELD

	bool load() {
		handle = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
		if (!handle) {
			return false;
		}
#define RESOLVE(ret, name, args) \
	name = reinterpret_cast<ret(*) args>(dlsym(handle, #name)); \
	if (!name) { \
		dlclose(handle); \
		handle = nullptr; \
		return false; \
	}
		DBUS_FUNCS(RESOLVE)
#undef RESOLVE
		return true;
	}

	void unload() {
		if (handle) {
			dlclose(handle);
			handle = nullptr;
		}
	}
};

// ---- PipeWire ----
// Only true ABI exports from libpipewire-0.3.so.0.
// pw_stream_add_listener, pw_stream_connect, pw_stream_dequeue_buffer,
// pw_stream_queue_buffer, pw_core_disconnect etc. are static-inline vtable
// wrappers — call them directly, do NOT add them here.
#define PW_FUNCS(X) \
	X(void, pw_init, (int *, char ***)) \
	X(void, pw_deinit, (void)) \
	X(struct pw_main_loop *, pw_main_loop_new, (const struct spa_dict *)) \
	X(void, pw_main_loop_destroy, (struct pw_main_loop *)) \
	X(int, pw_main_loop_run, (struct pw_main_loop *)) \
	X(int, pw_main_loop_quit, (struct pw_main_loop *)) \
	X(struct pw_loop *, pw_main_loop_get_loop, (struct pw_main_loop *)) \
	X(struct pw_context *, pw_context_new, \
			(struct pw_loop *, struct pw_properties *, size_t)) \
	X(void, pw_context_destroy, (struct pw_context *)) \
	X(struct pw_core *, pw_context_connect, \
			(struct pw_context *, struct pw_properties *, size_t)) \
	X(void, pw_properties_free, (struct pw_properties *)) \
	X(struct pw_stream *, pw_stream_new, \
			(struct pw_core *, const char *, struct pw_properties *)) \
	X(void, pw_stream_destroy, (struct pw_stream *))

struct DynPW {
	void *handle = nullptr;
	// pw_properties_new is variadic — declared separately.
	using pw_properties_new_fn = struct pw_properties *(*)(const char *, ...);
	pw_properties_new_fn pw_properties_new = nullptr;

#define FIELD(ret, name, args) ret(*name) args = nullptr;
	PW_FUNCS(FIELD)
#undef FIELD

	bool load() {
		handle = dlopen("libpipewire-0.3.so.0", RTLD_LAZY | RTLD_LOCAL);
		if (!handle) {
			return false;
		}
		pw_properties_new = reinterpret_cast<pw_properties_new_fn>(
				dlsym(handle, "pw_properties_new"));
		if (!pw_properties_new) {
			dlclose(handle);
			handle = nullptr;
			return false;
		}
#define RESOLVE(ret, name, args) \
	name = reinterpret_cast<ret(*) args>(dlsym(handle, #name)); \
	if (!name) { \
		dlclose(handle); \
		handle = nullptr; \
		return false; \
	}
		PW_FUNCS(RESOLVE)
#undef RESOLVE
		return true;
	}

	void unload() {
		if (handle) {
			dlclose(handle);
			handle = nullptr;
		}
	}
};

// Process-level singletons — loaded once, ref-counted.
static DynDbus g_dbus;
static DynPW g_pw;
static std::atomic<int> g_dbus_refs{ 0 };
static std::atomic<int> g_pw_refs{ 0 };

static bool acquire_dbus(std::string &err) {
	if (g_dbus_refs.fetch_add(1) == 0) {
		if (!g_dbus.load()) {
			g_dbus_refs.fetch_sub(1);
			err = "missing_dependency";
			return false;
		}
	}
	return true;
}

static void release_dbus() {
	if (g_dbus_refs.fetch_sub(1) == 1) {
		g_dbus.unload();
	}
}

static bool acquire_pw(std::string &err) {
	if (g_pw_refs.fetch_add(1) == 0) {
		if (!g_pw.load()) {
			g_pw_refs.fetch_sub(1);
			err = "missing_dependency";
			return false;
		}
		g_pw.pw_init(nullptr, nullptr);
	}
	return true;
}

static void release_pw() {
	if (g_pw_refs.fetch_sub(1) == 1) {
		g_pw.pw_deinit();
		g_pw.unload();
	}
}

// ============================================================
// Section 2 — D-Bus a{sv} dict helpers
// ============================================================

static void dict_begin(DBusMessageIter *args, DBusMessageIter *dict) {
	g_dbus.dbus_message_iter_open_container(args, DBUS_TYPE_ARRAY, "{sv}", dict);
}

static void dict_end(DBusMessageIter *args, DBusMessageIter *dict) {
	g_dbus.dbus_message_iter_close_container(args, dict);
}

static void dict_add_string(DBusMessageIter *dict,
		const char *key, const char *val) {
	DBusMessageIter entry, variant;
	g_dbus.dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
	g_dbus.dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	g_dbus.dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
	g_dbus.dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
	g_dbus.dbus_message_iter_close_container(&entry, &variant);
	g_dbus.dbus_message_iter_close_container(dict, &entry);
}

static void dict_add_uint32(DBusMessageIter *dict,
		const char *key, dbus_uint32_t val) {
	DBusMessageIter entry, variant;
	g_dbus.dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
	g_dbus.dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	g_dbus.dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
	g_dbus.dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &val);
	g_dbus.dbus_message_iter_close_container(&entry, &variant);
	g_dbus.dbus_message_iter_close_container(dict, &entry);
}

static void dict_add_bool(DBusMessageIter *dict,
		const char *key, dbus_bool_t val) {
	DBusMessageIter entry, variant;
	g_dbus.dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
	g_dbus.dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	g_dbus.dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
	g_dbus.dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
	g_dbus.dbus_message_iter_close_container(&entry, &variant);
	g_dbus.dbus_message_iter_close_container(dict, &entry);
}

// Generate a unique token string for portal handle_token / session_handle_token.
static std::string make_token() {
	static std::atomic<int> seq{ 0 };
	char buf[64];
	snprintf(buf, sizeof(buf), "godot_cap_%d_%d",
			static_cast<int>(getpid()), seq.fetch_add(1));
	return buf;
}

// ============================================================
// Section 3 — Portal response signal waiting
// ============================================================

// Pump the D-Bus connection until a Response signal arrives on request_path
// or the timeout expires.  Fills out_response and returns the message (caller
// must dbus_message_unref it).  Returns nullptr on timeout.
static DBusMessage *wait_for_response(DBusConnection *conn,
		const char *request_path, int timeout_ms, dbus_uint32_t &out_response) {
	auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeout_ms);

	while (std::chrono::steady_clock::now() < deadline) {
		g_dbus.dbus_connection_read_write(conn, 50);
		DBusMessage *msg = g_dbus.dbus_connection_pop_message(conn);
		if (!msg) {
			continue;
		}
		bool is_response =
				g_dbus.dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL &&
				strcmp(g_dbus.dbus_message_get_member(msg), "Response") == 0 &&
				strcmp(g_dbus.dbus_message_get_path(msg), request_path) == 0;

		if (is_response) {
			DBusMessageIter iter;
			g_dbus.dbus_message_iter_init(msg, &iter);
			g_dbus.dbus_message_iter_get_basic(&iter, &out_response);
			return msg;
		}
		g_dbus.dbus_message_unref(msg);
	}
	return nullptr; // timeout
}

// ============================================================
// Section 4 — Portal setup (D-Bus flow)
// ============================================================

bool PipeWireCaptureBackend::_portal_setup(int monitor_index,
		bool capture_cursor, uint32_t &out_node_id, std::string &error_out) {
	DBusError err{};
	g_dbus.dbus_error_init(&err);

	DBusConnection *conn = g_dbus.dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!conn || g_dbus.dbus_error_is_set(&err)) {
		if (g_dbus.dbus_error_is_set(&err)) {
			g_dbus.dbus_error_free(&err);
		}
		error_out = "dbus_session_unavailable";
		return false;
	}
	g_dbus.dbus_connection_flush(conn);

	// Add a broad match rule for all portal Request Response signals on this
	// connection.  We filter by path in wait_for_response().
	g_dbus.dbus_bus_add_match(conn,
			"type='signal',"
			"interface='org.freedesktop.portal.Request',"
			"member='Response'",
			&err);
	g_dbus.dbus_connection_flush(conn);
	if (g_dbus.dbus_error_is_set(&err)) {
		g_dbus.dbus_error_free(&err);
		g_dbus.dbus_connection_unref(conn);
		error_out = "dbus_add_match_failed";
		return false;
	}

	std::string session_handle;

	// ---- Step 1: CreateSession ----
	{
		std::string session_token = make_token();

		DBusMessage *call = g_dbus.dbus_message_new_method_call(
				"org.freedesktop.portal.Desktop",
				"/org/freedesktop/portal/desktop",
				"org.freedesktop.portal.ScreenCast",
				"CreateSession");

		DBusMessageIter args, dict;
		g_dbus.dbus_message_iter_init_append(call, &args);
		dict_begin(&args, &dict);
		dict_add_string(&dict, "session_handle_token", session_token.c_str());
		dict_end(&args, &dict);

		DBusMessage *reply = g_dbus.dbus_connection_send_with_reply_and_block(
				conn, call, 10000, &err);
		g_dbus.dbus_message_unref(call);

		if (!reply || g_dbus.dbus_error_is_set(&err)) {
			if (g_dbus.dbus_error_is_set(&err)) {
				g_dbus.dbus_error_free(&err);
			}
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_create_session_failed";
			return false;
		}

		// Reply arg 0: object path of the Request object.
		DBusMessageIter iter;
		g_dbus.dbus_message_iter_init(reply, &iter);
		const char *request_path_raw = nullptr;
		g_dbus.dbus_message_iter_get_basic(&iter, &request_path_raw);
		std::string request_path = request_path_raw ? request_path_raw : "";
		g_dbus.dbus_message_unref(reply);

		if (request_path.empty()) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_create_session_failed";
			return false;
		}

		dbus_uint32_t response_code = 1;
		DBusMessage *response = wait_for_response(conn, request_path.c_str(),
				30000, response_code);
		if (!response) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_timeout";
			return false;
		}
		if (response_code != 0) {
			g_dbus.dbus_message_unref(response);
			g_dbus.dbus_connection_unref(conn);
			error_out = "permission_denied";
			return false;
		}

		// Parse results a{sv} for "session_handle".
		// Args: (u response_code, a{sv} results)
		DBusMessageIter res_iter;
		g_dbus.dbus_message_iter_init(response, &res_iter);
		g_dbus.dbus_message_iter_next(&res_iter); // skip response_code

		DBusMessageIter results_dict;
		g_dbus.dbus_message_iter_recurse(&res_iter, &results_dict);
		while (g_dbus.dbus_message_iter_get_arg_type(&results_dict) ==
				DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter entry;
			g_dbus.dbus_message_iter_recurse(&results_dict, &entry);
			const char *key = nullptr;
			g_dbus.dbus_message_iter_get_basic(&entry, &key);
			g_dbus.dbus_message_iter_next(&entry);
			if (key && strcmp(key, "session_handle") == 0) {
				DBusMessageIter v;
				g_dbus.dbus_message_iter_recurse(&entry, &v);
				const char *sh = nullptr;
				g_dbus.dbus_message_iter_get_basic(&v, &sh);
				if (sh) {
					session_handle = sh;
				}
			}
			g_dbus.dbus_message_iter_next(&results_dict);
		}
		g_dbus.dbus_message_unref(response);

		if (session_handle.empty()) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_no_session_handle";
			return false;
		}
	}

	// ---- Step 2: SelectSources ----
	{
		std::string handle_token = make_token();

		DBusMessage *call = g_dbus.dbus_message_new_method_call(
				"org.freedesktop.portal.Desktop",
				"/org/freedesktop/portal/desktop",
				"org.freedesktop.portal.ScreenCast",
				"SelectSources");

		DBusMessageIter args, dict;
		g_dbus.dbus_message_iter_init_append(call, &args);
		// arg 0: session handle (object path)
		const char *sh = session_handle.c_str();
		g_dbus.dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
		// arg 1: options a{sv}
		dict_begin(&args, &dict);
		dict_add_string(&dict, "handle_token", handle_token.c_str());
		dict_add_uint32(&dict, "types", 1u); // MONITOR
		dict_add_bool(&dict, "multiple", FALSE);
		// cursor_mode: 2=EMBEDDED (cursor composited into stream), 1=HIDDEN
		dict_add_uint32(&dict, "cursor_mode", capture_cursor ? 2u : 1u);
		dict_add_uint32(&dict, "persist_mode", 0u);
		dict_end(&args, &dict);

		DBusMessage *reply = g_dbus.dbus_connection_send_with_reply_and_block(
				conn, call, 10000, &err);
		g_dbus.dbus_message_unref(call);

		if (!reply || g_dbus.dbus_error_is_set(&err)) {
			if (g_dbus.dbus_error_is_set(&err)) {
				g_dbus.dbus_error_free(&err);
			}
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_select_sources_failed";
			return false;
		}

		DBusMessageIter iter;
		g_dbus.dbus_message_iter_init(reply, &iter);
		const char *request_path_raw = nullptr;
		g_dbus.dbus_message_iter_get_basic(&iter, &request_path_raw);
		std::string request_path = request_path_raw ? request_path_raw : "";
		g_dbus.dbus_message_unref(reply);

		dbus_uint32_t response_code = 1;
		DBusMessage *response = wait_for_response(conn, request_path.c_str(),
				30000, response_code);
		if (!response) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_timeout";
			return false;
		}
		g_dbus.dbus_message_unref(response);
		if (response_code != 0) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "permission_denied";
			return false;
		}
	}

	// ---- Step 3: Start (triggers compositor picker dialog) ----
	{
		std::string handle_token = make_token();

		DBusMessage *call = g_dbus.dbus_message_new_method_call(
				"org.freedesktop.portal.Desktop",
				"/org/freedesktop/portal/desktop",
				"org.freedesktop.portal.ScreenCast",
				"Start");

		DBusMessageIter args, dict;
		g_dbus.dbus_message_iter_init_append(call, &args);
		const char *sh = session_handle.c_str();
		g_dbus.dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
		const char *parent_window = "";
		g_dbus.dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent_window);
		dict_begin(&args, &dict);
		dict_add_string(&dict, "handle_token", handle_token.c_str());
		dict_end(&args, &dict);

		DBusMessage *reply = g_dbus.dbus_connection_send_with_reply_and_block(
				conn, call, 10000, &err);
		g_dbus.dbus_message_unref(call);

		if (!reply || g_dbus.dbus_error_is_set(&err)) {
			if (g_dbus.dbus_error_is_set(&err)) {
				g_dbus.dbus_error_free(&err);
			}
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_start_failed";
			return false;
		}

		DBusMessageIter iter;
		g_dbus.dbus_message_iter_init(reply, &iter);
		const char *request_path_raw = nullptr;
		g_dbus.dbus_message_iter_get_basic(&iter, &request_path_raw);
		std::string request_path = request_path_raw ? request_path_raw : "";
		g_dbus.dbus_message_unref(reply);

		// 120 s — user must interact with the picker.
		dbus_uint32_t response_code = 1;
		DBusMessage *response = wait_for_response(conn, request_path.c_str(),
				120000, response_code);
		if (!response) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_timeout";
			return false;
		}
		if (response_code != 0) {
			g_dbus.dbus_message_unref(response);
			g_dbus.dbus_connection_unref(conn);
			error_out = "permission_denied";
			return false;
		}

		// Parse results for "streams": a(ua{sv})
		// We take the node_id (uint32) from the first stream entry.
		bool found = false;
		DBusMessageIter res_iter;
		g_dbus.dbus_message_iter_init(response, &res_iter);
		g_dbus.dbus_message_iter_next(&res_iter); // skip response_code

		DBusMessageIter results_dict;
		g_dbus.dbus_message_iter_recurse(&res_iter, &results_dict);
		while (!found && g_dbus.dbus_message_iter_get_arg_type(&results_dict) ==
				DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter entry;
			g_dbus.dbus_message_iter_recurse(&results_dict, &entry);
			const char *key = nullptr;
			g_dbus.dbus_message_iter_get_basic(&entry, &key);
			g_dbus.dbus_message_iter_next(&entry);

			if (key && strcmp(key, "streams") == 0) {
				// variant → a(ua{sv})
				DBusMessageIter variant, streams_arr;
				g_dbus.dbus_message_iter_recurse(&entry, &variant);
				g_dbus.dbus_message_iter_recurse(&variant, &streams_arr);

				if (g_dbus.dbus_message_iter_get_arg_type(&streams_arr) ==
						DBUS_TYPE_STRUCT) {
					DBusMessageIter stream_struct;
					g_dbus.dbus_message_iter_recurse(&streams_arr, &stream_struct);
					dbus_uint32_t node_id = 0;
					g_dbus.dbus_message_iter_get_basic(&stream_struct, &node_id);
					out_node_id = node_id;
					found = true;
				}
			}
			g_dbus.dbus_message_iter_next(&results_dict);
		}
		g_dbus.dbus_message_unref(response);

		if (!found) {
			g_dbus.dbus_connection_unref(conn);
			error_out = "portal_no_streams";
			return false;
		}
	}

	// Keep the connection alive — the portal session is tied to it.
	_dbus_conn = static_cast<void *>(conn);
	return true;
}

// ============================================================
// Section 5 — PipeWire stream event callbacks
// ============================================================

void PipeWireCaptureBackend::_on_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	if (!param || id != SPA_PARAM_Format) {
		return;
	}
	auto *self = static_cast<PipeWireCaptureBackend *>(data);

	struct spa_video_info_raw info{};
	if (spa_format_video_raw_parse(param, &info) < 0) {
		return;
	}
	self->_frame_width.store(static_cast<int32_t>(info.size.width));
	self->_frame_height.store(static_cast<int32_t>(info.size.height));
}

void PipeWireCaptureBackend::_on_process(void *data) {
	auto *self = static_cast<PipeWireCaptureBackend *>(data);

	if (!self->_running.load()) {
		return;
	}

	// Frame rate throttle.
	auto now = std::chrono::steady_clock::now();
	if (self->_max_fps > 0) {
		int64_t min_interval_us = 1'000'000 / self->_max_fps;
		int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
				now - self->_last_frame_time)
									 .count();
		if (elapsed_us < min_interval_us) {
			// Dequeue and immediately re-queue so the server can reuse the buffer.
			struct pw_buffer *b = pw_stream_dequeue_buffer(self->_pw_stream);
			if (b) {
				pw_stream_queue_buffer(self->_pw_stream, b);
			}
			return;
		}
	}

	struct pw_buffer *pw_buf = pw_stream_dequeue_buffer(self->_pw_stream);
	if (!pw_buf) {
		return;
	}

	struct spa_buffer *spa_buf = pw_buf->buffer;
	struct spa_data *d = &spa_buf->datas[0];

	// Skip DMA-BUF — SHM path only in this implementation.
	if (d->type == SPA_DATA_DmaBuf) {
		pw_stream_queue_buffer(self->_pw_stream, pw_buf);
		return;
	}

	// MAP_BUFFERS flag ensures d->data is already mmap'd for MemFd,
	// or is a direct MemPtr for shared memory.
	if (!d->data || d->chunk->size == 0) {
		pw_stream_queue_buffer(self->_pw_stream, pw_buf);
		return;
	}

	const int32_t w = self->_frame_width.load();
	const int32_t h = self->_frame_height.load();
	if (w <= 0 || h <= 0) {
		pw_stream_queue_buffer(self->_pw_stream, pw_buf);
		return;
	}

	const uint8_t *src = static_cast<const uint8_t *>(d->data) + d->chunk->offset;
	const int32_t stride = (d->chunk->stride > 0) ? d->chunk->stride : w * 4;

	// Convert BGRA → RGBA.
	self->_rgba_buf.resize(static_cast<size_t>(w * h * 4));
	uint8_t *dst = self->_rgba_buf.data();
	for (int32_t row = 0; row < h; ++row) {
		const uint8_t *s = src + row * stride;
		uint8_t *d_row = dst + row * w * 4;
		for (int32_t col = 0; col < w; ++col) {
			d_row[0] = s[2]; // R
			d_row[1] = s[1]; // G
			d_row[2] = s[0]; // B
			d_row[3] = s[3]; // A
			s += 4;
			d_row += 4;
		}
	}

	pw_stream_queue_buffer(self->_pw_stream, pw_buf);
	self->_last_frame_time = now;

	if (self->_frame_callback) {
		self->_frame_callback(self->_rgba_buf.data(), w, h);
	}
}

// ============================================================
// Section 6 — PipeWire object setup
// ============================================================

bool PipeWireCaptureBackend::_pw_setup(uint32_t node_id,
		std::string &error_out) {
	_pw_loop = g_pw.pw_main_loop_new(nullptr);
	if (!_pw_loop) {
		error_out = "pw_loop_create_failed";
		return false;
	}

	_pw_context = g_pw.pw_context_new(
			g_pw.pw_main_loop_get_loop(_pw_loop), nullptr, 0);
	if (!_pw_context) {
		g_pw.pw_main_loop_destroy(_pw_loop);
		_pw_loop = nullptr;
		error_out = "pw_context_create_failed";
		return false;
	}

	_pw_core = g_pw.pw_context_connect(_pw_context, nullptr, 0);
	if (!_pw_core) {
		g_pw.pw_context_destroy(_pw_context);
		_pw_context = nullptr;
		g_pw.pw_main_loop_destroy(_pw_loop);
		_pw_loop = nullptr;
		error_out = "pw_connect_failed";
		return false;
	}

	// Stream properties — PW_KEY_TARGET_OBJECT steers connection to portal node.
	char node_id_str[32];
	snprintf(node_id_str, sizeof(node_id_str), "%u", node_id);
	struct pw_properties *props = g_pw.pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Screen",
			PW_KEY_TARGET_OBJECT, node_id_str,
			nullptr);

	_pw_stream = g_pw.pw_stream_new(_pw_core, "godot-desktop-capture", props);
	// pw_stream_new takes ownership of props; do not call pw_properties_free.
	if (!_pw_stream) {
		pw_core_disconnect(_pw_core); // static inline
		_pw_core = nullptr;
		g_pw.pw_context_destroy(_pw_context);
		_pw_context = nullptr;
		g_pw.pw_main_loop_destroy(_pw_loop);
		_pw_loop = nullptr;
		error_out = "pw_stream_create_failed";
		return false;
	}

	// Set up events struct (heap-allocated so it outlives _pw_setup's stack).
	_pw_events = new pw_stream_events();
	memset(_pw_events, 0, sizeof(*_pw_events));
	_pw_events->version = PW_VERSION_STREAM_EVENTS;
	_pw_events->param_changed = &PipeWireCaptureBackend::_on_param_changed;
	_pw_events->process = &PipeWireCaptureBackend::_on_process;

	_pw_listener = new spa_hook();
	memset(_pw_listener, 0, sizeof(*_pw_listener));
	pw_stream_add_listener(_pw_stream, _pw_listener, _pw_events, this); // inline

	// Build stream format params (SPA inline functions — no dlopen needed).
	uint8_t pod_buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
	struct spa_video_info_raw fmt{};
	fmt.format = SPA_VIDEO_FORMAT_BGRA;
	const struct spa_pod *params[1];
	params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &fmt);

	int ret = pw_stream_connect( // static inline
			_pw_stream,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			static_cast<pw_stream_flags>(
					PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
			params, 1);

	if (ret < 0) {
		g_pw.pw_stream_destroy(_pw_stream);
		_pw_stream = nullptr;
		delete _pw_events;
		_pw_events = nullptr;
		delete _pw_listener;
		_pw_listener = nullptr;
		pw_core_disconnect(_pw_core); // inline
		_pw_core = nullptr;
		g_pw.pw_context_destroy(_pw_context);
		_pw_context = nullptr;
		g_pw.pw_main_loop_destroy(_pw_loop);
		_pw_loop = nullptr;
		error_out = "pw_stream_connect_failed";
		return false;
	}

	return true;
}

// ============================================================
// Section 7 — PipeWire thread
// ============================================================

void PipeWireCaptureBackend::_pw_thread_func() {
	g_pw.pw_main_loop_run(_pw_loop); // blocks until pw_main_loop_quit

	// Teardown — runs on the PW thread after quit.
	if (_pw_stream) {
		pw_stream_disconnect(_pw_stream); // static inline
		g_pw.pw_stream_destroy(_pw_stream);
		_pw_stream = nullptr;
	}
	delete _pw_events;
	_pw_events = nullptr;
	delete _pw_listener;
	_pw_listener = nullptr;

	if (_pw_core) {
		pw_core_disconnect(_pw_core); // static inline
		_pw_core = nullptr;
	}
	if (_pw_context) {
		g_pw.pw_context_destroy(_pw_context);
		_pw_context = nullptr;
	}
	if (_pw_loop) {
		g_pw.pw_main_loop_destroy(_pw_loop);
		_pw_loop = nullptr;
	}
}

// ============================================================
// Section 8 — Public API
// ============================================================

PipeWireCaptureBackend::~PipeWireCaptureBackend() {
	stop();
}

bool PipeWireCaptureBackend::start(int monitor_index, bool capture_cursor,
		int max_fps, PWFrameCallback callback, std::string &error_out) {
	if (_running.load()) {
		stop();
	}

	_monitor_index = monitor_index;
	_capture_cursor_enabled = capture_cursor;
	_max_fps = std::max(max_fps, 1);
	_frame_callback = std::move(callback);

	// Load dynamic libraries.
	if (!acquire_dbus(error_out)) {
		return false;
	}
	if (!acquire_pw(error_out)) {
		release_dbus();
		return false;
	}

	// Portal setup — blocks until user completes the picker dialog.
	uint32_t node_id = 0;
	if (!_portal_setup(monitor_index, capture_cursor, node_id, error_out)) {
		release_pw();
		release_dbus();
		return false;
	}

	// PipeWire object setup.
	if (!_pw_setup(node_id, error_out)) {
		if (_dbus_conn) {
			g_dbus.dbus_connection_unref(static_cast<DBusConnection *>(_dbus_conn));
			_dbus_conn = nullptr;
		}
		release_pw();
		release_dbus();
		return false;
	}

	_running.store(true);
	_last_frame_time = std::chrono::steady_clock::now() -
			std::chrono::seconds(1); // allow first frame immediately
	_pw_thread = std::thread(&PipeWireCaptureBackend::_pw_thread_func, this);
	return true;
}

void PipeWireCaptureBackend::stop() {
	if (!_running.exchange(false)) {
		return;
	}

	// Wake the PipeWire main loop from the calling thread.
	if (_pw_loop) {
		g_pw.pw_main_loop_quit(_pw_loop);
	}
	if (_pw_thread.joinable()) {
		_pw_thread.join();
	}

	// Close D-Bus connection — terminates the portal session.
	if (_dbus_conn) {
		g_dbus.dbus_connection_unref(static_cast<DBusConnection *>(_dbus_conn));
		_dbus_conn = nullptr;
	}

	release_pw();
	release_dbus();
}

int PipeWireCaptureBackend::enumerate_monitor_count() {
	// Count DRM connectors with "connected" status via sysfs.
	// Works on both Wayland and X11 without requiring any library.
	glob_t gl{};
	int count = 0;
	if (glob("/sys/class/drm/card[0-9]*-*/status", GLOB_NOSORT, nullptr, &gl) == 0) {
		for (size_t i = 0; i < gl.gl_pathc; ++i) {
			int fd = open(gl.gl_pathv[i], O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				continue;
			}
			char buf[16]{};
			ssize_t n = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (n > 0 && strncmp(buf, "connected", 9) == 0 && buf[9] != 'e') {
				// "connected\n" but NOT "disconnected"
				++count;
			}
		}
		globfree(&gl);
	}
	return count > 0 ? count : 1;
}

bool PipeWireCaptureBackend::get_monitor_size(int /*index*/,
		int32_t &out_w, int32_t &out_h) {
	// The portal ScreenCast API does not expose monitor dimensions without an
	// active session.  Return (0, 0) — callers should check for this.
	out_w = out_h = 0;
	return false;
}

#endif // __linux__
