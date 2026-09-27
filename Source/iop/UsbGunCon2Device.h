#pragma once

#include "UsbDevice.h"

class CIopBios;

namespace Iop
{
	// Namco GunCon 2 (VID 0x0b9a, PID 0x016a) — a USB HID-ish interrupt device,
	// not an SIO2 pad. Report format (6 bytes: u16 buttons active-low, s16 x,
	// s16 y) confirmed against PCSX2's usb-lightgun/guncon2.cpp (GPL, public
	// source, real hardware). Modeled on UsbBuzzerDevice.h/.cpp, the one other
	// USB peripheral this project already emulates.
	//
	// UNVERIFIED: the "guncon2" LLD name below is what a real game's IOP-side
	// driver must sceUsbdRegisterLld() with for CUsbd::RegisterLld() to bind
	// this device (see Iop_Usbd.cpp). No PS2 GunCon2-compatible game ISO was
	// available to confirm the exact string a real driver blob uses — this
	// compiles and is structurally correct, but is untested end-to-end.
	class CGunCon2UsbDevice : public CUsbDevice
	{
	public:
		enum
		{
			DEVICE_ID = 0x6762, // arbitrary, just needs to be unique among registered devices
			CONTROL_PIPE_ID = 0x321,
			PIPE_ID = 0x654,
		};

		// Button bit layout matches PCSX2's GunCon2 BID_* enum so the report is
		// byte-for-byte comparable against a known-working real-hardware emulation.
		enum BUTTON
		{
			BUTTON_C = 1,
			BUTTON_B = 2,
			BUTTON_A = 3,
			BUTTON_DPAD_UP = 4,
			BUTTON_DPAD_RIGHT = 5,
			BUTTON_DPAD_DOWN = 6,
			BUTTON_DPAD_LEFT = 7,
            BUTTON_PROGRESSIVE = 8,
			BUTTON_TRIGGER = 13,
			BUTTON_SELECT = 14,
			BUTTON_START = 15,
			BUTTON_SHOOT_OFFSCREEN = 16,
            BUTTON_CALIBRATE = 17
		};

		CGunCon2UsbDevice(CIopBios&, uint8*);

		uint16 GetId() const override;
		const char* GetLldName() const override;

		void SaveState(CRegisterState&) const override;
		void LoadState(const CRegisterState&) override;

		void CountTicks(uint32) override;
		void Send();

		void OnLldRegistered() override;
		uint32 ScanStaticDescriptor(uint32, uint32, uint32) override;
		int32 OpenPipe(uint32, uint32) override;
		int32 TransferPipe(uint32, uint32, uint32, uint32, uint32, uint32) override;

		// Called from the libretro RETRO_DEVICE_LIGHTGUN poll (main_libretro.cpp),
		// once per retro_run, for whichever port is bound to this device. x/y are
		// screen-space pixel coordinates (0,0 = offscreen, matching GunCon2Out's
		// convention); buttonMask is an OR of (1 << BUTTON_*).
		void SetGunState(int32 x, int32 y, uint32 buttonMask);

	private:
		CIopBios& m_bios;
		uint8* m_ram = nullptr;

		uint32 m_descriptorMemPtr = 0;
		int32 m_nextTransferTicks = 0;
		uint32 m_transferBufferPtr = 0;
		uint32 m_transferSize = 0;
		uint32 m_transferCb = 0;
		uint32 m_transferCbArg = 0;
        int32 m_calibration_timer = 0;

		int16 m_posX = 0;
		int16 m_posY = 0;
		uint32 m_buttonState = 0;
	};
}
