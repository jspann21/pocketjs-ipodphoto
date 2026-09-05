#![no_std]
#![feature(alloc_error_handler)]

extern crate alloc;

mod plan;

use alloc::borrow::Cow;
use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ptr;
use core::slice;

use pocketjs_core::{
    damage::{DamagePolicy, DamageRect, DamageTracker},
    package, raster, spec, Ui,
};

const WIDTH: u32 = 220;
const HEIGHT: u32 = 176;
const PIXELS: usize = WIDTH as usize * HEIGHT as usize;
const MAX_DAMAGE_REGIONS: usize = 8;

const BUTTON_SELECT: u32 = 1 << 0;
const BUTTON_RIGHT: u32 = 1 << 1;
const BUTTON_LEFT: u32 = 1 << 2;
const BUTTON_PLAY: u32 = 1 << 3;
const BUTTON_MENU: u32 = 1 << 4;

const POWER_FIREWIRE: u32 = 1 << 0;
const POWER_USB: u32 = 1 << 1;
const POWER_CHARGING: u32 = 1 << 2;
const POWER_ADC_VALID: u32 = 1 << 3;
const POWER_I2C_FAULT: u32 = 1 << 4;
const POWER_TELEMETRY_DISABLED: u32 = 1 << 5;
const POWER_ADC_RANGE_FAULT: u32 = 1 << 6;
const POWER_BATTERY_LOW: u32 = 1 << 8;
const POWER_BATTERY_CRITICAL: u32 = 1 << 9;

const PHOTO_BATTERY_CURVE: [u32; 11] = [
    3450, 3660, 3700, 3730, 3770, 3820, 3870, 3920, 4040, 4100, 4170,
];

extern "C" {
    fn pjs_heap_alloc(size: usize, alignment: usize) -> *mut u8;
    fn pjs_heap_realloc(pointer: *mut u8, size: usize, alignment: usize) -> *mut u8;
    fn pjs_heap_free(pointer: *mut u8);
    fn panic_code(reason: u32) -> !;
}

struct A1099Allocator;

unsafe impl GlobalAlloc for A1099Allocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        pjs_heap_alloc(layout.size(), layout.align())
    }

    unsafe fn dealloc(&self, pointer: *mut u8, _layout: Layout) {
        pjs_heap_free(pointer)
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        pjs_heap_realloc(pointer, new_size, layout.align())
    }
}

#[global_allocator]
static ALLOCATOR: A1099Allocator = A1099Allocator;

#[alloc_error_handler]
fn alloc_error(_layout: Layout) -> ! {
    unsafe { panic_code(0x52534f4d) } // RSOM
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { panic_code(0x5253504e) } // RSPN
}

