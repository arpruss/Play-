#include "ext/libretro.h"

#include "Log.h"
#include "AppConfig.h"
#include "PS2VM.h"
#include "ui_shared/BootableUtils.h"

#include "PS2VM_Preferences.h"
#include "GSH_OpenGL_Libretro.h"
#include "SH_LibreAudio.h"
#include "PH_Libretro_Input.h"
#include "iop/IopBios.h"
#include "iop/Iop_Usbd.h"
#include "iop/UsbGunCon2Device.h"

#include "PathUtils.h"
#include "PtrStream.h"
#include "MemStream.h"

#include "filesystem_def.h"
#include "DefaultAppConfig.h"

#include <vector>
#include <cstdlib>
#include <ctype.h>

#define LOG_NAME "LIBRETRO"

static CPS2VM* m_virtualMachine = nullptr;
static bool first_run = false;

bool libretro_supports_bitmasks = false;
retro_video_refresh_t g_video_cb;
retro_environment_t g_environ_cb;
retro_input_poll_t g_input_poll_cb;
retro_input_state_t g_input_state_cb;
retro_audio_sample_batch_t g_set_audio_sample_batch_cb;

std::map<int, int> g_ds2_to_retro_btn_map;
struct retro_hw_render_callback g_hw_render
{
};

int g_res_factor = 1;
CGSHandler::PRESENTATION_MODE g_presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FIT;
bool g_forceBilinearTextures = false;

// Port currently assigned RETRO_DEVICE_LIGHTGUN, or -1 if none (see
// retro_set_controller_port_device). GunCon2 is a fixed USB peripheral in
// Play! (CGunCon2UsbDevice, always device 0 on the emulated USB bus), so this
// only needs to remember which retropad *port* is driving it.
static int g_lightgun_port = -1;

struct lightgun_info_s {
    char serial[10];
    int width;
    int height;    
    int scale_x;
    int scale_y;
    int center_x;
    int center_y;
};

static const struct lightgun_info_s lightgun_defaults = { "default__", 640,240,10000,10000,320,120 };

const struct lightgun_info_s* lightgun_info = nullptr;

static const struct lightgun_info_s lightgun_games[] = {
    {"SLES50930", 640, 256, 9025, 9450, 390, 132}, // Dino Stalker (E, English)
    {"SLES51095", 640, 256, 9025, 9450, 390, 132}, // Dino Stalker (E, French)
    {"SLES51096", 640, 256, 9025, 9450, 390, 132}, // Dino Stalker (E, German)
    {"SLUS20485", 640, 240, 9025, 9250, 390, 132}, // Dino Stalker (U)
    {"SLUS20389", 640, 240, 8925, 9350, 422, 141}, // Endgame (U)
    {"SLES50936", 512, 256, 11200, 10000, 320, 120}, // Endgame (E) (Guncon2 needs to be connected to USB port 2)
    {"SLPM65139", 640, 240, 9000, 9150, 320, 120}, // Gun Survivor 3: Dino Crisis (J)
    {"SLES52620", 640, 256, 8950, 11230, 390, 147}, // Guncom 2 (E)
    {"SLES51289", 640, 256, 8450, 8900, 456, 164}, // Gunfighter 2 - Jesse James (E)
    {"SLPS25165", 640, 240, 9025, 9800, 390, 138}, // Gunvari Collection (J) (480i)
    {"SCES50889", 640, 256, 9025, 9450, 390, 169}, // Ninja Assault (E)
    {"SLPS20218", 640, 240, 9000, 9200, 320, 134}, // Ninja Assault (J)
    {"SLUS20492", 640, 240, 9025, 9250, 390, 132}, // Ninja Assault (U)
    {"SLES50650", 640, 240, 8475, 9600, 454, 164}, // Resident Evil Survivor 2 (E)
    {"SLES51448", 640, 240, 9025, 9500, 420, 132}, // Resident Evil - Dead Aim (E)
    {"SLUS20669", 640, 240, 9025, 9350, 420, 132}, // Resident Evil - Dead Aim (U)
    {"SLUS20619", 640, 256, 9025, 9175, 453, 154}, // Starsky & Hutch (U)
    {"SCES50300", 640, 256, 9025, 10275, 390, 138}, // Time Crisis II (E)
    {"SLPS20122", 640, 240, 9025, 9750, 390, 154}, // Time Crisis II (J)
    {"SLPS20113", 640, 240, 9025, 9750, 390, 154}, // Time Crisis II (with GunCon 2) (J)
    {"SLUS20219", 640, 240, 9025, 9750, 422, 170}, // Time Crisis 2 (U)
    {"SCES51844", 640, 256, 9025, 10275, 390, 138}, // Time Crisis 3 (E)
    {"SLUS20645", 640, 240, 9025, 9750, 390, 154}, // Time Crisis 3 (U)
    {"SCES52530", 640, 256, 9025, 9900, 390, 153}, // Crisis Zone (E)
    {"SLUS20927", 640, 240, 9025, 9900, 390, 153}, // Time Crisis - Crisis Zone (U) (480i)
    {"SCES50411", 640, 256, 8980, 9990, 421, 138}, // Vampire Night (E)
    {"SLPS25077", 640, 240, 9000, 9750, 422, 118}, // Vampire Night (J)
    {"SLUS20221", 640, 228, 8980, 10250, 422, 124}, // Vampire Night (U)
    {"SLES51229", 512, 256, 11015, 10000, 433, 159}, // Virtua Cop - Elite Edition (E,J) (480i)
    {"SLPM62205", 512, 256, 11015, 10000, 433, 159}, // Virtua Cop Re-Birth (J) (480i)
};


