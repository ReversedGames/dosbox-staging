/*
 *  Copyright (C) 2022-2022  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "mouse.h"
#include "mouse_config.h"
#include "mouse_interfaces.h"

#include <algorithm>

#include "bios.h"
#include "bitops.h"
#include "byteorder.h"
#include "callback.h"
#include "checks.h"
#include "cpu.h"
#include "dos_inc.h"
#include "math_utils.h"
#include "pic.h"
#include "regs.h"

#include "../../ints/int10.h"

using namespace bit::literals;

CHECK_NARROWING();

// This file implements the DOS mouse driver interface,
// using host system events

// Reference:
// - Ralf Brown's Interrupt List
// - WHEELAPI.TXT, INT10.LST, and INT33.LST from CuteMouse driver
// - https://www.stanislavs.org/helppc/int_33.html
// - http://www2.ift.ulaval.ca/~marchand/ift17583/dosints.pdf

static constexpr uint8_t cursor_size_x   = 16;
static constexpr uint8_t cursor_size_y   = 16;
static constexpr uint16_t cursor_size_xy = cursor_size_x * cursor_size_y;

static constexpr uint8_t num_buttons = 3;

enum class MouseCursor : uint8_t { Software = 0, Hardware = 1, Text = 2 };

// This enum has to be compatible with mask in DOS driver function 0x0c
enum class MouseEventId : uint8_t {
	NotDosEvent    = 0,
	MouseHasMoved  = 1 << 0,
	PressedLeft    = 1 << 1,
	ReleasedLeft   = 1 << 2,
	PressedRight   = 1 << 3,
	ReleasedRight  = 1 << 4,
	PressedMiddle  = 1 << 5,
	ReleasedMiddle = 1 << 6,
	WheelHasMoved  = 1 << 0,
};

// These values represent 'hardware' state, not driver state

static MouseButtons12S buttons = 0;
static float pos_x             = 0.0f;
static float pos_y             = 0.0f;
static int8_t counter_w        = 0; // wheel counter
static bool is_mapped = false; // true = physical mouse is mapped to this interface
static bool raw_input = true; // true = no host mouse acceleration pre-applied

static bool rate_is_set     = false; // true = rate was set by DOS application
static uint16_t rate_hz     = 0;
static uint16_t min_rate_hz = 0;

// Data from mouse events which were already received,
// but not necessary visible to the application

static struct {
	// Mouse movement
	float x_rel    = 0.0f;
	float y_rel    = 0.0f;
	uint16_t x_abs = 0;
	uint16_t y_abs = 0;

	// Wheel movement
	int16_t w_rel = 0;

	void Reset()
	{
		x_rel = 0.0f;
		y_rel = 0.0f;
		w_rel = 0;
	}
} pending;

// Multiply by 6.0f to compensate for 'MOUSE_GetBallisticsCoeff', which uses
// 6 as intersection point (just like 2:1 scaling model from PS/2 specification)
constexpr float acceleration_multiplier = 6.0f;
static MouseSpeedCalculator speed_mickeys(
        acceleration_multiplier *mouse_predefined.acceleration_dos);

static struct { // DOS driver state

	// Structure containing (only!) data which should be
	// saved/restored during task switching

	// DANGER, WILL ROBINSON!
	//
	// This whole structure can be read or written from the guest side
	// via virtual DOS driver, functions 0x15 / 0x16 / 0x17.
	// Do not put here any array indices, pointers, or anything that
	// can crash the emulator if filled-in incorrectly, or that can
	// be used by malicious code to escape from emulation!

	bool enabled   = false; // TODO: make use of this
	bool wheel_api = false; // CuteMouse compatible wheel extension

	uint16_t times_pressed[num_buttons]   = {0};
	uint16_t times_released[num_buttons]  = {0};
	uint16_t last_released_x[num_buttons] = {0};
	uint16_t last_released_y[num_buttons] = {0};
	uint16_t last_pressed_x[num_buttons]  = {0};
	uint16_t last_pressed_y[num_buttons]  = {0};
	uint16_t last_wheel_moved_x           = 0;
	uint16_t last_wheel_moved_y           = 0;

	int16_t mickey_counter_x = 0;
	int16_t mickey_counter_y = 0;

	float mickey_delta_x = 0.0f;
	float mickey_delta_y = 0.0f;

	float mickeys_per_pixel_x = 0.0f;
	float mickeys_per_pixel_y = 0.0f;

	uint16_t double_speed_threshold = 0; // in mickeys/s

	uint16_t granularity_x = 0; // mask
	uint16_t granularity_y = 0;

	int16_t update_region_x[2] = {0};
	int16_t update_region_y[2] = {0};

	uint16_t language = 0; // language for driver messages, unused
	uint8_t mode      = 0;

	// sensitivity
	uint8_t sensitivity_x = 0;
	uint8_t sensitivity_y = 0;
	// TODO: find out what it is for (acceleration?), for now
	// just set it to default value on startup
	uint8_t unknown_01 = 50;

	float sensitivity_coeff_x = 0;
	float sensitivity_coeff_y = 0;

	// mouse position allowed range
	int16_t minpos_x = 0;
	int16_t maxpos_x = 0;
	int16_t minpos_y = 0;
	int16_t maxpos_y = 0;

	// mouse cursor
	uint8_t page       = 0; // cursor display page number
	bool inhibit_draw  = false;
	uint16_t hidden    = 0;
	uint16_t oldhidden = 0;
	int16_t clipx      = 0;
	int16_t clipy      = 0;
	int16_t hot_x      = 0; // cursor hot spot, horizontal
	int16_t hot_y      = 0; // cursor hot spot, vertical

	struct {
		bool enabled                 = false;
		uint16_t pos_x               = 0;
		uint16_t pos_y               = 0;
		uint8_t data[cursor_size_xy] = {0};

	} background = {};

	MouseCursor cursor_type = MouseCursor::Software;

	// cursor shape definition
	uint16_t text_and_mask                       = 0;
	uint16_t text_xor_mask                       = 0;
	bool user_screen_mask                        = false;
	bool user_cursor_mask                        = false;
	uint16_t user_def_screen_mask[cursor_size_x] = {0};
	uint16_t user_def_cursor_mask[cursor_size_y] = {0};

	// user callback
	uint16_t user_callback_mask    = 0;
	uint16_t user_callback_segment = 0;
	uint16_t user_callback_offset  = 0;

} state;

static RealPt user_callback;

// ***************************************************************************
// Common helper routines
// ***************************************************************************

static uint8_t signed_to_reg8(const int8_t x)
{
	if (x >= 0)
		return static_cast<uint8_t>(x);
	else
		// -1 for 0xff, -2 for 0xfe, etc.
		return static_cast<uint8_t>(0x100 + x);
}

static uint16_t signed_to_reg16(const int16_t x)
{
	if (x >= 0)
		return static_cast<uint16_t>(x);
	else
		// -1 for 0xffff, -2 for 0xfffe, etc.
		return static_cast<uint16_t>(0x10000 + x);
}

static int16_t reg_to_signed16(const uint16_t x)
{
	if (bit::is(x, b15))
		// 0xffff for -1, 0xfffe for -2, etc.
		return static_cast<int16_t>(x - 0x10000);
	else
		return static_cast<int16_t>(x);
}

static uint16_t get_pos_x()
{
	return static_cast<uint16_t>(std::lround(pos_x)) & state.granularity_x;
}

static uint16_t get_pos_y()
{
	return static_cast<uint16_t>(std::lround(pos_y)) & state.granularity_y;
}

// ***************************************************************************
// Data - default cursor/mask
// ***************************************************************************

static constexpr uint16_t default_text_and_mask = 0x77FF;
static constexpr uint16_t default_text_xor_mask = 0x7700;

static uint16_t default_screen_mask[cursor_size_y] = {
	0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
        0x003F, 0x001F, 0x01FF, 0x00FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF
};

static uint16_t default_cursor_mask[cursor_size_y] = {
	0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
        0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000
};

// ***************************************************************************
// Text mode cursor
// ***************************************************************************

// Write and read directly to the screen. Do no use int_setcursorpos (LOTUS123)
extern void WriteChar(uint16_t col, uint16_t row, uint8_t page, uint8_t chr,
                      uint8_t attr, bool useattr);
extern void ReadCharAttr(uint16_t col, uint16_t row, uint8_t page, uint16_t *result);

static void restore_cursor_background_text()
{
	if (state.hidden || state.inhibit_draw)
		return;

	if (state.background.enabled) {
		WriteChar(state.background.pos_x,
		          state.background.pos_y,
		          real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE),
		          state.background.data[0],
		          state.background.data[1],
		          true);
		state.background.enabled = false;
	}
}

static void draw_cursor_text()
{
	// Restore Background
	restore_cursor_background_text();

	// Check if cursor in update region
	auto x = get_pos_x();
	auto y = get_pos_y();
	if ((y <= state.update_region_y[1]) && (y >= state.update_region_y[0]) &&
	    (x <= state.update_region_x[1]) && (x >= state.update_region_x[0])) {
		return;
	}

	// Save Background
	state.background.pos_x = static_cast<uint16_t>(x / 8);
	state.background.pos_y = static_cast<uint16_t>(y / 8);
	if (state.mode < 2)
		state.background.pos_x = state.background.pos_x / 2;

	// use current page (CV program)
	uint8_t page = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE);

	if (state.cursor_type == MouseCursor::Software) {
		uint16_t result = 0;
		ReadCharAttr(state.background.pos_x,
		             state.background.pos_y,
		             page,
		             &result); // result is in native/host-endian format
		state.background.data[0] = read_low_byte(result);
		state.background.data[1] = read_high_byte(result);
		state.background.enabled = true;

		// Write Cursor
		result &= state.text_and_mask;
		result ^= state.text_xor_mask;

		WriteChar(state.background.pos_x,
		          state.background.pos_y,
		          page,
		          read_low_byte(result),
		          read_high_byte(result),
		          true);
	} else {
		uint16_t address = static_cast<uint16_t>(
		        page * real_readw(BIOSMEM_SEG, BIOSMEM_PAGE_SIZE));
		address = static_cast<uint16_t>(
		        address + (state.background.pos_y *
		                           real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS) +
		                   state.background.pos_x) *
		                          2);
		address /= 2;
		uint16_t cr = real_readw(BIOSMEM_SEG, BIOSMEM_CRTC_ADDRESS);
		IO_Write(cr, 0xe);
		IO_Write(static_cast<io_port_t>(cr + 1),
		         static_cast<uint8_t>((address >> 8) & 0xff));
		IO_Write(cr, 0xf);
		IO_Write(static_cast<io_port_t>(cr + 1),
		         static_cast<uint8_t>(address & 0xff));
	}
}

// ***************************************************************************
// Graphic mode cursor
// ***************************************************************************

static struct {
	uint8_t sequ_address    = 0;
	uint8_t sequ_data       = 0;
	uint8_t grdc_address[9] = {0};

} vga_regs;

static void save_vga_registers()
{
	if (IS_VGA_ARCH) {
		for (uint8_t i = 0; i < 9; i++) {
			IO_Write(VGAREG_GRDC_ADDRESS, i);
			vga_regs.grdc_address[i] = IO_Read(VGAREG_GRDC_DATA);
		}
		// Setup some default values in GFX regs that should work
		IO_Write(VGAREG_GRDC_ADDRESS, 3);
		IO_Write(VGAREG_GRDC_DATA, 0); // disable rotate and operation
		IO_Write(VGAREG_GRDC_ADDRESS, 5);
		IO_Write(VGAREG_GRDC_DATA,
		         vga_regs.grdc_address[5] & 0xf0); // Force read/write
		                                           // mode 0

		// Set Map to all planes. Celtic Tales
		vga_regs.sequ_address = IO_Read(VGAREG_SEQU_ADDRESS);
		IO_Write(VGAREG_SEQU_ADDRESS, 2);
		vga_regs.sequ_data = IO_Read(VGAREG_SEQU_DATA);
		IO_Write(VGAREG_SEQU_DATA, 0xF);
	} else if (machine == MCH_EGA) {
		// Set Map to all planes.
		IO_Write(VGAREG_SEQU_ADDRESS, 2);
		IO_Write(VGAREG_SEQU_DATA, 0xF);
	}
}

static void restore_vga_registers()
{
	if (IS_VGA_ARCH) {
		for (uint8_t i = 0; i < 9; i++) {
			IO_Write(VGAREG_GRDC_ADDRESS, i);
			IO_Write(VGAREG_GRDC_DATA, vga_regs.grdc_address[i]);
		}

		IO_Write(VGAREG_SEQU_ADDRESS, 2);
		IO_Write(VGAREG_SEQU_DATA, vga_regs.sequ_data);
		IO_Write(VGAREG_SEQU_ADDRESS, vga_regs.sequ_address);
	}
}

static void clip_cursor_area(int16_t &x1, int16_t &x2, int16_t &y1, int16_t &y2,
                             uint16_t &addx1, uint16_t &addx2, uint16_t &addy)
{
	addx1 = 0;
	addx2 = 0;
	addy  = 0;
	// Clip up
	if (y1 < 0) {
		addy = static_cast<uint16_t>(addy - y1);
		y1   = 0;
	}
	// Clip down
	if (y2 > state.clipy) {
		y2 = state.clipy;
	};
	// Clip left
	if (x1 < 0) {
		addx1 = static_cast<uint16_t>(addx1 - x1);
		x1    = 0;
	};
	// Clip right
	if (x2 > state.clipx) {
		addx2 = static_cast<uint16_t>(x2 - state.clipx);
		x2    = state.clipx;
	};
}

static void restore_cursor_background()
{
	if (state.hidden || state.inhibit_draw || !state.background.enabled)
		return;

	save_vga_registers();

	// Restore background
	uint16_t addx1, addx2, addy;
	uint16_t data_pos = 0;
	int16_t x1        = static_cast<int16_t>(state.background.pos_x);
	int16_t y1        = static_cast<int16_t>(state.background.pos_y);
	int16_t x2        = static_cast<int16_t>(x1 + cursor_size_x - 1);
	int16_t y2        = static_cast<int16_t>(y1 + cursor_size_y - 1);

	clip_cursor_area(x1, x2, y1, y2, addx1, addx2, addy);

	data_pos = static_cast<uint16_t>(addy * cursor_size_x);
	for (int16_t y = y1; y <= y2; y++) {
		data_pos = static_cast<uint16_t>(data_pos + addx1);
		for (int16_t x = x1; x <= x2; x++) {
			INT10_PutPixel(static_cast<uint16_t>(x),
			               static_cast<uint16_t>(y),
			               state.page,
			               state.background.data[data_pos++]);
		};
		data_pos = static_cast<uint16_t>(data_pos + addx2);
	};
	state.background.enabled = false;

	restore_vga_registers();
}

void MOUSEDOS_DrawCursor()
{
	if (state.hidden || state.inhibit_draw)
		return;
	INT10_SetCurMode();
	// In Textmode ?
	if (CurMode->type == M_TEXT) {
		draw_cursor_text();
		return;
	}

	// Check video page. Seems to be ignored for text mode.
	// hence the text mode handled above this
	// >>> removed because BIOS page is not actual page in some cases, e.g.
	// QQP games
	//    if (real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE) != state.page)
	//    return;

	// Check if cursor in update region
	/*    if ((get_pos_x() >= state.update_region_x[0]) && (get_pos_y() <=
	   state.update_region_x[1]) && (get_pos_y() >= state.update_region_y[0])
	   && (GETPOS_Y <= state.update_region_y[1])) { if
	   (CurMode->type==M_TEXT16) restore_cursor_background_text(); else
	            restore_cursor_background();
	        --mouse.shown;
	        return;
	    }
	   */ /*Not sure yet what to do update region should be set to ??? */

	// Get Clipping ranges

	state.clipx = static_cast<int16_t>((Bits)CurMode->swidth - 1); // Get from
	                                                               // BIOS?
	state.clipy = static_cast<int16_t>((Bits)CurMode->sheight - 1);

	// might be vidmode == 0x13?2:1
	int16_t xratio = 640;
	if (CurMode->swidth > 0)
		xratio = static_cast<int16_t>(xratio / CurMode->swidth);
	if (xratio == 0)
		xratio = 1;

	restore_cursor_background();

	save_vga_registers();

	// Save Background
	uint16_t addx1, addx2, addy;
	uint16_t data_pos = 0;
	int16_t x1 = static_cast<int16_t>(get_pos_x() / xratio - state.hot_x);
	int16_t y1 = static_cast<int16_t>(get_pos_y() - state.hot_y);
	int16_t x2 = static_cast<int16_t>(x1 + cursor_size_x - 1);
	int16_t y2 = static_cast<int16_t>(y1 + cursor_size_y - 1);

	clip_cursor_area(x1, x2, y1, y2, addx1, addx2, addy);

	data_pos = static_cast<uint16_t>(addy * cursor_size_x);
	for (int16_t y = y1; y <= y2; y++) {
		data_pos = static_cast<uint16_t>(data_pos + addx1);
		for (int16_t x = x1; x <= x2; x++) {
			INT10_GetPixel(static_cast<uint16_t>(x),
			               static_cast<uint16_t>(y),
			               state.page,
			               &state.background.data[data_pos++]);
		};
		data_pos = static_cast<uint16_t>(data_pos + addx2);
	};
	state.background.enabled = true;
	state.background.pos_x   = static_cast<uint16_t>(get_pos_x() / xratio -
                                                       state.hot_x);
	state.background.pos_y = static_cast<uint16_t>(get_pos_y() - state.hot_y);

	// Draw Mousecursor
	data_pos               = static_cast<uint16_t>(addy * cursor_size_x);
	const auto screen_mask = state.user_screen_mask ? state.user_def_screen_mask
	                                                : default_screen_mask;
	const auto cursor_mask = state.user_cursor_mask ? state.user_def_cursor_mask
	                                                : default_cursor_mask;
	for (int16_t y = y1; y <= y2; y++) {
		uint16_t sc_mask = screen_mask[addy + y - y1];
		uint16_t cu_mask = cursor_mask[addy + y - y1];
		if (addx1 > 0) {
			sc_mask  = static_cast<uint16_t>(sc_mask << addx1);
			cu_mask  = static_cast<uint16_t>(cu_mask << addx1);
			data_pos = static_cast<uint16_t>(data_pos + addx1);
		};
		for (int16_t x = x1; x <= x2; x++) {
			constexpr auto highest_bit = (1 << (cursor_size_x - 1));
			uint8_t pixel              = 0;
			// ScreenMask
			if (sc_mask & highest_bit)
				pixel = state.background.data[data_pos];
			// CursorMask
			if (cu_mask & highest_bit)
				pixel = pixel ^ 0x0f;
			sc_mask = static_cast<uint16_t>(sc_mask << 1);
			cu_mask = static_cast<uint16_t>(cu_mask << 1);
			// Set Pixel
			INT10_PutPixel(static_cast<uint16_t>(x),
			               static_cast<uint16_t>(y),
			               state.page,
			               pixel);
			++data_pos;
		};
		data_pos = static_cast<uint16_t>(addx2 + data_pos);
	};

	restore_vga_registers();
}