#[repr(C)]
pub struct PjsCoreInput {
    buttons: u32,
    wheel_delta: i32,
    wheel_position: u32,
    wheel_touched: u32,
    hold: u32,
    battery_mv: u32,
    power_flags: u32,
    dropped_ticks: u32,
    cache_enabled: u32,
    last_frame_us: u32,
    runtime_status: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PjsCoreDamageRect {
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
}

#[repr(C)]
pub struct PjsCoreDamagePlan {
    count: u32,
    full_redraw: u32,
    area: u32,
    reserved: u32,
    regions: [PjsCoreDamageRect; MAX_DAMAGE_REGIONS],
}

impl Default for PjsCoreDamagePlan {
    fn default() -> Self {
        Self {
            count: 0,
            full_redraw: 0,
            area: 0,
            reserved: 0,
            regions: [PjsCoreDamageRect::default(); MAX_DAMAGE_REGIONS],
        }
    }
}

#[repr(C)]
pub struct PjsGuestPackage {
    javascript: *const u8,
    javascript_length: u32,
    pak: *const u8,
    pak_length: u32,
    plan: *const u8,
    plan_length: u32,
    package_hash_low: u32,
    package_hash_high: u32,
    variant_hash_low: u32,
    variant_hash_high: u32,
    #[cfg(feature = "portable-fs")]
    app_id: *const u8,
    #[cfg(feature = "portable-fs")]
    app_id_length: u32,
}

impl Default for PjsGuestPackage {
    fn default() -> Self {
        Self {
            javascript: ptr::null(),
            javascript_length: 0,
            pak: ptr::null(),
            pak_length: 0,
            plan: ptr::null(),
            plan_length: 0,
            package_hash_low: 0,
            package_hash_high: 0,
            variant_hash_low: 0,
            variant_hash_high: 0,
            #[cfg(feature = "portable-fs")]
            app_id: ptr::null(),
            #[cfg(feature = "portable-fs")]
            app_id_length: 0,
        }
    }
}

#[derive(Default)]
struct Nodes {
    heartbeat: i32,
    wheel_track: i32,
    wheel_marker: i32,
    hold: i32,
    battery_back: i32,
    battery_fill: i32,
    power_firewire: i32,
    power_usb: i32,
    power_charging: i32,
    runtime: i32,
    dropped: i32,
    buttons: [i32; 5],
}

struct BootDiagnosticNodes {
    panel: i32,
    label: i32,
}

struct CoreState {
    ui: Ui,
    nodes: Nodes,
    words: Vec<u32>,
    damage: DamageTracker<MAX_DAMAGE_REGIONS>,
    frame: u32,
    heartbeat_phase: u32,
    last_buttons: u32,
    last_wheel_position: u32,
    last_wheel_touched: u32,
    last_hold: u32,
    last_battery_mv: u32,
    last_power_flags: u32,
    last_cache_enabled: u32,
    last_runtime_status: u32,
    last_dropped_ticks: u32,
    last_frame_class: u32,
    boot_diagnostic: Option<BootDiagnosticNodes>,
    dirty: bool,
}

static mut STATE: *mut CoreState = ptr::null_mut();

#[no_mangle]
#[used]
pub static PJS_CORE_BACKEND_RUST: u32 = 0x5255_5354; /* RUST */

#[no_mangle]
pub extern "C" fn pjs_core_backend_marker() -> u32 {
    unsafe { core::ptr::read_volatile(&PJS_CORE_BACKEND_RUST) }
}

const fn abgr(red: u32, green: u32, blue: u32, alpha: u32) -> u32 {
    (red & 0xff) | ((green & 0xff) << 8) | ((blue & 0xff) << 16) | ((alpha & 0xff) << 24)
}

fn set(ui: &mut Ui, node: i32, prop: u8, value: f64) {
    ui.set_prop(node, prop, value);
}

fn view(ui: &mut Ui, x: f32, y: f32, width: f32, height: f32, color: u32) -> i32 {
    let node = ui.create_node(spec::NodeType::View as u8);
    if node == 0 {
        unsafe { panic_code(0x52534e44) } // RSND
    }
    set(
        ui,
        node,
        spec::prop::POS_TYPE,
        spec::PosType::Absolute as u8 as f64,
    );
    set(ui, node, spec::prop::INSET_L, x as f64);
    set(ui, node, spec::prop::INSET_T, y as f64);
    set(ui, node, spec::prop::WIDTH, width as f64);
    set(ui, node, spec::prop::HEIGHT, height as f64);
    set(ui, node, spec::prop::BG_COLOR, color as f64);
    ui.insert_before(spec::ROOT_ID, node, 0);
    node
}

fn set_color(ui: &mut Ui, node: i32, color: u32) {
    set(ui, node, spec::prop::BG_COLOR, color as f64);
}

fn photo_battery_percent(mv: u32) -> u32 {
    if mv <= PHOTO_BATTERY_CURVE[0] {
        return 0;
    }
    if mv >= PHOTO_BATTERY_CURVE[10] {
        return 100;
    }
    for index in 1..PHOTO_BATTERY_CURVE.len() {
        let high = PHOTO_BATTERY_CURVE[index];
        if mv > high {
            continue;
        }
        let low = PHOTO_BATTERY_CURVE[index - 1];
        let span = high - low;
        let offset = mv - low;
        return ((index as u32 - 1) * 10) + (offset * 10 + span / 2) / span;
    }
    100
}

/* HostOps text and property-batch payloads are borrowed only for the duration
 * of the C call. The core copies text into its retained tree and decodes every
 * batch record before returning, so QuickJS can release its temporary value as
 * soon as the operation completes. QuickJS may encode a lone UTF-16 surrogate
 * as invalid UTF-8, so replacement characters preserve the web/PSP behavior. */
unsafe fn host_text<'a>(bytes: *const u8, length: usize) -> Cow<'a, str> {
    if bytes.is_null() || length == 0 {
        Cow::Borrowed("")
    } else {
        String::from_utf8_lossy(slice::from_raw_parts(bytes, length))
    }
}

fn host_u64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes([
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
    ])
}

fn build_state() -> CoreState {
    let mut ui = Ui::new_with_raster_density(1);
    ui.set_viewport(WIDTH as f32, HEIGHT as f32);
    set_color(&mut ui, spec::ROOT_ID, abgr(8, 11, 18, 255));

    let nodes = if cfg!(feature = "production") {
        Nodes::default()
    } else {
    let top = view(&mut ui, 0.0, 0.0, 220.0, 16.0, abgr(24, 32, 48, 255));
    set(&mut ui, top, spec::prop::BORDER_WIDTH, 1.0);
    set(
        &mut ui,
        top,
        spec::prop::BORDER_COLOR,
        abgr(70, 90, 120, 255) as f64,
    );

    let heartbeat = view(&mut ui, 204.0, 4.0, 8.0, 8.0, abgr(255, 255, 255, 255));
    set(&mut ui, heartbeat, spec::prop::RADIUS, 2.0);

    let hold = view(&mut ui, 0.0, 48.0, 220.0, 8.0, abgr(230, 12, 32, 255));
    set(&mut ui, hold, spec::prop::OPACITY, 0.0);

    let wheel_track = view(&mut ui, 6.0, 105.0, 208.0, 5.0, abgr(46, 58, 74, 255));
    set(&mut ui, wheel_track, spec::prop::RADIUS, 2.0);
    let wheel_marker = view(&mut ui, 6.0, 100.0, 5.0, 15.0, abgr(240, 245, 255, 255));
    set(&mut ui, wheel_marker, spec::prop::RADIUS, 2.0);

    let battery_back = view(&mut ui, 7.0, 23.0, 102.0, 10.0, abgr(38, 45, 58, 255));
    let battery_fill = view(&mut ui, 9.0, 25.0, 1.0, 6.0, abgr(40, 220, 92, 255));
    set(&mut ui, battery_back, spec::prop::BORDER_WIDTH, 1.0);
    set(
        &mut ui,
        battery_back,
        spec::prop::BORDER_COLOR,
        abgr(100, 115, 138, 255) as f64,
    );

    let power_firewire = view(&mut ui, 124.0, 23.0, 16.0, 10.0, abgr(40, 45, 55, 255));
    let power_usb = view(&mut ui, 146.0, 23.0, 16.0, 10.0, abgr(40, 45, 55, 255));
    let runtime = view(&mut ui, 113.0, 24.0, 8.0, 8.0, abgr(40, 45, 55, 255));
    let power_charging = view(&mut ui, 168.0, 23.0, 16.0, 10.0, abgr(40, 45, 55, 255));
    let dropped = view(&mut ui, 190.0, 23.0, 16.0, 10.0, abgr(40, 45, 55, 255));
    set(&mut ui, runtime, spec::prop::RADIUS, 2.0);

    let mut buttons = [0; 5];
    let idle = abgr(45, 53, 67, 255);
    for (index, node) in buttons.iter_mut().enumerate() {
        *node = view(&mut ui, 12.0 + index as f32 * 41.0, 132.0, 32.0, 32.0, idle);
        set(&mut ui, *node, spec::prop::RADIUS, 5.0);
        set(&mut ui, *node, spec::prop::BORDER_WIDTH, 1.0);
        set(
            &mut ui,
            *node,
            spec::prop::BORDER_COLOR,
            abgr(88, 104, 132, 255) as f64,
        );
    }

        Nodes {
            heartbeat,
            wheel_track,
            wheel_marker,
            hold,
            battery_back,
            battery_fill,
            power_firewire,
            power_usb,
            power_charging,
            runtime,
            dropped,
            buttons,
        }
    };

    CoreState {
        ui,
        nodes,
        words: Vec::new(),
        damage: DamageTracker::new(),
        frame: 0,
        heartbeat_phase: u32::MAX,
        last_buttons: u32::MAX,
        last_wheel_position: u32::MAX,
        last_wheel_touched: u32::MAX,
        last_hold: u32::MAX,
        last_battery_mv: u32::MAX,
        last_power_flags: u32::MAX,
        last_cache_enabled: u32::MAX,
        last_runtime_status: u32::MAX,
        last_dropped_ticks: u32::MAX,
        last_frame_class: u32::MAX,
        boot_diagnostic: None,
        dirty: true,
    }
}

