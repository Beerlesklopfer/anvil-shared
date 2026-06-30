//! Anvil — plain-Rust IPC transport (iceoryx2 0.9) exporting the historical
//! stable C ABI (see `anvil_iox_wrapper.h`).
//!
//! This replaces the C++ `anvil_iox_wrapper.cpp` that sat on the iceoryx2
//! **C-FFI 0.8**. The generated PLC runtime (`forgeiec_plc`) now links
//! `libanvil.a` (`-lanvil`) instead of compiling the `.cpp` + linking
//! `iceoryx2_ffi_c`. Result: ONE iceoryx2 version (0.9) and ONE ABI across the
//! runtime, anvild and Hearth — no C++/FFI version drift on the shared bus.
//!
//! Wire format is byte-for-byte the same as the old C-FFI wrapper, so 0.9
//! peers connect:
//!   * `ipc::Service`, MessagingPattern PublishSubscribe
//!   * payload type details `"Anvil"` / `FixedSize` / size = `payload_size` /
//!     alignment 1  (built via the serde patch, mirroring Hearth's
//!     `anvil_payload_type`)
//!   * user-header `()` (the C-FFI set none → iox2 default)
//!
//! Thread-safety matches the C wrapper: handles are raw pointers owned by the
//! caller; iceoryx2 ports are used from the calling thread (the runtime drives
//! one node per process). No locking is added here.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Mutex;

use iceoryx2::prelude::*;
use iceoryx2::service::builder::{CustomHeaderMarker, CustomPayloadMarker};
use iceoryx2::service::ipc::Service as IpcService;
use iceoryx2::service::static_config::message_type_details::{TypeDetail, TypeVariant};

// ─── iceoryx2 0.9 type aliases (the "Anvil" wire contract) ──────────────────

type AnvilService = iceoryx2::service::port_factory::publish_subscribe::PortFactory<
    IpcService,
    [CustomPayloadMarker],
    CustomHeaderMarker,
>;
type AnvilPublisher = iceoryx2::port::publisher::Publisher<
    IpcService,
    [CustomPayloadMarker],
    CustomHeaderMarker,
>;
type AnvilSubscriber = iceoryx2::port::subscriber::Subscriber<
    IpcService,
    [CustomPayloadMarker],
    CustomHeaderMarker,
>;

// ─── Opaque handles (Box'd, handed to C as raw pointers) ────────────────────

/// Backs `anvil_node_t`.
pub struct NodeHandle {
    node: Node<IpcService>,
}

/// Backs `anvil_pub_t`. Keeps the `PortFactory` (service) alive next to the
/// publisher; `payload_size` is the registered FixedSize element size.
pub struct PubHandle {
    _service: AnvilService,
    publisher: AnvilPublisher,
    #[allow(dead_code)]
    payload_size: usize,
}

/// Backs `anvil_sub_t`.
pub struct SubHandle {
    _service: AnvilService,
    subscriber: AnvilSubscriber,
    payload_size: usize,
}

// ─── Debug + diagnostics state ──────────────────────────────────────────────

static ANVIL_DEBUG: AtomicI32 = AtomicI32::new(0);

#[inline]
fn debug() -> i32 {
    ANVIL_DEBUG.load(Ordering::Relaxed)
}

/// Topics that hit an incompatible-type open since process start (monotonic),
/// surfaced via `anvil_get_mismatch_topics`.
static MISMATCH_TOPICS: Mutex<Vec<String>> = Mutex::new(Vec::new());

fn note_mismatch(service_name: &str) {
    if let Ok(mut set) = MISMATCH_TOPICS.lock() {
        if !set.iter().any(|t| t == service_name) {
            set.push(service_name.to_string());
        }
    }
    eprintln!(
        "Anvil: ABI/type mismatch on '{service_name}' — another process uses a \
         DIFFERENT payload layout for the same topic. Rebuild + redeploy all \
         peers (runtime/anvild/hearth) so they share one iceoryx2 version. \
         Build-info: {}",
        build_info_str()
    );
}

// ─── Helpers ────────────────────────────────────────────────────────────────

/// Borrow a C string as an owned Rust `String` (lossy). `None` if NULL.
unsafe fn c_str(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    Some(CStr::from_ptr(p).to_string_lossy().into_owned())
}