// ***************************************************************************
// DOS driver interface implementation
// ***************************************************************************

static void update_driver_active()
{
	mouse_shared.active_dos = (state.user_callback_mask != 0);
	MOUSE_NotifyStateChanged();
}

static uint8_t get_reset_wheel_8bit()
{
	if (!state.wheel_api)
		return 0;

	const auto tmp = counter_w;
	counter_w      = 0; // reading always clears the counter

	// 0xff for -1, 0xfe for -2, etc.
	return signed_to_reg8(tmp);
}

static uint16_t get_reset_wheel_16bit()
{
	if (!state.wheel_api)
		return 0;

	const int16_t tmp = counter_w;
	counter_w         = 0; // reading always clears the counter

	return signed_to_reg16(tmp);
}

static void set_mickey_pixel_rate(const int16_t ratio_x, const int16_t ratio_y)
{
	// According to https://www.stanislavs.org/helppc/int_33-f.html
	// the values should be non-negative (highest bit not set)

	if ((ratio_x > 0) && (ratio_y > 0)) {
		// ratio = number of mickeys per 8 pixels
		constexpr auto pixels     = 8.0f;
		state.mickeys_per_pixel_x = static_cast<float>(ratio_x) / pixels;
		state.mickeys_per_pixel_y = static_cast<float>(ratio_y) / pixels;
	}
}