fn push_fixed_decimal(output: &mut String, value: u32, divisors: &[u32]) {
    let value = value.min(divisors[0] * 10 - 1);
    for divisor in divisors {
        output.push(char::from(b'0' + ((value / divisor) % 10) as u8));
    }
}

fn boot_diagnostic_text(
    source: u32,
    failure_stage: u32,
    failure_code: u32,
    sector_reads: u32,
) -> String {
    let mut output = String::with_capacity(20);
    output.push_str(match source {
        1 => "PEND",
        2 => "ACTV",
        3 => "LAST",
        4 => "APP",
        5 => "EMBD",
        6 => "CAT",
        _ => "FAIL",
    });
    if failure_stage == 0 {
        output.push_str(" OK");
    } else {
        output.push(' ');
        output.push(match failure_stage {
            1 => 'S',
            2 => 'P',
            3 => 'Q',
            4 => 'F',
            5 => 'I',
            6 => 'E',
            _ => 'X',
        });
        push_fixed_decimal(&mut output, failure_code, &[10, 1]);
    }
    output.push_str(" R");
    push_fixed_decimal(&mut output, sector_reads, &[10_000, 1_000, 100, 10, 1]);
    output
}

fn set_boot_diagnostic_text(state: &mut CoreState, text: &str, panel_color: u32) {
    if let Some(nodes) = state.boot_diagnostic.as_ref() {
        let panel = nodes.panel;
        let label = nodes.label;
        set_color(&mut state.ui, panel, panel_color);
        state.ui.set_text(label, &text);
        state.dirty = true;
        return;
    }

    /* The ordinary qualification app begins at y=16, leaving this single
     * fixed strip independent from its layout. It is created after the guest
     * mount, so it remains above the app without adding another status grid. */
    let panel = view(&mut state.ui, 86.0, 0.0, 134.0, 16.0, panel_color);
    set(&mut state.ui, panel, spec::prop::Z_INDEX, 32_760.0);
    let label = state.ui.create_node(spec::NodeType::Text as u8);
    if label == 0 {
        unsafe { panic_code(0x52534e44) } // RSND
    }
    set(
        &mut state.ui,
        label,
        spec::prop::POS_TYPE,
        spec::PosType::Absolute as u8 as f64,
    );
    set(&mut state.ui, label, spec::prop::INSET_L, 88.0);
    /* Slot 0 uses a 15 px-tall glyph cell. Keep its top on-screen: a 12 px
     * line box at y=0 centers the cell at -1.5 px, and the raster backend
     * intentionally drops glyph cells whose top-left is outside the screen. */
    set(&mut state.ui, label, spec::prop::INSET_T, 1.0);
    set(&mut state.ui, label, spec::prop::WIDTH, 130.0);
    set(&mut state.ui, label, spec::prop::HEIGHT, 15.0);
    set(
        &mut state.ui,
        label,
        spec::prop::TEXT_COLOR,
        abgr(255, 255, 255, 255) as f64,
    );
    set(&mut state.ui, label, spec::prop::FONT_SLOT, 0.0);
    set(&mut state.ui, label, spec::prop::LINE_HEIGHT, 15.0);
    set(&mut state.ui, label, spec::prop::Z_INDEX, 32_761.0);
    state.ui.set_text(label, &text);
    state.ui.insert_before(spec::ROOT_ID, label, 0);
    state.boot_diagnostic = Some(BootDiagnosticNodes { panel, label });
    state.dirty = true;
}

#[no_mangle]
pub extern "C" fn pjs_core_set_boot_diagnostic(
    source: u32,
    failure_stage: u32,
    failure_code: u32,
    sector_reads: u32,
) {
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return;
    };
    let text = boot_diagnostic_text(source, failure_stage, failure_code, sector_reads);
    let panel_color = match source {
        1..=4 => abgr(0, 92, 110, 255),
        5 if failure_stage == 0 => abgr(96, 38, 125, 255),
        5 => abgr(142, 75, 0, 255),
        _ => abgr(150, 20, 38, 255),
    };
    set_boot_diagnostic_text(state, &text, panel_color);
}

