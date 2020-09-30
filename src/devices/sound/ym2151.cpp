// license:BSD-3-Clause
// copyright-holders:Aaron Giles

#include "emu.h"
#include "ym2151.h"


DEFINE_DEVICE_TYPE(YM2151, ym2151_device, "ym2151", "YM2151 OPM")
DEFINE_DEVICE_TYPE(YM2164, ym2164_device, "ym2164", "YM2164 OPP")
DEFINE_DEVICE_TYPE(YM2414, ym2414_device, "ym2414", "YM2414 OPZ")


//*********************************************************
//  INLINE HELPERS
//*********************************************************

//-------------------------------------------------
//  linear_to_fp - given a 32-bit signed input
//  value, convert it to a signed 10.3 floating-
//  point value
//-------------------------------------------------

inline s16 linear_to_fp(s32 value)
{
	// start with the absolute value
	s32 avalue = (value < 0) ? -value : value;
	u8 shift = 0;

	// shift until absolute value fits in 9 bits (bit 10 is the sign)
	while (avalue != (avalue & 0x1ff))
	{
		avalue >>= 1;
		shift++;
	}

	// if out of range, just return maximum; note that YM3012 DAC does
	// not support a shift count of 7, so we clamp at 6
	if (shift >= 7)
		shift = 6, avalue = 0x1ff;

	// encode with shift in low 3 bits and signed mantissa in upper
	return shift | (((value < 0) ? -avalue : avalue) << 3);
}


//-------------------------------------------------
//  fp_to_linear - given a 10.3 floating-point
//  value, convert it to a signed 16-bit value,
//  clamping
//-------------------------------------------------

inline stream_buffer::sample_t fp_to_linear(s16 value)
{
	s32 result = (value >> 3) << BIT(value, 0, 3);
	if (result < -32768)
		result = -32768;
	else if (result > 32767)
		result = 32767;
	return stream_buffer::sample_t(result) * (1.0 / 32768.0);
}



//*********************************************************
//  YM2151 DEVICE
//*********************************************************

//-------------------------------------------------
//  ym2151_device - constructor
//-------------------------------------------------

ym2151_device::ym2151_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, device_type type) :
	device_t(mconfig, type, tag, owner, clock),
	device_sound_interface(mconfig, *this),
	m_opm(*this),
	m_stream(nullptr),
	m_port_w(*this),
	m_address(0),
	m_reset_state(1)
{
}


//-------------------------------------------------
//  read - handle a read from the device
//-------------------------------------------------

u8 ym2151_device::read(offs_t offset)
{
	u8 result = 0xff;
	switch (offset & 1)
	{
		case 0: // data port (unused)
			printf("Unexpected read from YM2151 offset %d\n", offset & 3);
			break;

		case 1:	// status port, YM2203 compatible
			result = m_opm.status();
			break;
	}
	return result;
}


//-------------------------------------------------
//  write - handle a write to the register
//  interface
//-------------------------------------------------

void ym2151_device::write(offs_t offset, u8 value)
{
	// ignore writes when the reset is active (low)
	if (m_reset_state == 0)
		return;

	switch (offset & 1)
	{
		case 0:	// address port
			m_address = value;
			break;

		case 1: // data port

			// force an update
			m_stream->update();

			// write to OPN
			m_opm.write(m_address, value);

			// writes to the test register can reset the LFO
			if (m_address == 0x01 && BIT(value, 1))
				m_opm.reset_lfo();

			// writes to register 0x1B send the upper 2 bits
			else if (m_address == 0x1b)
				m_port_w(0, value >> 6, 0xff);

			// mark busy for a bit
			m_opm.set_busy();
			break;
	}
}


//-------------------------------------------------
//  reset_w - write to the (active low) reset line
//-------------------------------------------------

WRITE_LINE_MEMBER(ym2151_device::reset_w)
{
	// reset the device upon going low
	if (state == 0 && m_reset_state != 0)
		reset();
	m_reset_state = state;
}


//-------------------------------------------------
//  device_start - start of emulation
//-------------------------------------------------

void ym2151_device::device_start()
{
	// create our stream
	m_stream = stream_alloc(0, 2, clock() / (2 * 4 * 8));

	// resolve the write callback
	m_port_w.resolve_safe();

	// call this for the variants that need to adjust the rate
	device_clock_changed();

	// save our data
	save_item(YMFM_NAME(m_address));
	save_item(YMFM_NAME(m_reset_state));

	// save the the engines
	m_opm.save(*this);
}


//-------------------------------------------------
//  device_reset - start of emulation
//-------------------------------------------------

void ym2151_device::device_reset()
{
	// reset the the engines
	m_opm.reset();
}


//-------------------------------------------------
//  device_clock_changed - update if clock changes
//-------------------------------------------------

void ym2151_device::device_clock_changed()
{
	m_stream->set_sample_rate(clock() / (2 * 4 * 8));
}


//-------------------------------------------------
//  sound_stream_update - update the sound stream
//-------------------------------------------------

void ym2151_device::sound_stream_update(sound_stream &stream, std::vector<read_stream_view> const &inputs, std::vector<write_stream_view> &outputs)
{
	// iterate over all target samples
	for (int sampindex = 0; sampindex < outputs[0].samples(); sampindex++)
	{
		// clock the system
		m_opm.clock(0xff);

		// update the OPN content; OPN is full 14-bit with no intermediate clipping
		s32 lsum = 0, rsum = 0;
		m_opm.output(lsum, rsum, 0, 32767, 0xff);

		// convert to 10.3 floating point value for the DAC and back
		// OPN is mono, so only the left sum matters
		outputs[0].put(sampindex, fp_to_linear(linear_to_fp(lsum)));
		outputs[1].put(sampindex, fp_to_linear(linear_to_fp(rsum)));
	}
}



//*********************************************************
//  YM2164 DEVICE
//*********************************************************

//-------------------------------------------------
//  ym2164_device - constructor
//-------------------------------------------------

ym2164_device::ym2164_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	ym2151_device(mconfig, tag, owner, clock, YM2164)
{
}


//-------------------------------------------------
//  write - handle a write to the register
//  interface
//-------------------------------------------------

void ym2164_device::write(offs_t offset, u8 value)
{
	// ignore writes when the reset is active (low)
	if (m_reset_state == 0)
		return;

	switch (offset & 1)
	{
		case 0:	// address port
			m_address = value;
			break;

		case 1: // data port

			// force an update
			m_stream->update();

			// write to OPN
			m_opm.write(m_address, value);

			// writes to register 0x1B send the upper 2 bits
			m_port_w(0, value >> 6, 0xff);

			// mark busy for a bit
			m_opm.set_busy();
			break;
	}
}



//*********************************************************
//  YM2151 DEVICE
//*********************************************************

//-------------------------------------------------
//  ym2414_device - constructor
//-------------------------------------------------

ym2414_device::ym2414_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	ym2151_device(mconfig, tag, owner, clock, YM2414)
{
}