static void set_double_speed_threshold(const uint16_t threshold)
{
	if (threshold)
		state.double_speed_threshold = threshold;
	else
		state.double_speed_threshold = 64; // default value
}

static void set_sensitivity(const uint16_t sensitivity_x,
                            const uint16_t sensitivity_y, const uint16_t unknown)
{
	const auto tmp_x = std::min(static_cast<uint16_t>(100), sensitivity_x);
	const auto tmp_y = std::min(static_cast<uint16_t>(100), sensitivity_y);
	const auto tmp_u = std::min(static_cast<uint16_t>(100), unknown);

	state.sensitivity_x = static_cast<uint8_t>(tmp_x);
	state.sensitivity_y = static_cast<uint8_t>(tmp_y);
	state.unknown_01    = static_cast<uint8_t>(tmp_u);

	// It is unclear how the original mouse driver handles sensitivity,
	// but one can observe that setting value 0 stops the mouse movement
	// completely, 50 is the default, and 100 seems to more or less
	// dobule it. Linear sensitivity should be good enough.

	state.sensitivity_coeff_x = state.sensitivity_x / 50.0f;
	state.sensitivity_coeff_y = state.sensitivity_y / 50.0f;
}

static void notify_interface_rate()
{
	// Real mouse drivers set the PS/2 mouse sampling rate
	// to the following rates:
	// - A4 Pointing Device 8.04A   100 Hz
	// - CuteMouse 2.1b4            100 Hz
	// - Genius Dynamic Mouse 9.20   60 Hz
	// - Microsoft Mouse 8.20        60 Hz
	// - Mouse Systems 8.00         100 Hz
	// and the most common serial mice were 1200 bauds, which gives
	// approx. 40 Hz sampling rate limit due to COM port bandwidth.

	// Original DOSBox uses 200 Hz for callbacks, but the internal
	// states (buttons, mickey counters) are updated in realtime.
	// This is too much (at least Ultima Underworld I and II do not
	// like this).

	// Set default value to 200 Hz (which is the maximum setting for
	// PS/2 mice - and hopefully this is safe (if it's not, user can
	// always adjust it in configuration file or with MOUSECTL.COM).

	constexpr uint16_t rate_default_hz = 200;

	if (rate_is_set)
		// Rate was set by guest application - use this value. The
		// minimum will be enforced by MouseInterface nevertheless
		MouseInterface::GetDOS()->NotifyInterfaceRate(rate_hz);
	else if (min_rate_hz)
		// If user set the minimum mouse rate - follow it
		MouseInterface::GetDOS()->NotifyInterfaceRate(min_rate_hz);
	else
		// No user setting in effect - use default value
		MouseInterface::GetDOS()->NotifyInterfaceRate(rate_default_hz);
}

static void set_interrupt_rate(const uint16_t rate_id)
{
	uint16_t val_hz;

	switch (rate_id) {
	case 0: val_hz = 0; break; // no events, TODO: this should be simulated
	case 1: val_hz = 30; break;
	case 2: val_hz = 50; break;
	case 3: val_hz = 100; break;
	default: val_hz = 200; break; // above 4 is not suported, set max
	}

	if (val_hz) {
		rate_is_set = true;
		rate_hz     = val_hz;
		notify_interface_rate();
	}
}