#[no_mangle]
pub extern "C" fn pjs_core_set_app_diagnostic(selected: u32, count: u32, sector_reads: u32) {
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return;
    };
    let count = count.clamp(1, 9);
    let selected = selected.min(count - 1);
    let mut text = String::with_capacity(18);
    text.push_str("APP ");
    text.push(char::from(b'1' + selected as u8));
    text.push('/');
    text.push(char::from(b'0' + count as u8));
    text.push_str(" R");
    push_fixed_decimal(&mut text, sector_reads, &[10_000, 1_000, 100, 10, 1]);
    set_boot_diagnostic_text(state, &text, abgr(0, 92, 110, 255));
}

#[no_mangle]
pub extern "C" fn pjs_core_set_persistence_diagnostic(
    mode: u32,
    slot: u32,
    generation: u32,
    error: u32,
) {
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return;
    };
    let mut text = String::with_capacity(16);
    text.push_str("PERS ");
    if mode >= 3 {
        text.push('E');
        push_fixed_decimal(&mut text, error, &[10, 1]);
        set_boot_diagnostic_text(state, &text, abgr(150, 20, 38, 255));
        return;
    }
    text.push(match mode {
        0 => 'L',
        1 => 'C',
        _ => 'A',
    });
    text.push(char::from(b'0' + slot.min(1) as u8));
    text.push_str(" G");
    push_fixed_decimal(&mut text, generation, &[10_000, 1_000, 100, 10, 1]);
    let color = match mode {
        0 => abgr(0, 92, 110, 255),
        1 => abgr(16, 112, 72, 255),
        _ => abgr(142, 75, 0, 255),
    };
    set_boot_diagnostic_text(state, &text, color);
}

#[no_mangle]
pub extern "C" fn pjs_core_set_lineage_diagnostic(
    mode: u32,
    source: u32,
    generation: u32,
    error: u32,
) {
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return;
    };
    let mut text = String::with_capacity(16);
    if mode >= 6 {
        text.push_str("LINE E");
        push_fixed_decimal(&mut text, error, &[10, 1]);
        set_boot_diagnostic_text(state, &text, abgr(150, 20, 38, 255));
        return;
    }
    text.push_str(match mode {
        0 => "ACPT ",
        1 => "ROLL ",
        2 => "KEEP ",
        3 => "CRSH ",
        4 => "RECV ",
        _ => "SAFE ",
    });
    text.push(match source {
        1 => 'P',
        2 => 'A',
        3 => 'L',
        4 => 'D',
        5 => 'E',
        6 => 'C',
        _ => 'X',
    });
    text.push_str(" G");
    push_fixed_decimal(&mut text, generation, &[10_000, 1_000, 100, 10, 1]);
    let color = match mode {
        0 => abgr(16, 112, 72, 255),
        1 => abgr(142, 75, 0, 255),
        2 => abgr(0, 92, 110, 255),
        3 => abgr(150, 20, 38, 255),
        4 => abgr(96, 42, 150, 255),
        _ => abgr(16, 112, 72, 255),
    };
    set_boot_diagnostic_text(state, &text, color);
}

#[no_mangle]
pub extern "C" fn pjs_core_set_kernel_diagnostic(mode: u32, error: u32) {
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return;
    };
    let (text, color) = match mode {
        0 => (String::from("PWR WAIT"), abgr(142, 75, 0, 255)),
        1 => (String::from("PWR BAT"), abgr(0, 92, 110, 255)),
        2 => (String::from("PWR USB"), abgr(16, 112, 72, 255)),
        3 => (String::from("PWR FIRE"), abgr(16, 112, 72, 255)),
        4 => (String::from("LCD WAKE"), abgr(16, 112, 72, 255)),
        5 => (String::from("SHDN READY"), abgr(16, 112, 72, 255)),
        6 => (String::from("DISK WAIT"), abgr(142, 75, 0, 255)),
        7 => (String::from("DISK GO"), abgr(96, 42, 150, 255)),
        10 => (String::from("WAKE USB"), abgr(16, 112, 72, 255)),
        11 => (String::from("WAKE FIRE"), abgr(16, 112, 72, 255)),
        13 => (String::from("SUSPEND"), abgr(142, 75, 0, 255)),
        14 => (String::from("RESUME"), abgr(16, 112, 72, 255)),
        15 => (String::from("CHG ONLY"), abgr(16, 112, 72, 255)),
        16 => (String::from("BAT LOW"), abgr(142, 75, 0, 255)),
        17 => (String::from("BAT CRIT"), abgr(150, 20, 38, 255)),
        18 => (String::from("DISK FLUSH"), abgr(142, 75, 0, 255)),
        19 => (String::from("BAT OFF"), abgr(150, 20, 38, 255)),
        20 => (String::from("DISK OFF"), abgr(96, 42, 150, 255)),
        21 => (String::from("SUSP EXT"), abgr(142, 75, 0, 255)),
        22 => (String::from("AUD READY"), abgr(16, 112, 72, 255)),
        23 => (String::from("AUD TONE"), abgr(96, 42, 150, 255)),
        24 => (String::from("AUD OFF"), abgr(0, 92, 110, 255)),
        25 => {
            let mut output = String::from("AUD E");
            push_fixed_decimal(&mut output, error, &[10, 1]);
            (output, abgr(150, 20, 38, 255))
        }
        27 => (String::from("STR PLAY"), abgr(16, 112, 72, 255)),
        28 => (String::from("STR PAUSE"), abgr(142, 75, 0, 255)),
        29 => (String::from("STR GAP"), abgr(142, 75, 0, 255)),
        30 => (String::from("STR DRAIN"), abgr(0, 92, 110, 255)),
        31 => (String::from("STR PASS"), abgr(16, 112, 72, 255)),
        32 => (String::from("STR STOP"), abgr(0, 92, 110, 255)),
        33 => {
            let mut output = String::from("STR E");
            push_fixed_decimal(&mut output, error, &[10, 1]);
            (output, abgr(150, 20, 38, 255))
        }
        34 => (String::from("44K MONO"), abgr(16, 112, 72, 255)),
        35 => (String::from("44K STER"), abgr(16, 112, 72, 255)),
        36 => (String::from("22K MONO"), abgr(16, 112, 72, 255)),
        37 => (String::from("22K STER"), abgr(16, 112, 72, 255)),
        38 => (String::from("11K MONO"), abgr(16, 112, 72, 255)),
        39 => (String::from("11K STER"), abgr(16, 112, 72, 255)),
        40 => (String::from("MIX FOUR"), abgr(16, 112, 72, 255)),
        41 => (String::from("MIX PAUSE"), abgr(142, 75, 0, 255)),
        42 => (String::from("MIX GAP"), abgr(142, 75, 0, 255)),
        43 => (String::from("MIX DRAIN"), abgr(0, 92, 110, 255)),
        44 => (String::from("MIX PASS"), abgr(16, 112, 72, 255)),
        45 => (String::from("MIX STOP"), abgr(0, 92, 110, 255)),
        47 => (String::from("MIX PRIME"), abgr(142, 75, 0, 255)),
        46 => {
            let mut output = String::from("MIX E");
            push_fixed_decimal(&mut output, error, &[10, 1]);
            (output, abgr(150, 20, 38, 255))
        }
        _ => {
            let mut output = String::from("KERN E");
            push_fixed_decimal(&mut output, error, &[10, 1]);
            (output, abgr(150, 20, 38, 255))
        }
    };
    set_boot_diagnostic_text(state, &text, color);
}