static std::vector<struct retro_variable> m_vars =
    {
        {"play_res_multi", "Resolution Multiplier; 1x|2x|4x|8x"},
        {"play_presentation_mode", "Presentation Mode; Fit Screen|Fill Screen|Original Size"},
        {"play_bilinear_filtering", "Force Bilinear Filtering; false|true"},
        {NULL, NULL},
};

enum class BootType
{
	CD,
	ELF
};

struct LastOpenCommand
{
	LastOpenCommand() = default;
	LastOpenCommand(BootType type, fs::path path)
	    : type(type)
	    , path(path)
	{
	}
	BootType type = BootType::CD;
	fs::path path;
};

LastOpenCommand m_bootCommand;

unsigned retro_api_version()
{
	return RETRO_API_VERSION;
}

void SetupVideoHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	auto gsHandler = m_virtualMachine->GetGSHandler();
	if(!gsHandler)
	{
		m_virtualMachine->CreateGSHandler(CGSH_OpenGL_Libretro::GetFactoryFunction());
	}
	else
	{
		auto retro_gs = static_cast<CGSH_OpenGL_Libretro*>(gsHandler);
		retro_gs->Reset();
	}
}

static void retro_context_destroy()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

static void retro_context_reset()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		SetupVideoHandler();
	}
}

void SetupSoundHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		m_virtualMachine->CreateSoundHandler(&CSH_LibreAudio::HandlerFactory);
	}
}

void SetupInputHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(!m_virtualMachine->GetPadHandler())
	{
		static struct retro_input_descriptor descDS2[] =
		    {
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3 / Gun Calibrate"},
		        {0},
		    };

		g_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descDS2);

		static const struct retro_controller_description controllers[] = {
		    {"PS2 DualShock2", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)},
		    {"PS2 GunCon2", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0)},
		};

		static const struct retro_controller_info ports[] = {
		    {controllers, 2},
		    {NULL, 0},
		};

		g_environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

		for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
		{
			auto ds2_button = static_cast<PS2::CControllerInfo::BUTTON>(i);
			auto retro_button = descDS2[i].id;
			g_ds2_to_retro_btn_map[ds2_button] = retro_button;
		}

		m_virtualMachine->CreatePadHandler(CPH_Libretro_Input::GetFactoryFunction());
	}
}