static void reset_hardware()
{
	// Resetting the wheel API status in reset() might seem to be a more
	// logical approach, but this is clearly not what CuteMouse does;
	// if this is done in reset(), the DN2 is unable to use mouse wheel
	state.wheel_api = false;
	counter_w       = 0;

	PIC_SetIRQMask(12, false); // lower IRQ line

	// Reset mouse refresh rate
	rate_is_set = false;
	notify_interface_rate();
}

void MOUSEDOS_NotifyMinRate(const uint16_t value_hz)
{
	min_rate_hz = value_hz;

	// If rate was set by a DOS application, don't change it
	if (rate_is_set)
		return;

	notify_interface_rate();
}

void MOUSEDOS_BeforeNewVideoMode()
{
	if (CurMode->type != M_TEXT)
		restore_cursor_background();
	else
		restore_cursor_background_text();

	state.hidden             = 1;
	state.oldhidden          = 1;
	state.background.enabled = false;
}

// TODO: Does way to much. Many things should be moved to mouse reset one day
void MOUSEDOS_AfterNewVideoMode(const bool setmode)
{
	state.inhibit_draw = false;
	// Get the correct resolution from the current video mode
	uint8_t mode = mem_readb(BIOS_VIDEO_MODE);
	if (setmode && mode == state.mode)
		LOG(LOG_MOUSE, LOG_NORMAL)
		("New video mode is the same as the old");
	state.granularity_x = 0xffff;
	state.granularity_y = 0xffff;
	switch (mode) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
	case 0x07: {
		state.granularity_x = (mode < 2) ? 0xfff0 : 0xfff8;
		state.granularity_y = 0xfff8;
		Bitu rows           = IS_EGAVGA_ARCH
		                            ? real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS)
		                            : 24;
		if ((rows == 0) || (rows > 250))
			rows = 24;
		state.maxpos_y = static_cast<int16_t>(8 * (rows + 1) - 1);
		break;
	}
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0d:
	case 0x0e:
	case 0x13: // 320x200 VGA
		if (mode == 0x0d || mode == 0x13)
			state.granularity_x = 0xfffe;
		state.maxpos_y = 199;
		break;
	case 0x0f:
	case 0x10: state.maxpos_y = 349; break;
	case 0x11:
	case 0x12: state.maxpos_y = 479; break;
	default:
		LOG(LOG_MOUSE, LOG_ERROR)
		("Unhandled videomode %X on reset", mode);
		state.inhibit_draw = true;
		return;
	}

	state.mode               = mode;
	state.maxpos_x           = 639;
	state.minpos_x           = 0;
	state.minpos_y           = 0;
	state.hot_x              = 0;
	state.hot_y              = 0;
	state.user_screen_mask   = false;
	state.user_cursor_mask   = false;
	state.text_and_mask      = default_text_and_mask;
	state.text_xor_mask      = default_text_xor_mask;
	state.page               = 0;
	state.update_region_y[1] = -1; // offscreen
	state.cursor_type        = MouseCursor::Software;
	state.enabled            = true;

	MOUSE_NotifyResetDOS();
}

static void reset()
{
	// Although these do not belong to the driver state,
	// reset them too to avoid any possible problems
	counter_w = 0;
	pending.Reset();

	MOUSEDOS_BeforeNewVideoMode();
	MOUSEDOS_AfterNewVideoMode(false);

	set_mickey_pixel_rate(8, 16);
	set_double_speed_threshold(0); // set default value

	state.enabled = true;

	pos_x = static_cast<float>((state.maxpos_x + 1) / 2);
	pos_y = static_cast<float>((state.maxpos_y + 1) / 2);

	state.mickey_counter_x = 0;
	state.mickey_counter_y = 0;
	state.mickey_delta_x   = 0.0f;
	state.mickey_delta_y   = 0.0f;

	state.last_wheel_moved_x = 0;
	state.last_wheel_moved_y = 0;

	for (uint16_t idx = 0; idx < num_buttons; idx++) {
		state.times_pressed[idx]   = 0;
		state.times_released[idx]  = 0;
		state.last_pressed_x[idx]  = 0;
		state.last_pressed_y[idx]  = 0;
		state.last_released_x[idx] = 0;
		state.last_released_y[idx] = 0;
	}

	state.user_callback_mask    = 0;
	mouse_shared.dos_cb_running = false;

	update_driver_active();
	MOUSE_NotifyResetDOS();
}

static void limit_coordinates()
{
	auto limit = [](float &pos, const int16_t minpos, const int16_t maxpos) {
		const float min = static_cast<float>(minpos);
		const float max = static_cast<float>(maxpos);

		pos = std::clamp(pos, min, max);
	};

	limit(pos_x, state.minpos_x, state.maxpos_x);
	limit(pos_y, state.minpos_y, state.maxpos_y);
}

static void update_mickeys_on_move(const float x_rel, const float y_rel)
{
	auto update = [](int16_t &counter, float &delta, const float rel) {
		delta += rel;

		// Check if movement is significant enough
		const auto d = static_cast<int16_t>(std::lround(delta));
		if (d == 0)
			return;

		// Consume part of delta to increase/decrease the counter
		delta -= d;
		int32_t counter_big = counter + d;

		// Handle counter wrap around int16_t limits
		if (counter_big > INT16_MAX)
			counter_big -= UINT16_MAX + 1;
		else if (counter_big < INT16_MIN)
			counter_big += UINT16_MAX + 1;

		counter = static_cast<int16_t>(counter_big);
	};

	const float x_mov = x_rel * state.mickeys_per_pixel_x;
	const float y_mov = y_rel * state.mickeys_per_pixel_y;

	// Update mickey counters and mickey speed measurement
	update(state.mickey_counter_x, state.mickey_delta_x, x_mov);
	update(state.mickey_counter_y, state.mickey_delta_y, y_mov);
	speed_mickeys.Update(std::sqrt(x_mov * x_mov + y_mov * y_mov));
}

static void move_cursor_captured(const float x_rel, const float y_rel)
{
	// Update mickey counters
	update_mickeys_on_move(x_rel, y_rel);

	// Apply mouse movement according to our acceleration model
	pos_x += x_rel;
	pos_y += y_rel;
}

static void move_cursor_seamless(const float x_rel, const float y_rel,
                                 const uint16_t x_abs, const uint16_t y_abs)
{
	// Update mickey counters
	update_mickeys_on_move(x_rel, y_rel);

	auto calculate = [](const uint16_t absolute,
	                    const uint16_t res,
	                    const uint16_t clip) {
		assert(res > 1u);
		return (static_cast<float>(absolute) - clip) /
		       static_cast<float>(res - 1);
	};

	// Apply mouse movement to mimic host OS
	float x = calculate(x_abs, mouse_video.res_x, mouse_video.clip_x);
	float y = calculate(y_abs, mouse_video.res_y, mouse_video.clip_y);

	// TODO: this is probably overcomplicated, especially
	// the usage of relative movement - to be investigated
	if (CurMode->type == M_TEXT) {
		pos_x = x * 8;
		pos_x *= real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
		pos_y = y * 8;
		pos_y *= IS_EGAVGA_ARCH
		               ? static_cast<float>(
		                         real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1)
		               : 25.0f;
	} else if ((state.maxpos_x < 2048) || (state.maxpos_y < 2048) ||
	           (state.maxpos_x != state.maxpos_y)) {
		if ((state.maxpos_x > 0) && (state.maxpos_y > 0)) {
			pos_x = x * state.maxpos_x;
			pos_y = y * state.maxpos_y;
		} else {
			pos_x += x_rel;
			pos_y += y_rel;
		}
	} else {
		// Fake relative movement through absolute coordinates
		pos_x += x_rel;
		pos_y += y_rel;
	}
}

static bool is_captured()
{
	// If DOS driver uses a mapped physical mouse, always consider it
	// captured, as we have no absolute mouse position from the host OS

	return mouse_is_captured || is_mapped;
}