#[no_mangle]
pub extern "C" fn pjs_core_init() -> i32 {
    unsafe {
        if !STATE.is_null() {
            return 0;
        }
        STATE = Box::into_raw(Box::new(build_state()));
    }
    0
}

#[no_mangle]
pub extern "C" fn pjs_core_step(input: *const PjsCoreInput) -> i32 {
    if input.is_null() {
        return -1;
    }
    let state = unsafe { STATE.as_mut() };
    let Some(state) = state else { return -2 };
    let input = unsafe { &*input };
    let mut changed = false;

    state.frame = state.frame.wrapping_add(1);
    if cfg!(feature = "production") {
        // Ordinary apps own the display. Keep animation/lifecycle ticking,
        // without changing hidden probe nodes and forcing diagnostic damage.
        state.dirty |= state.ui.has_time_driven_output();
        state.ui.tick();
        return 0;
    }
    let heartbeat_phase = (state.frame / 30) & 1;
    if heartbeat_phase != state.heartbeat_phase {
        let pulse = if heartbeat_phase == 0 { 1.0 } else { 0.45 };
        set(
            &mut state.ui,
            state.nodes.heartbeat,
            spec::prop::OPACITY,
            pulse,
        );
        state.heartbeat_phase = heartbeat_phase;
        changed = true;
    }

    let wheel_position = input.wheel_position.min(95);
    if wheel_position != state.last_wheel_position {
        let wheel_x = 6.0 + wheel_position as f32 * 203.0 / 95.0;
        set(
            &mut state.ui,
            state.nodes.wheel_marker,
            spec::prop::INSET_L,
            wheel_x as f64,
        );
        state.last_wheel_position = wheel_position;
        changed = true;
    }
    if input.wheel_touched != state.last_wheel_touched {
        set_color(
            &mut state.ui,
            state.nodes.wheel_marker,
            if input.wheel_touched != 0 {
                abgr(0, 220, 255, 255)
            } else {
                abgr(245, 248, 255, 255)
            },
        );
        state.last_wheel_touched = input.wheel_touched;
        changed = true;
    }
    if input.hold != state.last_hold {
        set(
            &mut state.ui,
            state.nodes.hold,
            spec::prop::OPACITY,
            if input.hold != 0 { 0.92 } else { 0.0 },
        );
        state.last_hold = input.hold;
        changed = true;
    }

    if input.buttons != state.last_buttons {
        let bits = [
            BUTTON_MENU,
            BUTTON_LEFT,
            BUTTON_SELECT,
            BUTTON_RIGHT,
            BUTTON_PLAY,
        ];
        let active = [
            abgr(235, 30, 50, 255),
            abgr(0, 205, 255, 255),
            abgr(30, 230, 90, 255),
            abgr(0, 205, 255, 255),
            abgr(255, 215, 35, 255),
        ];
        for index in 0..5 {
            set_color(
                &mut state.ui,
                state.nodes.buttons[index],
                if input.buttons & bits[index] != 0 {
                    active[index]
                } else {
                    abgr(45, 53, 67, 255)
                },
            );
        }
        state.last_buttons = input.buttons;
        changed = true;
    }

    if input.battery_mv != state.last_battery_mv || input.power_flags != state.last_power_flags {
        let telemetry_disabled = input.power_flags & POWER_TELEMETRY_DISABLED != 0;
        let telemetry_valid = input.power_flags & POWER_ADC_VALID != 0;
        let telemetry_fault = input.power_flags & POWER_I2C_FAULT != 0;
        let telemetry_range_fault = input.power_flags & POWER_ADC_RANGE_FAULT != 0;
        if telemetry_disabled {
            set(
                &mut state.ui,
                state.nodes.battery_fill,
                spec::prop::WIDTH,
                58.0,
            );
            set_color(
                &mut state.ui,
                state.nodes.battery_fill,
                abgr(145, 100, 255, 255),
            );
        } else if telemetry_valid {
            let mv = input.battery_mv.clamp(3200, 4200);
            let percent = photo_battery_percent(mv);
            let battery_width = 1.0 + percent as f32 * 96.0 / 100.0;
            set(
                &mut state.ui,
                state.nodes.battery_fill,
                spec::prop::WIDTH,
                battery_width as f64,
            );
            set_color(
                &mut state.ui,
                state.nodes.battery_fill,
                if input.power_flags & POWER_BATTERY_CRITICAL != 0 {
                    abgr(235, 30, 50, 255)
                } else if input.power_flags & POWER_BATTERY_LOW != 0 {
                    abgr(255, 145, 35, 255)
                } else if percent < 30 {
                    abgr(255, 205, 30, 255)
                } else {
                    abgr(35, 225, 92, 255)
                },
            );
        } else {
            set(
                &mut state.ui,
                state.nodes.battery_fill,
                spec::prop::WIDTH,
                96.0,
            );
            set_color(
                &mut state.ui,
                state.nodes.battery_fill,
                if telemetry_fault {
                    abgr(255, 45, 70, 255)
                } else if telemetry_range_fault {
                    abgr(255, 145, 35, 255)
                } else {
                    abgr(100, 110, 130, 255)
                },
            );
        }

        let off = abgr(40, 45, 55, 255);
        set_color(
            &mut state.ui,
            state.nodes.power_usb,
            if input.power_flags & POWER_USB != 0 {
                abgr(40, 150, 255, 255)
            } else {
                off
            },
        );
        set_color(
            &mut state.ui,
            state.nodes.power_charging,
            if telemetry_disabled {
                abgr(145, 100, 255, 255)
            } else if telemetry_fault {
                abgr(255, 45, 70, 255)
            } else if telemetry_range_fault {
                abgr(255, 145, 35, 255)
            } else if input.power_flags & POWER_CHARGING != 0 {
                abgr(40, 240, 100, 255)
            } else {
                off
            },
        );
        state.last_battery_mv = input.battery_mv;
        state.last_power_flags = input.power_flags;
        changed = true;
    }

    if input.cache_enabled != state.last_cache_enabled {
        set_color(
            &mut state.ui,
            state.nodes.power_firewire,
            if input.cache_enabled != 0 {
                abgr(40, 230, 100, 255)
            } else {
                abgr(255, 145, 35, 255)
            },
        );
        state.last_cache_enabled = input.cache_enabled;
        changed = true;
    }

    if input.runtime_status != state.last_runtime_status {
        let color = match input.runtime_status {
            1 => abgr(255, 215, 35, 255),
            2 => abgr(238, 76, 255, 255),
            3 => abgr(255, 40, 65, 255),
            4 => abgr(0, 222, 255, 255),
            5 => abgr(255, 145, 35, 255),
            _ => abgr(40, 45, 55, 255),
        };
        set_color(&mut state.ui, state.nodes.runtime, color);
        state.last_runtime_status = input.runtime_status;
        changed = true;
    }

    let frame_class = if input.dropped_ticks != 0 {
        4
    } else if input.last_frame_us == 0 {
        0
    } else if input.last_frame_us < 100_000 {
        1
    } else if input.last_frame_us < 300_000 {
        2
    } else {
        3
    };
    if input.dropped_ticks != state.last_dropped_ticks || frame_class != state.last_frame_class {
        let color = match frame_class {
            1 => abgr(40, 230, 100, 255),
            2 => abgr(255, 215, 35, 255),
            3 => abgr(255, 145, 35, 255),
            4 => abgr(255, 30, 60, 255),
            _ => abgr(40, 45, 55, 255),
        };
        set_color(&mut state.ui, state.nodes.dropped, color);
        state.last_dropped_ticks = input.dropped_ticks;
        state.last_frame_class = frame_class;
        changed = true;
    }

    state.ui.tick();
    state.dirty |= changed;
    0
}

