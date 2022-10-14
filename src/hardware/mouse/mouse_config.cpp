/*
 *  Copyright (C) 2022-2022  The DOSBox Staging Team
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

#include "mouse_config.h"
#include "mouse_common.h"
#include "mouse_interfaces.h"

#include "checks.h"
#include "control.h"
#include "math_utils.h"
#include "setup.h"
#include "support.h"
#include "video.h"

#include <cmath>

CHECK_NARROWING();

// TODO - IntelliMouse Explorer emulation is currently deactivated - there is
// probably no way to test it. The IntelliMouse 3.0 software can use it, but
// it seems to require physical PS/2 mouse registers to work correctly,
// and these are not emulated yet.

// #define ENABLE_EXPLORER_MOUSE

MouseConfig mouse_config;
MousePredefined mouse_predefined;

static const std::vector<std::string> list_models_ps2 = {
        "standard",
        "intellimouse",
#ifdef ENABLE_EXPLORER_MOUSE
        "explorer",
#endif
};

static const std::vector<std::string> list_models_com = {
        "2button",
        "3button",
        "wheel",
        "msm",
        "2button+msm",
        "3button+msm",
        "wheel+msm",
};

static const std::vector<uint16_t> list_rates = {
        // Commented out values are probably not interesting
        // for the end user as "boosted" sampling rate
        //  10",  // PS/2 mouse
        //  20",  // PS/2 mouse
        //  30",  // bus/InPort mouse
        40,  // PS/2 mouse, approx. limit for 1200 baud serial mouse
             //  50,  // bus/InPort mouse
        60,  // PS/2 mouse, used by Microsoft Mouse Driver 8.20
        80,  // PS/2 mouse, approx. limit for 2400 baud serial mouse
        100, // PS/2 mouse, bus/InPort mouse, used by CuteMouse 2.1b4
        125, // USB mouse (basic, non-gaming), Bluetooth mouse
        160, // approx. limit for 4800 baud serial mouse
        200, // PS/2 mouse, bus/InPort mouse
        250, // USB mouse (gaming)
        330, // approx. limit for 9600 baud serial mouse
        500, // USB mouse (gaming)

        // Todays gaming USB mice are capable of even higher sampling
        // rates (like 1000 Hz), but such values are way higher than
        // anything DOS games were designed for; most likely such rates
        // would only result in emulation slowdowns and compatibility
        // issues.
};

bool MouseConfig::ParseSerialModel(const std::string &model_str,
                                   MouseModelCOM &model, bool &auto_msm)
{
	if (model_str == list_models_com[0]) {
		model    = MouseModelCOM::Microsoft;
		auto_msm = false;
		return true;
	} else if (model_str == list_models_com[1]) {
		model    = MouseModelCOM::Logitech;
		auto_msm = false;
		return true;
	} else if (model_str == list_models_com[2]) {
		model    = MouseModelCOM::Wheel;
		auto_msm = false;
		return true;
	} else if (model_str == list_models_com[3]) {
		model    = MouseModelCOM::MouseSystems;
		auto_msm = false;
		return true;
	} else if (model_str == list_models_com[4]) {
		model    = MouseModelCOM::Microsoft;
		auto_msm = true;
		return true;
	} else if (model_str == list_models_com[5]) {
		model    = MouseModelCOM::Logitech;
		auto_msm = true;
		return true;
	} else if (model_str == list_models_com[6]) {
		model    = MouseModelCOM::Wheel;
		auto_msm = true;
		return true;
	}

	return false;
}

const std::vector<uint16_t> &MouseConfig::GetValidMinRateList()
{
	return list_rates;
}

static void config_read(Section *section)
{
	assert(section);
	const Section_prop *conf = dynamic_cast<Section_prop *>(section);
	assert(conf);
	if (!conf)
		return;

	// Settings changeable during runtime

	mouse_config.dos_immediate = conf->Get_bool("dos_mouse_immediate");
	mouse_config.raw_input     = conf->Get_bool("mouse_raw_input");
	GFX_SetMouseRawInput(mouse_config.raw_input);

	// Settings below should be read only once

	if (mouse_shared.ready_config_mouse) {
		MOUSE_NotifyStateChanged();
		return;
	}

	// Default mouse sensitivity

	auto sensitivity_from_str = [](const std::string &str) {
		const int32_t base_value = 50;
		int32_t ret_val          = base_value; // default
		bool invalid_input       = false;

		if (str.find('.') != std::string::npos) {
			// Parameter supplied in a form of floating point
			float tmp_float = 1.0f;
			try {
				tmp_float = std::stof(str);
			} catch (...) {
				invalid_input = true;
				tmp_float     = 1.0f;
			}
			// We need 'tmp_float' to be positive for further calculations
			if (tmp_float < 0.0f) {
				tmp_float = -tmp_float;
				ret_val   = -1;
			} else if (tmp_float > 0.0f)
				ret_val = 1;
			else
				ret_val = 0;
			// Now calculate user steps as a logarithm
			if (ret_val != 0) {
				tmp_float = std::log(tmp_float) / std::log(2.0f);
				tmp_float *= mouse_predefined.sensitivity_double_steps;
				tmp_float += static_cast<float>(base_value);
				const auto tmp_rounded = std::lround(
				        std::max(tmp_float, 1.0f));
				ret_val = clamp_to_int8(ret_val * tmp_rounded);
			}
		} else if (!str.empty()) {
			// Parameter supplied in a form of integer
			try {
				ret_val = std::stoi(str);
			} catch (...) {
				invalid_input = true;
				ret_val       = base_value;
			}
		}

		if (invalid_input)
			LOG_ERR("MOUSE: Invalid sensitivity value");

		const auto min = clamp_to_int8(-mouse_predefined.sensitivity_user_max);
		const auto max = clamp_to_int8(mouse_predefined.sensitivity_user_max);
		return std::clamp(clamp_to_int8(ret_val), min, max);
	};

	PropMultiVal *prop_multi = conf->GetMultiVal("mouse_sensitivity");
	const std::string xsens = prop_multi->GetSection()->Get_string("xsens");
	const std::string ysens = prop_multi->GetSection()->Get_string("ysens");

	mouse_config.sensitivity_x = sensitivity_from_str(xsens);
	if (ysens.empty())
		mouse_config.sensitivity_y = mouse_config.sensitivity_x;
	else
		mouse_config.sensitivity_y = sensitivity_from_str(ysens);

	// DOS driver configuration

	mouse_config.dos_driver = conf->Get_bool("dos_mouse_driver");

	// PS/2 AUX port mouse configuration

	std::string prop_str = conf->Get_string("ps2_mouse_model");
	if (prop_str == list_models_ps2[0])
		mouse_config.model_ps2 = MouseModelPS2::Standard;
	if (prop_str == list_models_ps2[1])
		mouse_config.model_ps2 = MouseModelPS2::IntelliMouse;
#ifdef ENABLE_EXPLORER_MOUSE
	if (prop_str == list_models_ps2[2])
		mouse_config.model_ps2 = MouseModelPS2::Explorer;
#endif

	// Serial (COM port) mice configuration

	auto set_model_com = [](const std::string &model_str,
	                        MouseModelCOM &model_var,
	                        bool &model_auto) {
		if (model_str == list_models_com[0] ||
		    model_str == list_models_com[4])
			model_var = MouseModelCOM::Microsoft;
		if (model_str == list_models_com[1] ||
		    model_str == list_models_com[5])
			model_var = MouseModelCOM::Logitech;
		if (model_str == list_models_com[2] ||
		    model_str == list_models_com[6])
			model_var = MouseModelCOM::Wheel;
		if (model_str == list_models_com[3])
			model_var = MouseModelCOM::MouseSystems;

		if (model_str == list_models_com[4] ||
		    model_str == list_models_com[5] ||
		    model_str == list_models_com[6])
			model_auto = true;
		else
			model_auto = false;
	};

	prop_str = conf->Get_string("com_mouse_model");
	set_model_com(prop_str, mouse_config.model_com, mouse_config.model_com_auto_msm);

	// Start mouse emulation if ready
	mouse_shared.ready_config_mouse = true;
	MOUSE_Startup();
}

static void config_init(Section_prop &secprop)
{
	constexpr auto always        = Property::Changeable::Always;
	constexpr auto only_at_start = Property::Changeable::OnlyAtStart;

	Prop_bool *prop_bool = nullptr;
	// XXX Prop_int     *prop_int   = nullptr;
	Prop_string *prop_str    = nullptr;
	PropMultiVal *prop_multi = nullptr;

	// General configuration

	prop_multi = secprop.AddMultiVal("mouse_sensitivity", only_at_start, ",");
	prop_multi->Set_help(
	        "Default mouse sensitivity.\n"
	        "Integer values work exponentially, add 10 to double the effect.\n"
	        "Alternatively, put 1.0 for base sensitivity, 1.5 do double sensitivity, etc.\n"
	        "Negative values reverse mouse direction, 0 disables the movement completely.\n"
	        "The optional second parameter specifies vertical sensitivity (e.g. 1.5,3.0).\n"
	        "Setting can be adjusted in runtime (also per mouse interface) using internal\n"
	        "MOUSECTL.COM tool, available on drive Z:.");
	prop_multi->SetValue("1.0");
	prop_str = prop_multi->GetSection()->Add_string("xsens", only_at_start, "1.0");
	prop_str = prop_multi->GetSection()->Add_string("ysens", only_at_start, "1.0");

	prop_bool = secprop.Add_bool("mouse_raw_input", always, true);
	prop_bool->Set_help(
	        "Enable to bypass your operating system's mouse acceleration and sensitivity\n"
	        "settings. Works in fullscreen or when the mouse is captured in window mode.");

	// DOS driver configuration

	prop_bool = secprop.Add_bool("dos_mouse_driver", only_at_start, true);
	assert(prop_bool);
	prop_bool->Set_help(
	        "Enable built-in DOS mouse driver.\n"
	        "Notes:\n"
	        "   Disable if you intend to use original MOUSE.COM driver in emulated DOS.\n"
	        "   When guest OS is booted, built-in driver gets disabled automatically.");

	prop_bool = secprop.Add_bool("dos_mouse_immediate", always, false);
	assert(prop_bool);
	prop_bool->Set_help(
	        "Updates mouse movement counters immediately, without waiting for interrupt.\n"
	        "May improve gameplay, especially in fast paced games (arcade, FPS, etc.) - as\n"
	        "for some games it effectively boosts the mouse sampling rate to 1000 Hz, without\n"
	        "increasing interrupt overhead.\n"
	        "Might cause compatibility issues. List of known incompatible games:\n"
	        "   - Ultima Underworld: The Stygian Abyss\n"
	        "   - Ultima Underworld II: Labyrinth of Worlds\n"
	        "Please file a bug with the project if you find another game that fails when\n"
	        "this is enabled, we will update this list.");

	// Physical mice configuration

	prop_str = secprop.Add_string("ps2_mouse_model", only_at_start, "intellimouse");
	assert(prop_str);
	prop_str->Set_values(list_models_ps2);
	prop_str->Set_help(
	        "PS/2 AUX port mouse model:\n"
	        // TODO - Add option "none"
	        "   standard:       3 buttons, standard PS/2 mouse.\n"
	        "   intellimouse:   3 buttons + wheel, Microsoft IntelliMouse.\n"
#ifdef ENABLE_EXPLORER_MOUSE
	        "   explorer:       5 buttons + wheel, Microsoft IntelliMouse Explorer.\n"
#endif
	        "Default: intellimouse");

	prop_str = secprop.Add_string("com_mouse_model", only_at_start, "wheel+msm");
	assert(prop_str);
	prop_str->Set_values(list_models_com);
	prop_str->Set_help(
	        "COM (serial) port default mouse model:\n"
	        "   2button:        2 buttons, Microsoft mouse.\n"
	        "   3button:        3 buttons, Logitech mouse, mostly compatible with Microsoft mouse.\n"
	        "   wheel:          3 buttons + wheel, mostly compatible with Microsoft mouse.\n"
	        "   msm:            3 buttons, Mouse Systems mouse, NOT COMPATIBLE with Microsoft mouse.\n"
	        "   2button+msm:    Automatic choice between 2button and msm.\n"
	        "   3button+msm:    Automatic choice between 3button and msm.\n"
	        "   wheel+msm:      Automatic choice between wheel and msm.\n"
	        "Default: wheel+msm\n"
	        "Notes:\n"
	        "   Go to [serial] section to enable/disable COM port mice.");
}

void MOUSE_AddConfigSection(const config_ptr_t &conf)
{
	assert(conf);
	Section_prop *sec = conf->AddSection_prop("mouse", &config_read, true);
	assert(sec);
	config_init(*sec);
}