static uint8_t move_cursor()
{
	const auto old_pos_x = get_pos_x();
	const auto old_pos_y = get_pos_y();

	const auto old_mickey_x = static_cast<int16_t>(state.mickey_counter_x);
	const auto old_mickey_y = static_cast<int16_t>(state.mickey_counter_y);

	if (is_captured()) {
		// For raw mouse input use our built-in pointer acceleration model
		const float acceleration_coeff =
		        raw_input ? MOUSE_GetBallisticsCoeff(
		                            speed_mickeys.Get() /
		                            state.double_speed_threshold) *
		                            2.0f
		                  : 2.0f;

		const float tmp_x = pending.x_rel * acceleration_coeff *
		                    state.sensitivity_coeff_x;
		const float tmp_y = pending.y_rel * acceleration_coeff *
		                    state.sensitivity_coeff_y;

		move_cursor_captured(MOUSE_ClampRelativeMovement(tmp_x),
		                     MOUSE_ClampRelativeMovement(tmp_y));

	} else
		move_cursor_seamless(pending.x_rel,
		                     pending.y_rel,
		                     pending.x_abs,
		                     pending.y_abs);

	// Pending relative movement is now consummed
	pending.x_rel = 0.0f;
	pending.y_rel = 0.0f;

	// Make sure cursor stays in the range defined by application
	limit_coordinates();

	// Filter out unneeded events (like sub-pixel mouse movements,
	// which won't change guest side mouse state)
	const bool abs_changed = (old_pos_x != get_pos_x()) ||
	                         (old_pos_y != get_pos_y());
	const bool rel_changed = (old_mickey_x != state.mickey_counter_x) ||
	                         (old_mickey_y != state.mickey_counter_y);

	if (abs_changed || rel_changed)
		return static_cast<uint8_t>(MouseEventId::MouseHasMoved);
	else
		return 0;
}

uint8_t MOUSEDOS_UpdateMoved()
{
	if (mouse_config.dos_immediate)
		return static_cast<uint8_t>(MouseEventId::MouseHasMoved);
	else
		return move_cursor();
}

uint8_t MOUSEDOS_UpdateButtons(const MouseButtons12S new_buttons_12S)
{
	if (buttons.data == new_buttons_12S.data)
		return 0;

	auto mark_pressed = [](const uint8_t idx) {
		state.last_pressed_x[idx] = get_pos_x();
		state.last_pressed_y[idx] = get_pos_y();
		++state.times_pressed[idx];
	};

	auto mark_released = [](const uint8_t idx) {
		state.last_released_x[idx] = get_pos_x();
		state.last_released_y[idx] = get_pos_y();
		++state.times_released[idx];
	};

	uint8_t mask = 0;
	if (new_buttons_12S.left && !buttons.left) {
		mark_pressed(0);
		mask |= static_cast<uint8_t>(MouseEventId::PressedLeft);
	} else if (!new_buttons_12S.left && buttons.left) {
		mark_released(0);
		mask |= static_cast<uint8_t>(MouseEventId::ReleasedLeft);
	}

	if (new_buttons_12S.right && !buttons.right) {
		mark_pressed(1);
		mask |= static_cast<uint8_t>(MouseEventId::PressedRight);
	} else if (!new_buttons_12S.right && buttons.right) {
		mark_released(1);
		mask |= static_cast<uint8_t>(MouseEventId::ReleasedRight);
	}

	if (new_buttons_12S.middle && !buttons.middle) {
		mark_pressed(2);
		mask |= static_cast<uint8_t>(MouseEventId::PressedMiddle);
	} else if (!new_buttons_12S.middle && buttons.middle) {
		mark_released(2);
		mask |= static_cast<uint8_t>(MouseEventId::ReleasedMiddle);
	}

	buttons = new_buttons_12S;
	return mask;
}

static uint8_t move_wheel()
{
	counter_w = clamp_to_int8(static_cast<int32_t>(counter_w + pending.w_rel));

	// Pending wheel scroll is now consummed
	pending.w_rel = 0;

	state.last_wheel_moved_x = get_pos_x();
	state.last_wheel_moved_y = get_pos_y();

	if (counter_w != 0)
		return static_cast<uint8_t>(MouseEventId::WheelHasMoved);
	else
		return 0;
}

uint8_t MOUSEDOS_UpdateWheel()
{
	if (mouse_config.dos_immediate)
		return static_cast<uint8_t>(MouseEventId::WheelHasMoved);
	else
		return move_wheel();
}

bool MOUSEDOS_NotifyMoved(const float x_rel, const float y_rel,
                          const uint16_t x_abs, const uint16_t y_abs)
{
	// Check if an event is needed
	bool event_needed = false;
	if (is_captured()) {
		// Uses relative mouse movements - processing is too complicated
		// to easily predict whether the event can be safely omitted
		event_needed = true;
		// TODO: it actually can be done - but it will require some
		// refactoring
	} else {
		// Uses absolute mouse position (seamless mode), relative
		// movements can wait to be reported - they are completely
		// unreliable anyway
		if (pending.x_abs != x_abs || pending.y_abs != y_abs)
			event_needed = true;
	}

	// Update values to be consummed when the event arrives
	pending.x_rel = MOUSE_ClampRelativeMovement(pending.x_rel + x_rel);
	pending.y_rel = MOUSE_ClampRelativeMovement(pending.y_rel + y_rel);
	pending.x_abs = x_abs;
	pending.y_abs = y_abs;

	// NOTES:
	//
	// It might be tempting to optimize the flow here, by skipping
	// the whole event-queue-callback flow if there is no callback
	// registered, no graphic cursor to draw, etc. Don't do this - there
	// is at least one game (Master of Orion II), which performs INT 0x33
	// calls with 0x0f parameter (changing the callback settings)
	// constantly (don't ask me, why) - doing too much optimization
	// can cause the game to skip mouse events.

	if (!event_needed)
		return 0;

	if (mouse_config.dos_immediate)
		return (move_cursor() != 0);
	else
		return true;
}

bool MOUSEDOS_NotifyWheel(const int16_t w_rel)
{
	if (!state.wheel_api)
		return 0;

	// Although in some places it is possible for the guest code to get
	// wheel counter in 16-bit format, scrolling hundreds of lines in one
	// go would be insane - thus, limit the wheel counter to 8 bits and
	// reuse the code written for other mouse modules
	pending.w_rel = clamp_to_int8(pending.w_rel + w_rel);

	if (pending.w_rel == 0)
		return 0;

	if (mouse_config.dos_immediate)
		return (move_wheel() != 0);
	else
		return true;
}

