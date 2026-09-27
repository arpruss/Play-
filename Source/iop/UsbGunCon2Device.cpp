#include "UsbGunCon2Device.h"
#include "UsbDefs.h"
#include "IopBios.h"
#include "Ps2Const.h"
#include "states/RegisterState.h"

using namespace Iop;

#define STATE_REG_DESCRIPTORMEMPTR ("descriptorMemPtr")
#define STATE_REG_NEXTTRANSFERTICKS ("nextTransferTicks")
#define STATE_REG_TRANSFERBUFFERPTR ("transferBufferPtr")
#define STATE_REG_TRANSFERSIZE ("transferSize")
#define STATE_REG_TRANSFERCB ("transferCb")
#define STATE_REG_TRANSFERCBARG ("transferCbArg")

// Namco GunCon 2 (real hardware IDs, from PCSX2's usb-lightgun/guncon2.cpp)
#define GUNCON2_VENDOR_ID (0x0b9a)
#define GUNCON2_PRODUCT_ID (0x016a)

CGunCon2UsbDevice::CGunCon2UsbDevice(CIopBios& bios, uint8* ram)
    : m_bios(bios)
    , m_ram(ram)
{
}

void CGunCon2UsbDevice::SaveState(CRegisterState& state) const
{
	state.SetRegister32(STATE_REG_DESCRIPTORMEMPTR, m_descriptorMemPtr);
	state.SetRegister32(STATE_REG_NEXTTRANSFERTICKS, m_nextTransferTicks);
	state.SetRegister32(STATE_REG_TRANSFERBUFFERPTR, m_transferBufferPtr);
	state.SetRegister32(STATE_REG_TRANSFERSIZE, m_transferSize);
	state.SetRegister32(STATE_REG_TRANSFERCB, m_transferCb);
	state.SetRegister32(STATE_REG_TRANSFERCBARG, m_transferCbArg);
}

void CGunCon2UsbDevice::LoadState(const CRegisterState& state)
{
	m_descriptorMemPtr = state.GetRegister32(STATE_REG_DESCRIPTORMEMPTR);
	m_nextTransferTicks = state.GetRegister32(STATE_REG_NEXTTRANSFERTICKS);
	m_transferBufferPtr = state.GetRegister32(STATE_REG_TRANSFERBUFFERPTR);
	m_transferSize = state.GetRegister32(STATE_REG_TRANSFERSIZE);
	m_transferCb = state.GetRegister32(STATE_REG_TRANSFERCB);
	m_transferCbArg = state.GetRegister32(STATE_REG_TRANSFERCBARG);
}

uint16 CGunCon2UsbDevice::GetId() const
{
	return DEVICE_ID;
}

const char* CGunCon2UsbDevice::GetLldName() const
{
	return "usbgun"; // guncon2";
}

void CGunCon2UsbDevice::Send()
{
    uint8* buffer = m_ram + m_transferBufferPtr;
    // GunCon2Out report: u16 buttons (active low), s16 pos_x, s16 pos_y — all little-endian.
    uint16 buttons = static_cast<uint16>(~m_buttonState);
    uint32_t x = m_posX;
    uint32_t y = m_posY;

    if ((m_buttonState & (1U << BUTTON_CALIBRATE)) && m_calibration_timer == 0) {
        buttons &= ~(1U << BUTTON_TRIGGER);        
        m_calibration_timer = 12;
    }
    else if (m_calibration_timer > 0) {
        buttons &= ~(1U << BUTTON_TRIGGER);
        if (m_calibration_timer < 5) {
            x = 0;
            y = 0;
        }
        m_calibration_timer--;
    }
    buffer[0] = static_cast<uint8>(buttons & 0xFF);
    buffer[1] = static_cast<uint8>((buttons >> 8) & 0xFF);
    buffer[2] = static_cast<uint8>(x & 0xFF);
    buffer[3] = static_cast<uint8>((x >> 8) & 0xFF);
    buffer[4] = static_cast<uint8>(y & 0xFF);
    buffer[5] = static_cast<uint8>((y >> 8) & 0xFF);
    m_bios.TriggerCallback(m_transferCb, 0, m_transferSize, m_transferCbArg);
    m_nextTransferTicks = 0;
    m_transferCb = 0;
}

void CGunCon2UsbDevice::CountTicks(uint32 ticks)
{
	if(m_nextTransferTicks != 0)
	{
		m_nextTransferTicks -= ticks;
		if(m_nextTransferTicks <= 0) Send();
	}
}