/// The `"Anvil"` FixedSize payload `TypeDetail` for a given struct size.
///
/// iceoryx2 only connects pub↔sub when the payload type details match exactly
/// (type_name + variant + size ==; alignment <=). A native `[u8]` slice
/// (`Dynamic`/`"u8"`/size 1) would be rejected. `TypeDetail`'s fields are
/// `pub(crate)`, so — exactly like Hearth's `anvil_payload_type` — we take a
/// real `u8`/FixedSize detail (correct shape: alignment 1) and patch
/// `type_name` + `size` through serde.
fn anvil_payload_type(size: usize) -> TypeDetail {
    let template = TypeDetail::new::<u8>(TypeVariant::FixedSize);
    let patched = (|| -> Option<TypeDetail> {
        let mut v = serde_json::to_value(&template).ok()?;
        v["type_name"] = serde_json::Value::String("Anvil".to_string());
        v["size"] = serde_json::json!(size.max(1));
        serde_json::from_value(v).ok()
    })();
    patched.unwrap_or(template)
}

/// User-header `TypeDetail` = unit `()` — the C-FFI wrapper set no user header,
/// so iox2 registered the default `()`. Mirroring it keeps the service hash
/// identical to the runtime's historical services (restart-safe).
fn anvil_user_header_type() -> TypeDetail {
    TypeDetail::new::<()>(TypeVariant::FixedSize)
}

/// Open-or-create the `"Anvil"` pub-sub service for `service_name`/`payload_size`.
fn open_anvil_service(
    node: &Node<IpcService>,
    service_name: &str,
    payload_size: usize,
) -> Option<AnvilService> {
    let sname = match ServiceName::new(service_name) {
        Ok(s) => s,
        Err(e) => {
            if debug() >= 1 {
                eprintln!("Anvil: bad service name '{service_name}': {e:?}");
            }
            return None;
        }
    };
    let payload_type = anvil_payload_type(payload_size);
    let user_header_type = anvil_user_header_type();

    // SAFETY: custom type-detail override — the byte layout is fully described
    // by the FixedSize "Anvil" payload (size bytes) + () user-header. Same call
    // sequence as Hearth's proven bridge.
    let result = unsafe {
        node.service_builder(&sname)
            .publish_subscribe::<[CustomPayloadMarker]>()
            .user_header::<CustomHeaderMarker>()
            .__internal_set_payload_type_details(&payload_type)
            .__internal_set_user_header_type_details(&user_header_type)
            .open_or_create()
    };
    match result {
        Ok(service) => Some(service),
        Err(e) => {
            let msg = format!("{e:?}");
            if msg.contains("IncompatibleTypes") {
                note_mismatch(service_name);
            } else if debug() >= 1 {
                eprintln!("Anvil: open_or_create '{service_name}' failed: {msg}");
            }
            None
        }
    }
}

// ─── C ABI: always-defined (no iceoryx2 needed) ─────────────────────────────

/// Enable/disable diagnostic logging (0=silent, 1=errors, 2=verbose).
#[no_mangle]
pub extern "C" fn anvil_set_debug(level: c_int) {
    ANVIL_DEBUG.store(level, Ordering::Relaxed);
    // One knob: ANVIL_DEBUG also drives the iceoryx2 log level so a single
    // `--anvil-debug 2` surfaces iceoryx2's own `fail!` cause messages.
    set_log_level(iox_level_for_debug(level));
    if level > 0 {
        eprintln!("Anvil: debug level set to {level}");
    }
}

/// The Anvil IPC **ABI contract version**. Bump this BY HAND whenever the
/// on-the-wire topic layout or the C-ABI changes incompatibly — i.e. anything
/// that would make a peer built against an older value fail to share the bus:
/// the `"Anvil"` payload `TypeDetail` shape, the service-name → hash mapping,
/// or the signatures of the `anvil_*` functions below.
///
/// Unlike `anvil_wrapper_build_info` (a build-time *identity* string, useful
/// for logs) this is a deliberate *compatibility* number. The editor reports
/// it from the `libanvil.a` it bundles and compares against the value anvild
/// announces in the gRPC handshake — a divergence is surfaced proactively at
/// connect, before any topic actually collides at runtime.
///
/// MUST stay in lockstep with the `ANVIL_ABI_VERSION` macro in
/// `anvil_iox_wrapper.h` (the C/C++ consumers' source of truth).
pub const ANVIL_ABI_VERSION: u32 = 1;

/// The ABI contract version (see [`ANVIL_ABI_VERSION`]). Always defined — needs
/// no iceoryx2 — so even a transport-less peer can answer the handshake.
#[no_mangle]
pub extern "C" fn anvil_abi_version() -> u32 {
    ANVIL_ABI_VERSION
}

// ─── iceoryx2 log-level control (makes the InternalError black box visible) ──
//
// iceoryx2 logs the REAL cause of a NodeCreationFailure (e.g.
// DynamicStorageOpenError(VersionMismatch)) via its `fail!` macro — but at
// TRACE, while the default level is INFO, and the IOX2_LOG_LEVEL env var only
// takes effect once an application calls set_log_level_from_env_or_default().
// Neither anvild nor its peers did that, so the cause stayed invisible (see the
// anvil_local bring-up post-mortem). These functions let anvild flip the iceoryx2
// log level — at startup from the env, and live via the editor/MCP.