static Bitu int33_handler()
{
	switch (reg_ax) {
	case 0x00: // MS MOUSE - reset driver and read status
		reset_hardware();
		[[fallthrough]];
	case 0x21:               // MS MOUSE v6.0+ - software reset
		reg_ax = 0xffff; // mouse driver installed
		reg_bx = 3;      // for 2 buttons return 0xffff
		reset();
		break;
	case 0x01: // MS MOUSE v1.0+ - show mouse cursor
		if (state.hidden)
			--state.hidden;
		state.update_region_y[1] = -1; // offscreen
		MOUSEDOS_DrawCursor();
		break;
	case 0x02: // MS MOUSE v1.0+ - hide mouse cursor
		if (CurMode->type != M_TEXT)
			restore_cursor_background();
		else
			restore_cursor_background_text();
		++state.hidden;
		break;
	case 0x03: // MS MOUSE v1.0+ / WheelAPI v1.0+ - get position and button
	           // status
		reg_bl = buttons.data;
		reg_bh = get_reset_wheel_8bit(); // CuteMouse clears wheel
		                                 // counter too
		reg_cx = get_pos_x();
		reg_dx = get_pos_y();
		break;
	case 0x04: // MS MOUSE v1.0+ - position mouse cursor
	{
		// If position isn't different from current position, don't
		// change it. (position is rounded so numbers get lost when the
		// rounded number is set) (arena/simulation Wolf)
		if (reg_to_signed16(reg_cx) != get_pos_x())
			pos_x = static_cast<float>(reg_cx);
		if (reg_to_signed16(reg_dx) != get_pos_y())
			pos_y = static_cast<float>(reg_dx);
		limit_coordinates();
		MOUSEDOS_DrawCursor();
		break;
	}
	case 0x05: // MS MOUSE v1.0+ / WheelAPI v1.0+ - get button press / wheel
	           // data
	{
		const uint16_t idx = reg_bx; // button index
		if (idx == 0xffff && state.wheel_api) {
			// 'magic' index for checking wheel instead of button
			reg_bx = get_reset_wheel_16bit();
			reg_cx = state.last_wheel_moved_x;
			reg_dx = state.last_wheel_moved_y;
		} else if (idx < num_buttons) {
			reg_ax                   = buttons.data;
			reg_bx                   = state.times_pressed[idx];
			reg_cx                   = state.last_pressed_x[idx];
			reg_dx                   = state.last_pressed_y[idx];
			state.times_pressed[idx] = 0;
		} else {
			// unsupported - try to do something same
			reg_ax = buttons.data;
			reg_bx = 0;
			reg_cx = 0;
			reg_dx = 0;
		}
		break;
	}
	case 0x06: // MS MOUSE v1.0+ / WheelAPI v1.0+ - get button release data
	           // / mouse wheel data
	{
		const uint16_t idx = reg_bx; // button index
		if (idx == 0xffff && state.wheel_api) {
			// 'magic' index for checking wheel instead of button
			reg_bx = get_reset_wheel_16bit();
			reg_cx = state.last_wheel_moved_x;
			reg_dx = state.last_wheel_moved_y;
		} else if (idx < num_buttons) {
			reg_ax = buttons.data;
			reg_bx = state.times_released[idx];
			reg_cx = state.last_released_x[idx];
			reg_dx = state.last_released_y[idx];

			state.times_released[idx] = 0;
		} else {
			// unsupported - try to do something same
			reg_ax = buttons.data;
			reg_bx = 0;
			reg_cx = 0;
			reg_dx = 0;
		}
		break;
	}
	case 0x07: // MS MOUSE v1.0+ - define horizontal cursor range
		// Lemmings set 1-640 and wants that. Iron Seed set 0-640. but
		// doesn't like 640. Iron Seed works if newvideo mode with mode
		// 13 sets 0-639. Larry 6 actually wants newvideo mode with mode
		// 13 to set it to 0-319.
		state.minpos_x = std::min(reg_to_signed16(reg_cx), reg_to_signed16(reg_dx));
		state.maxpos_x = std::max(reg_to_signed16(reg_cx), reg_to_signed16(reg_dx));
		// Battle Chess wants this
		pos_x = std::clamp(pos_x,
		                   static_cast<float>(state.minpos_x),
		                   static_cast<float>(state.maxpos_x));
		// Or alternatively this:
		// pos_x = (state.maxpos_x - state.minpos_x + 1) / 2;
		LOG(LOG_MOUSE, LOG_NORMAL)
		("Define Hortizontal range min:%d max:%d", state.minpos_x, state.maxpos_x);
		break;
	case 0x08: // MS MOUSE v1.0+ - define vertical cursor range
		// not sure what to take instead of the CurMode (see case 0x07
		// as well) especially the cases where sheight= 400 and we set
		// it with the mouse_reset to 200 disabled it at the moment.
		// Seems to break syndicate who want 400 in mode 13
		state.minpos_y = std::min(reg_to_signed16(reg_cx),
		                          reg_to_signed16(reg_dx));
		state.maxpos_y = std::max(reg_to_signed16(reg_cx),
		                          reg_to_signed16(reg_dx));
		// Battle Chess wants this
		pos_y = std::clamp(pos_y,
		                   static_cast<float>(state.minpos_y),
		                   static_cast<float>(state.maxpos_y));
		// Or alternatively this:
		// pos_y = (state.maxpos_y - state.minpos_y + 1) / 2;
		LOG(LOG_MOUSE, LOG_NORMAL)
		("Define Vertical range min:%d max:%d", state.minpos_y, state.maxpos_y);
		break;
	case 0x09: // MS MOUSE v3.0+ - define GFX cursor
	{
		auto clamp_hot = [](const uint16_t reg, const int cursor_size) {
			return std::clamp(reg_to_signed16(reg),
			                  static_cast<int16_t>(-cursor_size),
			                  static_cast<int16_t>(cursor_size));
		};

		PhysPt src = SegPhys(es) + reg_dx;
		MEM_BlockRead(src, state.user_def_screen_mask, cursor_size_y * 2);
		src += cursor_size_y * 2;
		MEM_BlockRead(src, state.user_def_cursor_mask, cursor_size_y * 2);
		state.user_screen_mask = true;
		state.user_cursor_mask = true;
		state.hot_x            = clamp_hot(reg_bx, cursor_size_x);
		state.hot_y            = clamp_hot(reg_cx, cursor_size_y);
		state.cursor_type      = MouseCursor::Text;
		MOUSEDOS_DrawCursor();
		break;
	}
	case 0x0a: // MS MOUSE v3.0+ - define text cursor
		// TODO: shouldn't we use MouseCursor::Text, not
		// MouseCursor::Software?
		state.cursor_type   = (reg_bx ? MouseCursor::Hardware
		                              : MouseCursor::Software);
		state.text_and_mask = reg_cx;
		state.text_xor_mask = reg_dx;
		if (reg_bx) {
			INT10_SetCursorShape(reg_cl, reg_dl);
			LOG(LOG_MOUSE, LOG_NORMAL)
			("Hardware Text cursor selected");
		}
		MOUSEDOS_DrawCursor();
		break;
	case 0x27: // MS MOUSE v7.01+ - get screen/cursor masks and mickey counts
		reg_ax = state.text_and_mask;
		reg_bx = state.text_xor_mask;
		[[fallthrough]];
	case 0x0b: // MS MOUSE v1.0+ - read motion data
		reg_cx = signed_to_reg16(state.mickey_counter_x);
		reg_dx = signed_to_reg16(state.mickey_counter_y);
		state.mickey_counter_x = 0;
		state.mickey_counter_y = 0;
		break;
	case 0x0c: // MS MOUSE v1.0+ - define user callback parameters
		state.user_callback_mask    = reg_cx;
		state.user_callback_segment = SegValue(es);
		state.user_callback_offset  = reg_dx;
		update_driver_active();
		break;
	case 0x0d: // MS MOUSE v1.0+ - light pen emulation on
	case 0x0e: // MS MOUSE v1.0+ - light pen emulation off
		// Both buttons down = pen pressed, otherwise pen considered
		// off-screen
		// TODO: maybe implement light pen using SDL touch events?
		LOG(LOG_MOUSE, LOG_ERROR)
		("Mouse light pen emulation not implemented");
		break;
	case 0x0f: // MS MOUSE v1.0+ - define mickey/pixel rate
		set_mickey_pixel_rate(reg_to_signed16(reg_cx),
		                      reg_to_signed16(reg_dx));
		break;
	case 0x10: // MS MOUSE v1.0+ - define screen region for updating
		state.update_region_x[0] = reg_to_signed16(reg_cx);
		state.update_region_y[0] = reg_to_signed16(reg_dx);
		state.update_region_x[1] = reg_to_signed16(reg_si);
		state.update_region_y[1] = reg_to_signed16(reg_di);
		MOUSEDOS_DrawCursor();
		break;
	case 0x11: // WheelAPI v1.0+ - get mouse capabilities
		reg_ax          = 0x574d; // Identifier for detection purposes
		reg_bx          = 0;      // Reserved capabilities flags
		reg_cx          = 1;      // Wheel is supported
		state.wheel_api = true; // This call enables WheelAPI extensions
		counter_w       = 0;
		// Previous implementation provided Genius Mouse 9.06 function
		// to get number of buttons
		// (https://sourceforge.net/p/dosbox/patches/32/), it was
		// returning 0xffff in reg_ax and number of buttons in reg_bx; I
		// suppose the WheelAPI extensions are more useful
		break;
	case 0x12: // MS MOUSE - set large graphics cursor block
		LOG(LOG_MOUSE, LOG_ERROR)
		("Large graphics cursor block not implemented");
		break;
	case 0x13: // MS MOUSE v5.0+ - set double-speed threshold
		set_double_speed_threshold(reg_bx);
		break;
	case 0x14: // MS MOUSE v3.0+ - exchange event-handler
	{
		const auto old_segment = state.user_callback_segment;
		const auto old_offset  = state.user_callback_offset;
		const auto old_mask    = state.user_callback_mask;
		// Set new values
		state.user_callback_mask    = reg_cx;
		state.user_callback_segment = SegValue(es);
		state.user_callback_offset  = reg_dx;
		update_driver_active();
		// Return old values
		reg_cx = old_mask;
		reg_dx = old_offset;
		SegSet16(es, old_segment);
		break;
	}
	case 0x15: // MS MOUSE v6.0+ - get driver storage space requirements
		reg_bx = sizeof(state);
		break;
	case 0x16: // MS MOUSE v6.0+ - save driver state
		LOG(LOG_MOUSE, LOG_WARN)("Saving driver state...");
		MEM_BlockWrite(SegPhys(es) + reg_dx, &state, sizeof(state));
		break;
	case 0x17: // MS MOUSE v6.0+ - load driver state
		LOG(LOG_MOUSE, LOG_WARN)("Loading driver state...");
		MEM_BlockRead(SegPhys(es) + reg_dx, &state, sizeof(state));
		pending.Reset();
		update_driver_active();
		set_sensitivity(state.sensitivity_x,
		                state.sensitivity_y,
		                state.unknown_01);
		// TODO: we should probably also fake an event for mouse
		// movement, redraw cursor, etc.
		break;
	case 0x18: // MS MOUSE v6.0+ - set alternate mouse user handler
	case 0x19: // MS MOUSE v6.0+ - set alternate mouse user handler
		LOG(LOG_MOUSE, LOG_ERROR)
		("Alternate mouse user handler not implemented");
		break;
	case 0x1a: // MS MOUSE v6.0+ - set mouse sensitivity
		// NOTE: Ralf Brown Interrupt List (and some other sources)
		// claim, that this should duplicate functions 0x0f and 0x13 -
		// this is not true at least for Mouse Systems driver v8.00 and
		// IBM/Microsoft driver v8.20
		set_sensitivity(reg_bx, reg_cx, reg_dx);
		break;
	case 0x1b: //  MS MOUSE v6.0+ - get mouse sensitivity
		reg_bx = state.sensitivity_x;
		reg_cx = state.sensitivity_y;
		reg_dx = state.unknown_01;
		break;
	case 0x1c: // MS MOUSE v6.0+ - set interrupt rate
		set_interrupt_rate(reg_bx);
		break;
	case 0x1d: // MS MOUSE v6.0+ - set display page number
		state.page = reg_bl;
		break;
	case 0x1e: // MS MOUSE v6.0+ - get display page number
		reg_bx = state.page;
		break;
	case 0x1f: // MS MOUSE v6.0+ - disable mouse driver
		// ES:BX old mouse driver Zero at the moment TODO
		reg_bx = 0;
		SegSet16(es, 0);
		state.enabled   = false;
		state.oldhidden = state.hidden;
		state.hidden    = 1;
		// According to Ralf Brown Interrupt List it returns 0x20 if
		// success,  but CuteMouse source code claims the code for
		// success if 0x1f. Both agree that 0xffff means failure.
		// Since reg_ax is 0x1f here, no need to change anything.
		break;
	case 0x20: // MS MOUSE v6.0+ - enable mouse driver
		state.enabled = true;
		state.hidden  = state.oldhidden;
		break;
	case 0x22: // MS MOUSE v6.0+ - set language for messages
		// 00h = English, 01h = French, 02h = Dutch, 03h = German, 04h =
		// Swedish 05h = Finnish, 06h = Spanish, 07h = Portugese, 08h =
		// Italian
		state.language = reg_bx;
		break;
	case 0x23: // MS MOUSE v6.0+ - get language for messages
		reg_bx = state.language;
		break;
	case 0x24: // MS MOUSE v6.26+ - get Software version, mouse type, and
	           // IRQ number
		reg_bx = 0x805; // version 8.05 woohoo
		reg_ch = 0x04;  // PS/2 type
		reg_cl = 0; // PS/2 mouse; for others it would be an IRQ number
		break;
	case 0x25: // MS MOUSE v6.26+ - get general driver information
		// TODO: According to PC sourcebook reference
		//       Returns:
		//       AH = status
		//         bit 7 driver type: 1=sys 0=com
		//         bit 6: 0=non-integrated 1=integrated mouse driver
		//         bits 4-5: cursor type  00=software text cursor
		//         01=hardware text cursor 1X=graphics cursor bits 0-3:
		//         Function 28 mouse interrupt rate
		//       AL = Number of MDDS (?)
		//       BX = fCursor lock
		//       CX = FinMouse code
		//       DX = fMouse busy
		LOG(LOG_MOUSE, LOG_ERROR)
		("General driver information not implemented");
		break;
	case 0x26: // MS MOUSE v6.26+ - get maximum virtual coordinates
		reg_bx = (state.enabled ? 0x0000 : 0xffff);
		reg_cx = signed_to_reg16(state.maxpos_x);
		reg_dx = signed_to_reg16(state.maxpos_y);
		break;
	case 0x28: // MS MOUSE v7.0+ - set video mode
		// TODO: According to PC sourcebook
		//       Entry:
		//       CX = Requested video mode
		//       DX = Font size, 0 for default
		//       Returns:
		//       DX = 0 on success, nonzero (requested video mode) if not
		LOG(LOG_MOUSE, LOG_ERROR)("Set video mode not implemented");
		break;
	case 0x29: // MS MOUSE v7.0+ - enumerate video modes
		// TODO: According to PC sourcebook
		//       Entry:
		//       CX = 0 for first, != 0 for next
		//       Exit:
		//       BX:DX = named string far ptr
		//       CX = video mode number
		LOG(LOG_MOUSE, LOG_ERROR)
		("Enumerate video modes not implemented");
		break;
	case 0x2a: // MS MOUSE v7.01+ - get cursor hot spot
		// Microsoft uses a negative byte counter
		// for cursor visibility
		reg_al = static_cast<uint8_t>(-state.hidden);
		reg_bx = signed_to_reg16(state.hot_x);
		reg_cx = signed_to_reg16(state.hot_y);
		reg_dx = 0x04; // PS/2 mouse type
		break;
	case 0x2b: // MS MOUSE v7.0+ - load acceleration profiles
	case 0x2c: // MS MOUSE v7.0+ - get acceleration profiles
	case 0x2d: // MS MOUSE v7.0+ - select acceleration profile
	case 0x2e: // MS MOUSE v8.10+ - set acceleration profile names
	case 0x33: // MS MOUSE v7.05+ - get/switch accelleration profile
		LOG(LOG_MOUSE, LOG_ERROR)
		("Custom acceleration profiles not implemented");
		break;
	case 0x2f: // MS MOUSE v7.02+ - mouse hardware reset
		LOG(LOG_MOUSE, LOG_ERROR)
		("INT 33 AX=2F mouse hardware reset not implemented");
		break;
	case 0x30: // MS MOUSE v7.04+ - get/set BallPoint information
		LOG(LOG_MOUSE, LOG_ERROR)
		("Get/set BallPoint information not implemented");
		break;
	case 0x31: // MS MOUSE v7.05+ - get current min/max virtual coordinates
		reg_ax = signed_to_reg16(state.minpos_x);
		reg_bx = signed_to_reg16(state.minpos_y);
		reg_cx = signed_to_reg16(state.maxpos_x);
		reg_dx = signed_to_reg16(state.maxpos_y);
		break;
	case 0x32: // MS MOUSE v7.05+ - get active advanced functions
		LOG(LOG_MOUSE, LOG_ERROR)
		("Get active advanced functions not implemented");
		break;
	case 0x34: // MS MOUSE v8.0+ - get initialization file
		LOG(LOG_MOUSE, LOG_ERROR)
		("Get initialization file not implemented");
		break;
	case 0x35: // MS MOUSE v8.10+ - LCD screen large pointer support
		LOG(LOG_MOUSE, LOG_ERROR)
		("LCD screen large pointer support not implemented");
		break;
	case 0x4d: // MS MOUSE - return pointer to copyright string
		LOG(LOG_MOUSE, LOG_ERROR)
		("Return pointer to copyright string not implemented");
		break;
	case 0x6d: // MS MOUSE - get version string
		LOG(LOG_MOUSE, LOG_ERROR)("Get version string not implemented");
		break;
	case 0x70: // Mouse Systems - installation check
	case 0x72: // Mouse Systems 7.01+, Genius Mouse 9.06+ - unknown
	case 0x73: // Mouse Systems 7.01+ - get button assignments
		LOG(LOG_MOUSE, LOG_ERROR)
		("Mouse Sytems mouse extensions not implemented");
		break;
	case 0x53C1: // Logitech CyberMan
		LOG(LOG_MOUSE, LOG_NORMAL)
		("Mouse function 53C1 for Logitech CyberMan called. Ignored by regular mouse driver.");
		break;
	default:
		LOG(LOG_MOUSE, LOG_ERROR)
		("Mouse function %04X not implemented", reg_ax);
		break;
	}
	return CBRET_NONE;
}