void CGunCon2UsbDevice::SetGunState(int32 x, int32 y, uint32 buttonMask)
{
	// (0, 0) is reserved by the protocol for "offscreen" — matches libretro's
	// RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN convention closely enough that the
	// caller can just clamp to (0,0) for an offscreen shot.
	m_posX = static_cast<int16>(x);
	m_posY = static_cast<int16>(y);
	m_buttonState = buttonMask;
}

void CGunCon2UsbDevice::OnLldRegistered()
{
	m_descriptorMemPtr = m_bios.GetSysmem()->AllocateMemory(0x80, 0, 0);
}

uint32 CGunCon2UsbDevice::ScanStaticDescriptor(uint32 deviceId, uint32 descriptorPtr, uint32 descriptorType)
{
	assert(deviceId == DEVICE_ID);
	uint32 result = 0;
	switch(descriptorType)
	{
	case Usb::DESCRIPTOR_TYPE_DEVICE:
	{
		auto descriptor = reinterpret_cast<Usb::DEVICE_DESCRIPTOR*>(m_ram + m_descriptorMemPtr);
		descriptor->base.descriptorType = Usb::DESCRIPTOR_TYPE_DEVICE;
		descriptor->vendorId = GUNCON2_VENDOR_ID;
		descriptor->productId = GUNCON2_PRODUCT_ID;
		result = m_descriptorMemPtr;
	}
	break;
	case Usb::DESCRIPTOR_TYPE_CONFIGURATION:
	{
		auto descriptor = reinterpret_cast<Usb::CONFIGURATION_DESCRIPTOR*>(m_ram + m_descriptorMemPtr);
		descriptor->base.descriptorType = Usb::DESCRIPTOR_TYPE_CONFIGURATION;
		descriptor->numInterfaces = 1;
		result = m_descriptorMemPtr;
	}
	break;
	case Usb::DESCRIPTOR_TYPE_INTERFACE:
	{
		auto descriptor = reinterpret_cast<Usb::INTERFACE_DESCRIPTOR*>(m_ram + m_descriptorMemPtr);
		descriptor->base.descriptorType = Usb::DESCRIPTOR_TYPE_INTERFACE;
		descriptor->numEndpoints = 1;
		result = m_descriptorMemPtr;
	}
	break;
	case Usb::DESCRIPTOR_TYPE_ENDPOINT:
	{
		auto descriptor = reinterpret_cast<Usb::ENDPOINT_DESCRIPTOR*>(m_ram + m_descriptorMemPtr);
		if(descriptor->base.descriptorType != Usb::DESCRIPTOR_TYPE_ENDPOINT)
		{
			descriptor->base.descriptorType = Usb::DESCRIPTOR_TYPE_ENDPOINT;
			descriptor->endpointAddress = 0x81;
			descriptor->attributes = 3; //Interrupt transfer type
			descriptor->maxPacketSize = 8;
			result = m_descriptorMemPtr;
		}
	}
	break;
	}
	return result;
}

int32 CGunCon2UsbDevice::OpenPipe(uint32 deviceId, uint32 descriptorPtr)
{
	assert(deviceId == DEVICE_ID);
	if(descriptorPtr != 0)
	{
		assert(descriptorPtr == m_descriptorMemPtr);
		return PIPE_ID;
	}
	else
	{
		return CONTROL_PIPE_ID;
	}
}

int32 CGunCon2UsbDevice::TransferPipe(uint32 pipeId, uint32 bufferPtr, uint32 size, uint32 optionPtr, uint32 doneCb, uint32 arg)
{
    uint16 deviceId = (pipeId & 0xFFFF);
	uint16 internalPipeId = (pipeId >> 16) & 0xFFF;
	assert(deviceId == DEVICE_ID);

	switch(internalPipeId)
	{
	case CONTROL_PIPE_ID:
		m_bios.TriggerCallback(doneCb, 0, size, arg);
		return 0;
		break;
	case PIPE_ID:
		//Interrupt transfer
		m_transferBufferPtr = bufferPtr;
		m_transferSize = size;
		m_transferCb = doneCb;
		m_transferCbArg = arg;
		m_nextTransferTicks = PS2::IOP_CLOCK_OVER_FREQ / 60;
		return 0;
	default:
		assert(false);
		return -1;
	}
}