/// Map an Anvil debug level (0..=2, as used by ANVIL_DEBUG / `--anvil-debug`)
/// onto an iceoryx2 log level and apply it. One knob raises BOTH Anvil's own
/// diagnostics and iceoryx2's internal logging.
fn iox_level_for_debug(level: c_int) -> LogLevel {
    match level {
        i if i <= 0 => LogLevel::Warn,
        1 => LogLevel::Info,
        2 => LogLevel::Debug,
        _ => LogLevel::Trace,
    }
}

/// Apply the iceoryx2 log level from the `IOX2_LOG_LEVEL` env var (or iceoryx2's
/// default if unset). anvild calls this at startup so `IOX2_LOG_LEVEL=trace`
/// actually surfaces iceoryx2's `fail!` cause messages.
#[no_mangle]
pub extern "C" fn anvil_set_log_level_from_env() {
    set_log_level_from_env_or_default();
}

/// Set the iceoryx2 log level explicitly (0=Warn .. 4=Trace). Drives the live
/// editor/MCP control (`anvil_set_log_level`). Out-of-range clamps to Trace.
#[no_mangle]
pub extern "C" fn anvil_set_log_level(level: c_int) {
    let lvl = match level {
        i if i <= 0 => LogLevel::Warn,
        1 => LogLevel::Info,
        2 => LogLevel::Debug,
        3 => LogLevel::Trace,
        _ => LogLevel::Trace,
    };
    set_log_level(lvl);
}

fn build_info_str() -> &'static str {
    // Version + git sha (set via ANVIL_WRAPPER_GIT_SHA at build time, else
    // "unknown"). Two processes sharing a topic compare these to spot skew.
    concat!(
        "anvil-rs ",
        env!("CARGO_PKG_VERSION"),
        " git:",
    )
}

/// Build-info string (NUL-terminated, static) for cross-process skew checks.
#[no_mangle]
pub extern "C" fn anvil_wrapper_build_info() -> *const c_char {
    // Compose a 'static NUL-terminated buffer once.
    use std::sync::OnceLock;
    static BUF: OnceLock<std::ffi::CString> = OnceLock::new();
    let s = BUF.get_or_init(|| {
        let sha = option_env!("ANVIL_WRAPPER_GIT_SHA").unwrap_or("unknown");
        std::ffi::CString::new(format!("{}{sha}", build_info_str())).unwrap_or_default()
    });
    s.as_ptr()
}

/// Comma-separate the mismatch topics into `buf` (truncated to `buflen-1`).
/// Returns the number of distinct topics. `buf=NULL`/`buflen=0` → count only.
#[no_mangle]
pub extern "C" fn anvil_get_mismatch_topics(buf: *mut c_char, buflen: usize) -> c_int {
    let set = match MISMATCH_TOPICS.lock() {
        Ok(s) => s,
        Err(_) => return 0,
    };
    if !buf.is_null() && buflen > 0 {
        let joined = set.join(",");
        let bytes = joined.as_bytes();
        let n = bytes.len().min(buflen - 1);
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
    }
    set.len() as c_int
}

// ─── C ABI: node ────────────────────────────────────────────────────────────

/// Create an iceoryx2 node. Returns NULL on failure.
#[no_mangle]
pub extern "C" fn anvil_node_create(name: *const c_char) -> *mut NodeHandle {
    // Best-effort: reap stale resources of dead nodes so a recompiled PLC with
    // a different struct size does not collide with old services. iceoryx2 0.9
    // cleans dead nodes on node creation; the explicit pass below is a no-op if
    // unavailable.
    let nm = unsafe { c_str(name) }.unwrap_or_default();

    let mut builder = NodeBuilder::new();
    if !nm.is_empty() {
        if let Ok(node_name) = NodeName::new(&nm) {
            builder = builder.name(&node_name);
        }
    }
    match builder.create::<IpcService>() {
        Ok(node) => Box::into_raw(Box::new(NodeHandle { node })),
        Err(e) => {
            if debug() >= 1 {
                eprintln!("Anvil: node create failed: {e:?}");
            }
            ptr::null_mut()
        }
    }
}

/// Destroy a node and release its resources.
#[no_mangle]
pub extern "C" fn anvil_node_destroy(node: *mut NodeHandle) {
    if node.is_null() {
        return;
    }
    // Drop the node (releases its iceoryx2 resources).
    drop(unsafe { Box::from_raw(node) });
}

// ─── C ABI: publisher ───────────────────────────────────────────────────────