static Bitu mouse_bd_handler()
{
	// the stack contains offsets to register values
	uint16_t raxpt = real_readw(SegValue(ss), static_cast<uint16_t>(reg_sp + 0x0a));
	uint16_t rbxpt = real_readw(SegValue(ss), static_cast<uint16_t>(reg_sp + 0x08));
	uint16_t rcxpt = real_readw(SegValue(ss), static_cast<uint16_t>(reg_sp + 0x06));
	uint16_t rdxpt = real_readw(SegValue(ss), static_cast<uint16_t>(reg_sp + 0x04));

	// read out the actual values, registers ARE overwritten
	const uint16_t rax = real_readw(SegValue(ds), raxpt);
	reg_ax = rax;
	reg_bx = real_readw(SegValue(ds), rbxpt);
	reg_cx = real_readw(SegValue(ds), rcxpt);
	reg_dx = real_readw(SegValue(ds), rdxpt);

	// some functions are treated in a special way (additional registers)
	switch (rax) {
	case 0x09: // Define GFX Cursor
	case 0x16: // Save driver state
	case 0x17: // load driver state
		SegSet16(es, SegValue(ds));
		break;
	case 0x0c: // Define interrupt subroutine parameters
	case 0x14: // Exchange event-handler
		if (reg_bx != 0)
			SegSet16(es, reg_bx);
		else
			SegSet16(es, SegValue(ds));
		break;
	case 0x10: // Define screen region for updating
		reg_cx = real_readw(SegValue(ds), rdxpt);
		reg_dx = real_readw(SegValue(ds), static_cast<uint16_t>(rdxpt + 2));
		reg_si = real_readw(SegValue(ds), static_cast<uint16_t>(rdxpt + 4));
		reg_di = real_readw(SegValue(ds), static_cast<uint16_t>(rdxpt + 6));
		break;
	default: break;
	}

	int33_handler();

	// save back the registers, too
	real_writew(SegValue(ds), raxpt, reg_ax);
	real_writew(SegValue(ds), rbxpt, reg_bx);
	real_writew(SegValue(ds), rcxpt, reg_cx);
	real_writew(SegValue(ds), rdxpt, reg_dx);
	switch (rax) {
	case 0x1f: // Disable Mousedriver
		real_writew(SegValue(ds), rbxpt, SegValue(es));
		break;
	case 0x14: // Exchange event-handler
		real_writew(SegValue(ds), rcxpt, SegValue(es));
		break;
	default: break;
	}

	return CBRET_NONE;
}