fn swap_rendered_region(framebuffer: &mut [u16], region: DamageRect) {
    let x0 = region.x0.max(0).min(WIDTH as i32) as usize;
    let y0 = region.y0.max(0).min(HEIGHT as i32) as usize;
    let x1 = region.x1.max(0).min(WIDTH as i32) as usize;
    let y1 = region.y1.max(0).min(HEIGHT as i32) as usize;
    for y in y0..y1 {
        let row = &mut framebuffer[y * WIDTH as usize + x0..y * WIDTH as usize + x1];
        for pixel in row {
            *pixel = pixel.swap_bytes();
        }
    }
}

fn lcd_aligned_region(region: DamageRect) -> PjsCoreDamageRect {
    let mut x0 = region.x0.max(0).min(WIDTH as i32);
    let mut x1 = region.x1.max(0).min(WIDTH as i32);
    let y0 = region.y0.max(0).min(HEIGHT as i32);
    let y1 = region.y1.max(0).min(HEIGHT as i32);
    x0 &= !1;
    x1 = (x1 + 1) & !1;
    if x1 > WIDTH as i32 {
        x1 = WIDTH as i32;
    }
    PjsCoreDamageRect { x0, y0, x1, y1 }
}

#[no_mangle]
pub extern "C" fn pjs_core_render_damage(
    pixels: *mut u16,
    pixel_count: u32,
    damage_out: *mut PjsCoreDamagePlan,
) -> i32 {
    if pixels.is_null() || damage_out.is_null() || pixel_count as usize != PIXELS {
        return -1;
    }
    let state = unsafe { STATE.as_mut() };
    let Some(state) = state else { return -2 };
    state.words.clear();
    state.words.extend_from_slice(&state.ui.draw().words);
    let framebuffer = unsafe { slice::from_raw_parts_mut(pixels, PIXELS) };
    #[cfg(not(feature = "cooperative-audio"))]
    let plan = match raster::render_scaled_rgb565_incremental(
        &state.ui,
        &state.words,
        framebuffer,
        1,
        &mut state.damage,
        DamagePolicy::new(80),
    ) {
        Ok(plan) => plan,
        Err(_) => return -3,
    };

    #[cfg(feature = "cooperative-audio")]
    let plan = {
        use pocketjs_core::damage::DamageTarget;
        extern "C" {
            fn pjs_audio_stream_gate_refill();
            fn pjs_audio_stream_gate_active() -> bool;
        }
        // Same retained damage plan and painter order, rendered in bounded
        // horizontal strips. Only native PCM code runs at these yield points:
        // it must never re-enter Rust UI state while this borrow is live.
        let target = DamageTarget::new(WIDTH, HEIGHT, 1, u64::from_be_bytes(*b"IPODR565"));
        let plan = match state
            .damage
            .prepare(&state.ui, &state.words, target)
            .and_then(|plan| plan.with_policy(DamagePolicy::new(80)))
        {
            Ok(plan) => plan,
            Err(_) => return -3,
        };
        // Without a PCM producer there is nothing to service between strips.
        // Render each damage region once instead of replaying the draw list
        // up to 22 times for a full-height frame.
        let strip_height = if unsafe { pjs_audio_stream_gate_active() } { 8 } else { HEIGHT as i32 };
        for &region in plan.regions() {
            let mut y = region.y0;
            while y < region.y1 {
                let end = (y + strip_height).min(region.y1);
                let strip = DamageRect::new(region.x0, y, region.x1, end);
                unsafe {
                    pjs_audio_stream_gate_refill();
                }
                raster::render_scaled_rgb565_regions(
                    &state.ui,
                    &state.words,
                    framebuffer,
                    1,
                    &[strip],
                );
                y = end;
            }
        }
        unsafe {
            pjs_audio_stream_gate_refill();
        }
        state.damage.commit(&state.ui, &state.words, target);
        plan
    };

    for &region in plan.regions() {
        swap_rendered_region(framebuffer, region);
    }

    let mut output = PjsCoreDamagePlan::default();
    output.count = plan.region_count() as u32;
    output.full_redraw = plan.is_full_redraw() as u32;
    output.area = plan.area().min(u32::MAX as u64) as u32;
    for (slot, &region) in output.regions.iter_mut().zip(plan.regions()) {
        *slot = lcd_aligned_region(region);
    }
    unsafe { ptr::write(damage_out, output) };
    state.dirty = false;
    0
}