void retro_get_system_info(struct retro_system_info* info)
{
	*info = {};
	info->library_name = "Play!";
	info->library_version = PLAY_VERSION;
	info->need_fullpath = true;
	info->valid_extensions = "elf|iso|cso|isz|cue|chd|bin";
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	*info = {};
	info->timing.fps = 60.0;
	info->timing.sample_rate = 44100;
	info->geometry.base_width = 640;
	info->geometry.base_height = 448;
	info->geometry.max_width = 640 * 8;
	info->geometry.max_height = 448 * 8;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_video_cb = cb;
}

void retro_set_environment(retro_environment_t cb)
{
	g_environ_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_input_state_cb = cb;
}

static bool matches_serial(const char* serial, const char* name) {
    while (*serial && *name) {
        if (isalnum(*name)) {
            if (toupper(*name) != *serial)
                return false;
            name++;
            serial++;
        }
        else {
            name++;
        }
    }
    return *serial == 0;
}

static void setup_lightgun(const char* name) {
    if (name != nullptr) {
        for (int i=0; i<sizeof(lightgun_games)/sizeof(*lightgun_games); i++) {
            if (matches_serial(lightgun_games[i].serial, name)) {
                lightgun_info = &(lightgun_games[i]);
                return;
            }
        }
    }
    
    lightgun_info = &lightgun_defaults;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_LIGHTGUN)
	{
		g_lightgun_port = static_cast<int>(port);
        if (m_virtualMachine) {
            setup_lightgun(m_virtualMachine->m_ee->m_os->GetExecutableName());
        }
	}
	else if(g_lightgun_port == static_cast<int>(port))
	{
		g_lightgun_port = -1;
	}
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_set_audio_sample_batch_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

unsigned retro_get_region(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return RETRO_REGION_NTSC;
}

size_t retro_serialize_size(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return 40 * 1024 * 1024;
}

bool retro_serialize(void* data, size_t size)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	try
	{
		Framework::CMemStream stateStream;
		Framework::CZipArchiveWriter archive;

		m_virtualMachine->m_ee->SaveState(archive);
		m_virtualMachine->m_iop->SaveState(archive);
		m_virtualMachine->m_ee->m_gs->SaveState(archive);

		archive.Write(stateStream);
		stateStream.Seek(0, Framework::STREAM_SEEK_DIRECTION::STREAM_SEEK_SET);
		stateStream.Read(data, size);
	}
	catch(...)
	{
		return false;
	}

	return true;
}

bool retro_unserialize(const void* data, size_t size)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	try
	{
		Framework::CPtrStream stateStream(data, size);
		Framework::CZipArchiveReader archive(stateStream);

		try
		{
			m_virtualMachine->m_ee->LoadState(archive);
			m_virtualMachine->m_iop->LoadState(archive);
			m_virtualMachine->m_ee->m_gs->LoadState(archive);
		}
		catch(...)
		{
			//Any error that occurs in the previous block is critical
			throw;
		}
	}
	catch(...)
	{
		return false;
	}

	m_virtualMachine->OnMachineStateChange();
	return true;
}

void* retro_get_memory_data(unsigned id)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(id == RETRO_MEMORY_SYSTEM_RAM)
	{
		return m_virtualMachine->m_ee->m_ram;
	}
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(id == RETRO_MEMORY_SYSTEM_RAM)
	{
		return PS2::EE_RAM_SIZE;
	}
	return 0;
}

void retro_cheat_reset(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	(void)index;
	(void)enabled;
	(void)code;
}