static Bitu user_callback_handler()
{
	mouse_shared.dos_cb_running = false;
	return CBRET_NONE;
}

bool MOUSEDOS_HasCallback(const uint8_t mask)
{
	return state.user_callback_mask & mask;
}

Bitu MOUSEDOS_DoCallback(const uint8_t mask, const MouseButtons12S buttons_12S)
{
	mouse_shared.dos_cb_running = true;
	const bool mouse_moved      = mask &
	                         static_cast<uint8_t>(MouseEventId::MouseHasMoved);
	const bool wheel_moved = mask &
	                         static_cast<uint8_t>(MouseEventId::WheelHasMoved);

	// Extension for Windows mouse driver by javispedro:
	// - https://git.javispedro.com/cgit/vbados.git/about/
	// which allows seamless mouse integration. It is also included in
	// DOSBox-X and Dosemu2:
	// - https://github.com/joncampbell123/dosbox-x/pull/3424
	// -
	// https://github.com/dosemu2/dosemu2/issues/1552#issuecomment-1100777880
	// -
	// https://github.com/dosemu2/dosemu2/commit/cd9d2dbc8e3d58dc7cbc92f172c0d447881526be
	// -
	// https://github.com/joncampbell123/dosbox-x/commit/aec29ce28eb4b520f21ead5b2debf370183b9f28
	reg_ah = (!is_captured() && mouse_moved) ? 1 : 0;

	reg_al = mask;
	reg_bl = buttons_12S.data;
	reg_bh = wheel_moved ? get_reset_wheel_8bit() : 0;
	reg_cx = get_pos_x();
	reg_dx = get_pos_y();
	reg_si = signed_to_reg16(state.mickey_counter_x);
	reg_di = signed_to_reg16(state.mickey_counter_y);

	CPU_Push16(RealSeg(user_callback));
	CPU_Push16(RealOff(user_callback));
	CPU_Push16(state.user_callback_segment);
	CPU_Push16(state.user_callback_offset);

	return CBRET_NONE;
}

void MOUSEDOS_NotifyMapped(const bool enabled)
{
	is_mapped = enabled;
}

void MOUSEDOS_NotifyRawInput(const bool enabled)
{
	raw_input = enabled;
}

void MOUSEDOS_Init()
{
	// Callback for mouse interrupt 0x33
	auto call_int33 = CALLBACK_Allocate();
	// RealPt int33_location = RealMake(CB_SEG + 1,(call_int33 * CB_SIZE) -
	// 0x10);
	RealPt int33_location = RealMake(static_cast<uint16_t>(DOS_GetMemory(0x1) - 1),
	                                 0x10);
	CALLBACK_Setup(call_int33,
	               &int33_handler,
	               CB_MOUSE,
	               Real2Phys(int33_location),
	               "Mouse");
	// Wasteland needs low(seg(int33))!=0 and low(ofs(int33))!=0
	real_writed(0, 0x33 << 2, int33_location);

	auto call_mouse_bd = CALLBACK_Allocate();
	CALLBACK_Setup(call_mouse_bd,
	               &mouse_bd_handler,
	               CB_RETF8,
	               PhysMake(RealSeg(int33_location),
	                        static_cast<uint16_t>(RealOff(int33_location) + 2)),
	               "MouseBD");
	// pseudocode for CB_MOUSE (including the special backdoor entry point):
	//    jump near i33hd
	//    callback mouse_bd_handler
	//    retf 8
	//  label i33hd:
	//    callback int33_handler
	//    iret

	// Callback for mouse user routine return
	auto call_user = CALLBACK_Allocate();
	CALLBACK_Setup(call_user, &user_callback_handler, CB_RETF_CLI, "mouse user ret");
	user_callback = CALLBACK_RealPointer(call_user);

	state.user_callback_segment = 0x6362;    // magic value
	state.hidden                = 1;         // hide cursor on startup
	state.mode                  = UINT8_MAX; // non-existing mode

	set_sensitivity(50, 50, 50);
	reset_hardware();
	reset();
}