#[no_mangle]
pub extern "C" fn pjs_core_needs_render() -> u32 {
    unsafe { STATE.as_ref().map_or(0, |state| state.dirty as u32) }
}

#[no_mangle]
pub extern "C" fn pjs_core_render(pixels: *mut u16, pixel_count: u32) -> i32 {
    let state = unsafe { STATE.as_mut() };
    let Some(state) = state else { return -2 };
    state.damage.invalidate();
    let mut damage = PjsCoreDamagePlan::default();
    pjs_core_render_damage(pixels, pixel_count, &mut damage)
}

#[no_mangle]
pub extern "C" fn pjs_core_frame() -> u32 {
    unsafe { STATE.as_ref().map_or(0, |state| state.frame) }
}

#[no_mangle]
pub extern "C" fn pjs_core_draw_words() -> u32 {
    unsafe { STATE.as_ref().map_or(0, |state| state.words.len() as u32) }
}

#[no_mangle]
pub extern "C" fn pjs_package_open_ipod_photo(
    bytes: *const u8,
    length: u32,
    guest_out: *mut PjsGuestPackage,
) -> i32 {
    if bytes.is_null() || guest_out.is_null() || length == 0 {
        return -1;
    }
    let bytes = unsafe { slice::from_raw_parts(bytes, length as usize) };
    let guest = match package::select_guest(bytes, "ipod-photo", 1, false) {
        Ok(guest) => guest,
        Err(_) => return -2,
    };
    #[cfg(all(feature = "portable-audio", not(feature = "portable-fs")))]
    let supported_features: &[&str] = &["input.buttons", "audio.pcm", "text.glyphs.baked"];
    #[cfg(all(feature = "portable-audio", feature = "portable-fs"))]
    let supported_features: &[&str] =
        &["input.buttons", "audio.pcm", "data.fs", "text.glyphs.baked"];
    #[cfg(all(not(feature = "portable-audio"), feature = "portable-fs"))]
    let supported_features: &[&str] = &["input.buttons", "data.fs", "text.glyphs.baked"];
    #[cfg(all(not(feature = "portable-audio"), not(feature = "portable-fs")))]
    let supported_features: &[&str] = &["input.buttons", "text.glyphs.baked"];
    let plan_contract = plan::FixedPlanContract {
        target: "ipod-photo",
        host_abi: 1,
        width: WIDTH,
        height: HEIGHT,
        presentation: "native",
        raster_density: 1,
        supported_features,
    };
    if let Err(error) = plan::validate_fixed_plan(guest.plan, &plan_contract) {
        return match error {
            plan::PlanError::Syntax => -40,
            plan::PlanError::DuplicateField => -41,
            plan::PlanError::MissingTarget => -42,
            plan::PlanError::MissingViewport => -43,
            plan::PlanError::MissingFeatures => -44,
            plan::PlanError::TargetMismatch => -45,
            plan::PlanError::HostAbiMismatch => -46,
            plan::PlanError::ViewportMismatch => -47,
            plan::PlanError::UnsupportedFeature => -48,
        };
    }
    if guest.js.is_empty()
        || guest.js.len() > u32::MAX as usize
        || guest.pak.len() > u32::MAX as usize
        || guest.plan.len() > u32::MAX as usize
    {
        return -3;
    }
    let result = PjsGuestPackage {
        javascript: guest.js.as_ptr(),
        javascript_length: guest.js.len() as u32,
        pak: guest.pak.as_ptr(),
        pak_length: guest.pak.len() as u32,
        plan: guest.plan.as_ptr(),
        plan_length: guest.plan.len() as u32,
        package_hash_low: guest.package_hash as u32,
        package_hash_high: (guest.package_hash >> 32) as u32,
        variant_hash_low: guest.variant_hash as u32,
        variant_hash_high: (guest.variant_hash >> 32) as u32,
        #[cfg(feature = "portable-fs")]
        app_id: guest.app_id.as_ptr(),
        #[cfg(feature = "portable-fs")]
        app_id_length: guest.app_id.len() as u32,
    };
    unsafe { ptr::write(guest_out, result) };
    0
}