/// Create a publisher on `service_name` with the given max payload size.
#[no_mangle]
pub extern "C" fn anvil_pub_create(
    node: *mut NodeHandle,
    service_name: *const c_char,
    payload_size: usize,
) -> *mut PubHandle {
    if node.is_null() {
        return ptr::null_mut();
    }
    let node = unsafe { &(*node).node };
    let svc = match unsafe { c_str(service_name) } {
        Some(s) => s,
        None => return ptr::null_mut(),
    };
    let service = match open_anvil_service(node, &svc, payload_size) {
        Some(s) => s,
        None => return ptr::null_mut(),
    };
    let publisher = match service.publisher_builder().create() {
        Ok(p) => p,
        Err(e) => {
            if debug() >= 1 {
                eprintln!("Anvil: publisher '{svc}' failed: {e:?}");
            }
            return ptr::null_mut();
        }
    };
    if debug() >= 2 {
        eprintln!("Anvil: pub service '{svc}' payload_size={payload_size}");
    }
    Box::into_raw(Box::new(PubHandle {
        _service: service,
        publisher,
        payload_size,
    }))
}

/// Destroy a publisher.
#[no_mangle]
pub extern "C" fn anvil_pub_destroy(pub_: *mut PubHandle) {
    if pub_.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(pub_) });
}

/// Publish `size` bytes from `data`. Returns 0 on success, -1 on failure.
#[no_mangle]
pub extern "C" fn anvil_publish(pub_: *mut PubHandle, data: *const c_void, size: usize) -> c_int {
    if pub_.is_null() || data.is_null() || size == 0 {
        return -1;
    }
    let h = unsafe { &*pub_ };
    // Loan ONE element of the overridden "Anvil" type (= payload_size bytes),
    // copy the caller's bytes in, send. SAFETY: custom-payload API — the uninit
    // slice length is payload_size markers (1 byte each). Mirrors Hearth.
    let mut sample = match unsafe { h.publisher.loan_custom_payload(1) } {
        Ok(s) => s,
        Err(e) => {
            if debug() >= 1 {
                eprintln!("Anvil: loan failed: {e:?}");
            }
            return -1;
        }
    };
    let dst = sample.payload_mut();
    let n = size.min(dst.len());
    unsafe {
        ptr::copy_nonoverlapping(data as *const u8, dst.as_mut_ptr() as *mut u8, n);
    }
    let sample = unsafe { sample.assume_init() };
    match sample.send() {
        Ok(_) => 0,
        Err(e) => {
            if debug() >= 1 {
                eprintln!("Anvil: send failed: {e:?}");
            }
            -1
        }
    }
}

// ─── C ABI: subscriber ──────────────────────────────────────────────────────

/// Create a subscriber on `service_name` with the given max payload size.
#[no_mangle]
pub extern "C" fn anvil_sub_create(
    node: *mut NodeHandle,
    service_name: *const c_char,
    payload_size: usize,
) -> *mut SubHandle {
    if node.is_null() {
        return ptr::null_mut();
    }
    let node = unsafe { &(*node).node };
    let svc = match unsafe { c_str(service_name) } {
        Some(s) => s,
        None => return ptr::null_mut(),
    };
    let service = match open_anvil_service(node, &svc, payload_size) {
        Some(s) => s,
        None => return ptr::null_mut(),
    };
    let subscriber = match service.subscriber_builder().create() {
        Ok(s) => s,
        Err(e) => {
            if debug() >= 1 {
                eprintln!("Anvil: subscriber '{svc}' failed: {e:?}");
            }
            return ptr::null_mut();
        }
    };
    if debug() >= 2 {
        eprintln!("Anvil: sub service '{svc}' payload_size={payload_size}");
    }
    Box::into_raw(Box::new(SubHandle {
        _service: service,
        subscriber,
        payload_size,
    }))
}

/// Destroy a subscriber.
#[no_mangle]
pub extern "C" fn anvil_sub_destroy(sub: *mut SubHandle) {
    if sub.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(sub) });
}

/// Receive the next sample, copy up to `size` bytes into `buf`.
/// Returns 0 if a sample was received, -1 if none available.
#[no_mangle]
pub extern "C" fn anvil_receive(sub: *mut SubHandle, buf: *mut c_void, size: usize) -> c_int {
    if sub.is_null() || buf.is_null() || size == 0 {
        return -1;
    }
    let h = unsafe { &*sub };
    // SAFETY: custom-payload receive — for the FixedSize "Anvil" type the
    // publisher sends 1 element = payload_size bytes, so the marker slice length
    // IS the byte count. Mirrors Hearth's receive path.
    match unsafe { h.subscriber.receive_custom_payload() } {
        Ok(Some(sample)) => {
            let markers = sample.payload();
            let n = h.payload_size.min(size).min(markers.len());
            unsafe {
                ptr::copy_nonoverlapping(markers.as_ptr() as *const u8, buf as *mut u8, n);
            }
            0
        }
        _ => -1,
    }
}