void updateVars()
{
	for(int i = 0; i < m_vars.size() - 1; ++i)
	{
		auto item = m_vars[i];
		if(!item.key)
			continue;

		struct retro_variable var = {nullptr};
		var.key = item.key;
		if(g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool videoUpdate = false;
			switch(i)
			{
			case 0:
			{
				std::string val = var.value;
				auto res_factor = std::atoi(val.substr(0, -1).c_str());
				if(res_factor != g_res_factor)
				{
					g_res_factor = res_factor;
					CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, res_factor);
					videoUpdate = true;
				}
			}
			break;
			case 1:
			{
				CGSHandler::PRESENTATION_MODE presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FIT;

				std::string val(var.value);
				if(val == "Fill Screen")
					presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FILL;
				else if(val == "Original Size")
					presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_ORIGINAL;

				if(presentation_mode != g_presentation_mode)
				{
					g_presentation_mode = presentation_mode;
					CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSHANDLER_PRESENTATION_MODE, presentation_mode);
					videoUpdate = true;
				}
			}
			break;
			case 2:
			{
				bool forceBilinearTextures = (std::string(var.value) == "true");
				if(forceBilinearTextures != g_forceBilinearTextures)
				{
					g_forceBilinearTextures = forceBilinearTextures;
					CAppConfig::GetInstance().SetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, forceBilinearTextures);
					videoUpdate = true;
				}
			}
			break;
			}

			if(videoUpdate)
			{
				if(m_virtualMachine)
					if(m_virtualMachine->GetGSHandler())
						static_cast<CGSH_OpenGL_Libretro*>(m_virtualMachine->GetGSHandler())->UpdatePresentation();
			}
		}
	}
}

void checkVarsUpdates()
{
	static bool updates = true;
	if(!updates)
		g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updates);

	if(updates)
	{
		updateVars();
	}
	updates = false;
}

// Polls RETRO_DEVICE_LIGHTGUN for `port` and pushes the result into
// CGunCon2UsbDevice. Screen coordinates come back from RetroArch as
// RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X/Y, full range [-0x8000, 0x7fff] mapped
// to the visible display area; we rescale it to the appropriate game.
static void UpdateGunConInputState(unsigned port)
{
	auto iopOs = dynamic_cast<CIopBios*>(m_virtualMachine->m_iop->m_bios.get());
	if(!iopOs) return;

	auto device = iopOs->GetUsbd()->GetDevice<Iop::CGunCon2UsbDevice>();
	if(!device) return;

	bool offscreen = g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0;
	bool trigger = g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) != 0;
	bool reload = g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) != 0;

	uint32 buttonMask = 0;
	if(trigger && !offscreen) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_TRIGGER);
	if((trigger || reload) && offscreen) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_SHOOT_OFFSCREEN);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_START);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SELECT)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_SELECT);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_A);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_B);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_C)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_C);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_DPAD_UP);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_DPAD_DOWN);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_DPAD_LEFT);
	if(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_DPAD_RIGHT);
	if(g_input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3)) buttonMask |= (1 << Iop::CGunCon2UsbDevice::BUTTON_CALIBRATE);

	if(offscreen)
	{
		// (0, 0) is the protocol's reserved offscreen position.
		device->SetGunState(0, 0, buttonMask);
		return;
	}

	int32 screenX = static_cast<int32>(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X));
	int32 screenY = static_cast<int32>(g_input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y));
    
	int32 x = ( (screenX * lightgun_info->width) / 0x100 * lightgun_info->scale_x + 0x100 * 5000) / (0x100 * 10000)
                    + lightgun_info->center_x;
	int32 y = ( (screenY * lightgun_info->height) / 0x100 * lightgun_info->scale_y + 0x100 * 5000) / (0x100 * 10000)
                    + lightgun_info->center_y;

	// Avoid colliding with the reserved (0,0) offscreen sentinel for an
	// on-screen shot that happens to land exactly at the top-left pixel.
	if(x == 0 && y == 0) x = 1;

	device->SetGunState(x, y, buttonMask);
}

void retro_run()
{
	// CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	checkVarsUpdates();

	if(!first_run)
	{
		if(m_virtualMachine)
		{
			// m_virtualMachine->Pause();
			m_virtualMachine->Reset();
			if(m_bootCommand.type == BootType::CD)
			{
				m_virtualMachine->m_ee->m_os->BootFromCDROM();
			}
			else
			{
				m_virtualMachine->m_ee->m_os->BootFromFile(m_bootCommand.path);
			}
			m_virtualMachine->Resume();
			first_run = true;
			CLog::GetInstance().Print(LOG_NAME, "%s\n", "Start Game");
            if (g_lightgun_port >= 0)
                setup_lightgun(m_virtualMachine->m_ee->m_os->GetExecutableName());
		}
	}

	if(m_virtualMachine)
	{
		auto pad = m_virtualMachine->GetPadHandler();
		if(pad)
			static_cast<CPH_Libretro_Input*>(pad)->UpdateInputState();

		if(g_lightgun_port >= 0)
		{
			UpdateGunConInputState(static_cast<unsigned>(g_lightgun_port));
		}

		if(m_virtualMachine->GetSoundHandler())
			static_cast<CSH_LibreAudio*>(m_virtualMachine->GetSoundHandler())->ProcessBuffer();

		if(m_virtualMachine->GetGSHandler())
			m_virtualMachine->GetGSHandler()->ProcessSingleFrame();
	}
}