#[no_mangle]
pub extern "C" fn pjs_ui_create_node(node_type: u32) -> i32 {
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return 0;
    };
    if node_type > u8::MAX as u32 {
        return 0;
    }
    let id = state.ui.create_node(node_type as u8);
    if id != 0 {
        state.dirty = true;
    }
    id
}

#[no_mangle]
pub extern "C" fn pjs_ui_destroy_node(id: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.destroy_node(id);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_insert_before(parent: i32, child: i32, anchor: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.insert_before(parent, child, anchor);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_remove_child(parent: i32, child: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.remove_child(parent, child);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_style(id: i32, style_id: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_style(id, style_id);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_prop(id: i32, prop: u32, value: f64) {
    if prop > u8::MAX as u32 {
        return;
    }
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_prop(id, prop as u8, value);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_prop_batch(bytes: *const u8, length: usize) {
    const RECORD_BYTES: usize = 3 * core::mem::size_of::<f64>();
    if bytes.is_null() || length < RECORD_BYTES {
        return;
    }
    let records = unsafe { slice::from_raw_parts(bytes, length - (length % RECORD_BYTES)) };
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return;
    };
    for record in records.chunks_exact(RECORD_BYTES) {
        let id = f64::from_bits(host_u64(&record[0..8])) as i32;
        let prop = f64::from_bits(host_u64(&record[8..16])) as u32;
        let value = f64::from_bits(host_u64(&record[16..24]));
        if prop <= u8::MAX as u32 {
            state.ui.set_prop(id, prop as u8, value);
        }
    }
    state.dirty = true;
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_text(id: i32, text: *const u8, length: usize) {
    let value = unsafe { host_text(text, length) };
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_text(id, &value);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_replace_text(id: i32, text: *const u8, length: usize) {
    let value = unsafe { host_text(text, length) };
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.replace_text(id, &value);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_upload_texture(
    bytes: *const u8,
    length: usize,
    width: u32,
    height: u32,
    pixel_storage: u32,
) -> i32 {
    let data = if bytes.is_null() || length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(bytes, length) }
    };
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return -1;
    };
    state.ui.upload_texture(data, width, height, pixel_storage)
}

#[no_mangle]
pub extern "C" fn pjs_ui_upload_img_entry(bytes: *const u8, length: usize) -> i32 {
    let data = if bytes.is_null() || length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(bytes, length) }
    };
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return -1;
    };
    state.ui.upload_img_entry(data)
}

#[no_mangle]
pub extern "C" fn pjs_ui_free_texture(handle: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.free_texture(handle);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_image(id: i32, texture: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_image(id, texture);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_sprite(id: i32, atlas: i32, frames: u32, columns: u32, step: u32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_sprite(id, atlas, frames, columns, step);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_animate(
    id: i32,
    prop: u32,
    to: f64,
    duration_ms: u32,
    easing: u32,
    delay_ms: u32,
) -> i32 {
    if prop > u8::MAX as u32 || easing > u8::MAX as u32 {
        return -1;
    }
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return -1;
    };
    let animation = state
        .ui
        .animate(id, prop as u8, to, duration_ms, easing as u8, delay_ms);
    if animation > 0 {
        state.dirty = true;
    }
    animation
}

#[no_mangle]
pub extern "C" fn pjs_ui_cancel_anim(animation_id: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.cancel_anim(animation_id);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_focus(id: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_focus(id);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_set_active(id: i32, active: i32) {
    if let Some(state) = unsafe { STATE.as_mut() } {
        state.ui.set_active(id, active != 0);
        state.dirty = true;
    }
}

#[no_mangle]
pub extern "C" fn pjs_ui_load_styles(bytes: *const u8, length: usize) -> i32 {
    let data = if bytes.is_null() || length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(bytes, length) }
    };
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return 0;
    };
    state.ui.load_styles(data) as i32
}

#[no_mangle]
pub extern "C" fn pjs_ui_load_font_atlas(bytes: *const u8, length: usize) -> i32 {
    let data = if bytes.is_null() || length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(bytes, length) }
    };
    let Some(state) = (unsafe { STATE.as_mut() }) else {
        return 0;
    };
    state.ui.load_font_atlas(data) as i32
}

#[no_mangle]
pub extern "C" fn pjs_ui_measure_text(text: *const u8, length: usize, font_slot: u32) -> f32 {
    let value = unsafe { host_text(text, length) };
    let Some(state) = (unsafe { STATE.as_ref() }) else {
        return 0.0;
    };
    state.ui.measure_text(&value, font_slot as u8)
}

#[no_mangle]
pub extern "C" fn pjs_core_shutdown() {
    unsafe {
        if !STATE.is_null() {
            drop(Box::from_raw(STATE));
            STATE = ptr::null_mut();
        }
    }
}