void retro_reset(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		if(!m_virtualMachine->GetGSHandler())
			SetupVideoHandler();
		// m_virtualMachine->Pause();
		m_virtualMachine->Reset();
		m_virtualMachine->m_ee->m_os->BootFromCDROM();
		m_virtualMachine->Resume();
		CLog::GetInstance().Print(LOG_NAME, "%s\n", "Reset Game");
	}
	first_run = false;
}

bool retro_load_game(const retro_game_info* info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

#if defined(IOS)
	bool can_jit = false;
	if(g_environ_cb(RETRO_ENVIRONMENT_GET_JIT_CAPABLE, &can_jit) && !can_jit)
	{
		// trying to run without the jit will cause a crash.
		retro_message retromsg;
		retromsg.msg = "Cannot run without JIT";
		retromsg.frames = 5000 / 17;
		g_environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &retromsg);
		return false;
	}
#endif

	fs::path filePath = info->path;
	if(BootableUtils::IsBootableExecutablePath(filePath))
	{
		m_bootCommand = LastOpenCommand(BootType::ELF, filePath);
	}
	else if(BootableUtils::IsBootableDiscImagePath(filePath))
	{
		m_bootCommand = LastOpenCommand(BootType::CD, filePath);
		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, filePath);
		CAppConfig::GetInstance().Save();
	}
	first_run = false;

	auto rgb = RETRO_PIXEL_FORMAT_XRGB8888;
	g_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb);

#ifdef GLES_COMPATIBILITY
	g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
#else
	g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
#endif

	g_hw_render.version_major = 3;
	g_hw_render.version_minor = 2;
	g_hw_render.context_reset = retro_context_reset;
	g_hw_render.context_destroy = retro_context_destroy;
	g_hw_render.cache_context = false;
	g_hw_render.bottom_left_origin = true;
	g_hw_render.depth = true;
	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render);

	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

	g_environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)m_vars.data());

	return true;
}

void retro_unload_game(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return false;
}

void retro_init()
{
#ifdef __ANDROID__
	Framework::PathUtils::SetFilesDirPath(getenv("EXTERNAL_STORAGE"));
#endif
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(g_environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
		libretro_supports_bitmasks = true;

	CAppConfig::GetInstance().RegisterPreferenceInteger(PREF_AUDIO_SPUBLOCKCOUNT, 22);

	m_virtualMachine = new CPS2VM();
	m_virtualMachine->Initialize();

	//Disable frame limiter, RetroArch handles this on its own
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, false);
	m_virtualMachine->ReloadFrameRateLimit();

	SetupInputHandler();
	SetupSoundHandler();
	first_run = false;
}

void retro_deinit()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		m_virtualMachine->PauseAsync();
		auto gsHandler = static_cast<CGSH_OpenGL_Libretro*>(m_virtualMachine->GetGSHandler());
		if(gsHandler)
		{
			// Note: since we've forced GS into running on this/main/libretro thread
			// we need to clear its queue, to prevent it from locking up VM
			while(m_virtualMachine->GetStatus() != CVirtualMachine::PAUSED)
			{
				std::this_thread::yield();
				gsHandler->Release();
			}
		}
		m_virtualMachine->DestroyPadHandler();
		m_virtualMachine->DestroyGSHandler();
		m_virtualMachine->DestroySoundHandler();
		m_virtualMachine->Destroy();
		delete m_virtualMachine;
		m_virtualMachine = nullptr;
	}
	libretro_supports_bitmasks = false;
}
