// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

Killer Instinct 1 & 2 multi-game hardware

Based on rare/kinst.cpp by Aaron Giles and Bryan McPhail.

Runs both Killer Instinct games from a single ROM set and a single unified
hard disk image.  The running game is selected at runtime: the software
issues IDE command 0xf1 with the SECTOR_COUNT register holding 1 (Killer
Instinct) or 2 (Killer Instinct 2), and the driver copies the matching boot
ROM and DCS sound data into the regions the hardware reads.  Any other
SECTOR_COUNT value aborts the command and changes nothing.

This machine is built for modified ROMs and a custom disk image:
- the boot ROMs apply their own offset into the unified disk image, so no
  address translation happens here and there is one DISK_IMAGE
- the Killer Instinct 2 boot ROM is patched to use Killer Instinct's control
  register layout, so there is only one address map
- both boot ROMs accept any drive, so the IDENTIFY buffer fakery in kinst.cpp
  is not needed here
- the ROM jumps to the new game itself after the swap, so nothing is reset
  except the sound CPU
- the code that issues 0xf1 and polls for completion must be byte-identical
  at the same address in both boot ROMs, or must execute from RAM -- the
  swap replaces the instructions under the PC, and execution resumes at the
  same address in the new ROM

***************************************************************************/

#include "emu.h"
#include "dcs.h"

#include "bus/ata/ataintf.h"
#include "bus/ata/hdd.h"
#include "cpu/adsp2100/adsp2100.h"
#include "cpu/mips/mips3.h"

#include "emupal.h"
#include "screen.h"
#include "speaker.h"

#include <algorithm>


DECLARE_DEVICE_TYPE(KINST_MULTI_HDD, ide_hdd_device)


namespace {

/*************************************
 *
 *  Game selector hard disk
 *
 *  Adds vendor command 0xf1: select the game named by the SECTOR_COUNT
 *  register.  Any other value aborts, which the base class does for us.
 *
 *************************************/

class kinst_multi_hdd_device : public ide_hdd_device
{
public:
	kinst_multi_hdd_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	auto game_select_cb() { return m_game_select_cb.bind(); }

protected:
	virtual void device_start() override ATTR_COLD;

	virtual void process_command() override;
	virtual void finished_command() override;

private:
	// 0xf1 is SECURITY SET PASSWORD from ATA-3 onward; unimplemented by MAME's
	// HLE today (atahle.h has 0xf2/0xf6 but no 0xf1), so this is safe for now,
	// but if upstream ever adds 0xf1 this interception would silently shadow it
	static inline constexpr uint8_t IDE_COMMAND_SELECT_GAME = 0xf1;

	devcb_write8 m_game_select_cb;
	uint8_t m_pending_game = 0;
};


kinst_multi_hdd_device::kinst_multi_hdd_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	ide_hdd_device(mconfig, KINST_MULTI_HDD, tag, owner, clock),
	m_game_select_cb(*this)
{
}


void kinst_multi_hdd_device::device_start()
{
	ide_hdd_device::device_start();

	save_item(NAME(m_pending_game));
}


void kinst_multi_hdd_device::process_command()
{
	if (m_command == IDE_COMMAND_SELECT_GAME)
	{
		// SECTOR_COUNT selects the game: 1 = Killer Instinct, 2 = Killer Instinct 2
		if (m_sector_count == 1 || m_sector_count == 2)
		{
			m_pending_game = m_sector_count - 1;
			start_busy(MINIMUM_COMMAND_TIME, PARAM_COMMAND);
			return;
		}

		// anything else falls through; the base class aborts unknown commands
	}

	ide_hdd_device::process_command();
}


void kinst_multi_hdd_device::finished_command()
{
	if (m_command == IDE_COMMAND_SELECT_GAME)
	{
		m_game_select_cb(m_pending_game);

		m_status |= IDE_STATUS_DRDY;
		ata_hle_device_base::set_irq(ASSERT_LINE);
		return;
	}

	ide_hdd_device::finished_command();
}


static void kinst_multi_ata_devices(device_slot_interface &device)
{
	device.option_add("hdd", KINST_MULTI_HDD);
}


class kinst_multi_state : public driver_device
{
public:
	kinst_multi_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_ata(*this, "ata"),
		m_dcs(*this, "dcs"),
		m_palette(*this, "palette"),
		m_rambase(*this, "rambase"),
		m_rambase2(*this, "rambase2"),
		m_rombase(*this, "user1"),
		m_bootrom(*this, "user1"),
		m_dcsrom(*this, "dcs"),
		m_boot_src(*this, "boot_ki%u", 1U),
		m_dcs_src(*this, "dcs_ki%u", 1U)
	{
	}

	void kinstmulti(machine_config &config);

	ioport_value sound_status_r() { return BIT(m_dcs->control_r(), 11); }

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;
	virtual void device_post_load() override;

private:
	required_device<mips3_device> m_maincpu;
	required_device<ata_interface_device> m_ata;
	required_device<dcs_audio_2k_device> m_dcs;
	required_device<palette_device> m_palette;

	required_shared_ptr<uint32_t> m_rambase;
	required_shared_ptr<uint32_t> m_rambase2;
	required_region_ptr<uint32_t> m_rombase;

	// active regions the hardware reads
	required_memory_region m_bootrom;
	required_memory_region m_dcsrom;

	// sources copied into the active regions; index 0 = KI, 1 = KI2
	required_memory_region_array<2> m_boot_src;
	required_memory_region_array<2> m_dcs_src;

	void kinst_map(address_map &map) ATTR_COLD;

	uint32_t screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);

	uint32_t ide_r(offs_t offset, uint32_t mem_mask = ~0);
	void ide_w(offs_t offset, uint32_t data, uint32_t mem_mask = ~0);
	uint32_t ide_extra_r();
	void ide_extra_w(uint32_t data);
	uint32_t rom_r(offs_t offset);

	void vram_control_w(offs_t offset, uint32_t data, uint32_t mem_mask);
	void sound_reset_w(offs_t offset, uint32_t data, uint32_t mem_mask);
	void sound_control_w(offs_t offset, uint32_t data, uint32_t mem_mask);
	void sound_data_w(offs_t offset, uint32_t data, uint32_t mem_mask);
	void coin_control_w(offs_t offset, uint32_t data, uint32_t mem_mask);

	void game_select_w(uint8_t data);
	void apply_game_select(bool restart_sound);

	uint32_t *m_video_base = nullptr;

	uint32_t m_vram_control = 0;
	uint32_t m_sound_reset = 0;
	uint32_t m_sound_control = 0;
	uint32_t m_sound_data = 0;
	uint32_t m_coin_control = 0;

	uint8_t m_game_select = 0;
};



/*************************************
 *
 *  Machine start / reset
 *
 *************************************/

void kinst_multi_state::machine_start()
{
	// set the fastest DRC options
	m_maincpu->mips3drc_set_options(MIPS3DRC_FASTEST_OPTIONS);

	// configure fast RAM regions
	m_maincpu->add_fastram(0x08000000, 0x087fffff, false, m_rambase2);
	m_maincpu->add_fastram(0x00000000, 0x0007ffff, false, m_rambase);

	// The active regions were already primed with Killer Instinct by ROM_COPY at
	// ROM load time -- they have to be, because the DCS boots the ADSP-2105 out
	// of the "dcs" region during its own device_start(), long before we get here.
	// This re-copy is therefore a no-op on a cold boot; it keeps the regions and
	// m_game_select consistent unconditionally. Do not reset the DCS from here.
	apply_game_select(false);

	// register for savestates
	save_item(NAME(m_vram_control));
	save_item(NAME(m_sound_reset));
	save_item(NAME(m_sound_control));
	save_item(NAME(m_sound_data));
	save_item(NAME(m_coin_control));
	save_item(NAME(m_game_select));
}


void kinst_multi_state::machine_reset()
{
	// set a safe base location for video
	m_video_base = &m_rambase[0x30000/4];
}


void kinst_multi_state::device_post_load()
{
	// m_video_base is a host pointer derived from m_vram_control; it cannot be
	// saved directly and machine_reset()/vram_control_w() do not run on a load
	m_video_base = &m_rambase[(m_vram_control & 4) ? 0x58000/4 : 0x30000/4];

	// ROM region contents are not part of a save state, so restore them to match
	// the selection that was saved; the DCS state was already restored, leave it alone
	apply_game_select(false);
}



/*************************************
 *
 *  Game selection
 *
 *************************************/

void kinst_multi_state::game_select_w(uint8_t data)
{
	if (data == m_game_select)
		return;

	m_game_select = data;
	apply_game_select(true);
}


void kinst_multi_state::apply_game_select(bool restart_sound)
{
	// game_select_w only ever stores 0 or 1, but device_post_load() calls us
	// with whatever m_game_select a (possibly hand-edited or corrupt) state
	// file contained; object_array_finder::operator[] only asserts in debug
	// builds, so clamp here to avoid an out-of-bounds region access in release
	int const game = (m_game_select < m_boot_src.size()) ? m_game_select : 0;

	// hold the ADSP-2105 in reset across the copy
	if (restart_sound)
		m_dcs->reset_w(0);

	// bound by both lengths: today every source/destination pair is the same
	// size, but nothing enforces that if a region is resized later
	std::copy_n(m_boot_src[game]->base(), std::min(m_boot_src[game]->bytes(), m_bootrom->bytes()), m_bootrom->base());
	std::copy_n(m_dcs_src[game]->base(), std::min(m_dcs_src[game]->bytes(), m_dcsrom->bytes()), m_dcsrom->base());

	// blocks compiled from the boot ROM window are never revalidated by the DRC
	m_maincpu->mips3drc_flush_cache();

	// re-runs dcs_boot() from the new sound data at the next sync point
	if (restart_sound)
		m_dcs->reset_w(1);

	// NOTE: this forced reset pulse bypasses m_sound_reset (normally kept in
	// sync by sound_reset_w()); in the expected flow the new boot ROM rewrites
	// that register during its own init, so the shadow is theoretical only
}



/*************************************
 *
 *  Video refresh
 *
 *************************************/

uint32_t kinst_multi_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	pen_t const *const pen = m_palette->pens();

	// loop over rows and copy to the destination
	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		uint32_t const *src = &m_video_base[640/4 * y];
		uint32_t *dest = &bitmap.pix(y, cliprect.min_x);

		// loop over columns
		for (int x = cliprect.min_x; x < cliprect.max_x; x += 2)
		{
			uint32_t const data = *src++;

			// store two pixels
			*dest++ = pen[(data >>  0) & 0x7fff];
			*dest++ = pen[(data >> 16) & 0x7fff];
		}
	}
	return 0;
}



/*************************************
 *
 *  IDE controller access
 *
 *************************************/

uint32_t kinst_multi_state::ide_r(offs_t offset, uint32_t mem_mask)
{
	return m_ata->cs0_r(offset / 2, mem_mask);
}


void kinst_multi_state::ide_w(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	m_ata->cs0_w(offset / 2, data, mem_mask);
}


uint32_t kinst_multi_state::ide_extra_r()
{
	return m_ata->cs1_r(6, 0xff);
}


void kinst_multi_state::ide_extra_w(uint32_t data)
{
	m_ata->cs1_w(6, data, 0xff);
}



/*************************************
 *
 *  Control handling
 *
 *************************************/

void kinst_multi_state::vram_control_w(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	COMBINE_DATA(&m_vram_control);
	if (m_vram_control & 4)
		m_video_base = &m_rambase[0x58000/4];
	else
		m_video_base = &m_rambase[0x30000/4];
}


void kinst_multi_state::sound_reset_w(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	COMBINE_DATA(&m_sound_reset);
	m_dcs->reset_w(m_sound_reset & 0x01);
}


void kinst_multi_state::sound_control_w(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	uint32_t prev = m_sound_control;
	COMBINE_DATA(&m_sound_control);

	if (!(prev & 0x02) && (m_sound_control & 0x02))
		m_dcs->data_w(m_sound_data);
}


void kinst_multi_state::sound_data_w(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	COMBINE_DATA(&m_sound_data);
}


void kinst_multi_state::coin_control_w(offs_t offset, uint32_t data, uint32_t mem_mask)
{
	COMBINE_DATA(&m_coin_control);
	// d0: coincounter? (only on coin slot 1,2)
}


uint32_t kinst_multi_state::rom_r(offs_t offset)
{
	// add RdRdy clocks on EPROM access
	// bootup sequence takes approx. 6 seconds, and it's not a CPU clock divider
	if (!machine().side_effects_disabled())
		m_maincpu->adjust_icount(-128);

	return m_rombase[offset];
}



/*************************************
 *
 *  Main CPU memory handlers
 *
 *************************************/

void kinst_multi_state::kinst_map(address_map &map)
{
	map.unmap_value_high();
	map(0x00000000, 0x0007ffff).ram().share(m_rambase);
	map(0x08000000, 0x087fffff).ram().share(m_rambase2);

	map(0x10000080, 0x10000083).portr("P1").w(FUNC(kinst_multi_state::vram_control_w));
	map(0x10000088, 0x1000008b).portr("P2").w(FUNC(kinst_multi_state::sound_reset_w));
	map(0x10000090, 0x10000093).portr("VOLUME").w(FUNC(kinst_multi_state::sound_control_w));
	map(0x10000098, 0x1000009b).portr("UNUSED").w(FUNC(kinst_multi_state::sound_data_w));
	map(0x100000a0, 0x100000a3).portr("DSW").nopw();
	map(0x100000b0, 0x100000b3).w(FUNC(kinst_multi_state::coin_control_w));

	map(0x10000100, 0x1000013f).rw(FUNC(kinst_multi_state::ide_r), FUNC(kinst_multi_state::ide_w));
	map(0x10000170, 0x10000173).rw(FUNC(kinst_multi_state::ide_extra_r), FUNC(kinst_multi_state::ide_extra_w));
	map(0x1fc00000, 0x1fc7ffff).r(FUNC(kinst_multi_state::rom_r));
}



/*************************************
 *
 *  Port definitions
 *
 *************************************/

static INPUT_PORTS_START( kinstmulti )
	PORT_START("P1")
	PORT_BIT( 0x00000001, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(1) PORT_NAME("P1 High Attack - Quick")
	PORT_BIT( 0x00000002, IP_ACTIVE_LOW, IPT_BUTTON2 ) PORT_PLAYER(1) PORT_NAME("P1 High Attack - Medium")
	PORT_BIT( 0x00000004, IP_ACTIVE_LOW, IPT_BUTTON3 ) PORT_PLAYER(1) PORT_NAME("P1 High Attack - Fierce")
	PORT_BIT( 0x00000008, IP_ACTIVE_LOW, IPT_BUTTON4 ) PORT_PLAYER(1) PORT_NAME("P1 Low Attack - Quick")
	PORT_BIT( 0x00000010, IP_ACTIVE_LOW, IPT_BUTTON5 ) PORT_PLAYER(1) PORT_NAME("P1 Low Attack - Medium")
	PORT_BIT( 0x00000020, IP_ACTIVE_LOW, IPT_BUTTON6 ) PORT_PLAYER(1) PORT_NAME("P1 Low Attack - Fierce")
	PORT_BIT( 0x00000040, IP_ACTIVE_LOW, IPT_JOYSTICK_UP ) PORT_PLAYER(1)
	PORT_BIT( 0x00000080, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN ) PORT_PLAYER(1)
	PORT_BIT( 0x00000100, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT ) PORT_PLAYER(1)
	PORT_BIT( 0x00000200, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(1)
	PORT_BIT( 0x00000400, IP_ACTIVE_LOW, IPT_START1 )
	PORT_BIT( 0x00000800, IP_ACTIVE_LOW, IPT_COIN1 )
	PORT_SERVICE_NO_TOGGLE( 0x00001000, IP_ACTIVE_LOW )
	PORT_BIT( 0x00002000, IP_ACTIVE_LOW, IPT_COIN3 )
	PORT_BIT( 0x00004000, IP_ACTIVE_LOW, IPT_COIN4 )
	PORT_BIT( 0x00008000, IP_ACTIVE_LOW, IPT_CUSTOM ) // door
	PORT_BIT( 0xffff0000, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("P2")
	PORT_BIT( 0x00000001, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(2) PORT_NAME("P2 High Attack - Quick")
	PORT_BIT( 0x00000002, IP_ACTIVE_LOW, IPT_BUTTON2 ) PORT_PLAYER(2) PORT_NAME("P2 High Attack - Medium")
	PORT_BIT( 0x00000004, IP_ACTIVE_LOW, IPT_BUTTON3 ) PORT_PLAYER(2) PORT_NAME("P2 High Attack - Fierce")
	PORT_BIT( 0x00000008, IP_ACTIVE_LOW, IPT_BUTTON4 ) PORT_PLAYER(2) PORT_NAME("P2 Low Attack - Quick")
	PORT_BIT( 0x00000010, IP_ACTIVE_LOW, IPT_BUTTON5 ) PORT_PLAYER(2) PORT_NAME("P2 Low Attack - Medium")
	PORT_BIT( 0x00000020, IP_ACTIVE_LOW, IPT_BUTTON6 ) PORT_PLAYER(2) PORT_NAME("P2 Low Attack - Fierce")
	PORT_BIT( 0x00000040, IP_ACTIVE_LOW, IPT_JOYSTICK_UP ) PORT_PLAYER(2)
	PORT_BIT( 0x00000080, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN ) PORT_PLAYER(2)
	PORT_BIT( 0x00000100, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT ) PORT_PLAYER(2)
	PORT_BIT( 0x00000200, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(2)
	PORT_BIT( 0x00000400, IP_ACTIVE_LOW, IPT_START2 )
	PORT_BIT( 0x00000800, IP_ACTIVE_LOW, IPT_COIN2 )
	PORT_BIT( 0x00001000, IP_ACTIVE_LOW, IPT_TILT )
	PORT_BIT( 0x00002000, IP_ACTIVE_LOW, IPT_SERVICE1 )
	PORT_BIT( 0x00004000, IP_ACTIVE_LOW, IPT_BILL1 ) // bill
	PORT_BIT( 0x00008000, IP_ACTIVE_LOW, IPT_CUSTOM ) // coin door
	PORT_BIT( 0xffff0000, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("VOLUME")
	PORT_BIT( 0x00000001, IP_ACTIVE_LOW, IPT_UNKNOWN )
	PORT_BIT( 0x00000002, IP_ACTIVE_HIGH, IPT_CUSTOM ) PORT_CUSTOM_MEMBER(FUNC(kinst_multi_state::sound_status_r))
	PORT_BIT( 0x00000004, IP_ACTIVE_LOW, IPT_VOLUME_UP )
	PORT_BIT( 0x00000008, IP_ACTIVE_LOW, IPT_VOLUME_DOWN )
	PORT_BIT( 0x0000fff0, IP_ACTIVE_LOW, IPT_UNKNOWN )
	PORT_BIT( 0xffff0000, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("UNUSED")
	PORT_BIT( 0x0000ffff, IP_ACTIVE_LOW, IPT_UNUSED ) // verify
	PORT_BIT( 0xffff0000, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("DSW")
	PORT_DIPNAME( 0x00000003, 0x00000003, "Blood Level" )
	PORT_DIPSETTING(          0x00000003, DEF_STR( High ))
	PORT_DIPSETTING(          0x00000002, DEF_STR( Medium ))
	PORT_DIPSETTING(          0x00000001, DEF_STR( Low ))
	PORT_DIPSETTING(          0x00000000, DEF_STR( None ))
	PORT_DIPNAME( 0x00000004, 0x00000004, DEF_STR( Demo_Sounds ))
	PORT_DIPSETTING(          0x00000000, DEF_STR( Off ))
	PORT_DIPSETTING(          0x00000004, DEF_STR( On ))
	PORT_DIPNAME( 0x00000008, 0x00000008, "Finishing Moves" )
	PORT_DIPSETTING(          0x00000000, DEF_STR( Off ))
	PORT_DIPSETTING(          0x00000008, DEF_STR( On ))
	PORT_DIPNAME( 0x00000010, 0x00000010, "Display Warning" )
	PORT_DIPSETTING(          0x00000000, DEF_STR( Off ))
	PORT_DIPSETTING(          0x00000010, DEF_STR( On ))
	PORT_DIPNAME( 0x00000020, 0x00000020, "Blood" )
	PORT_DIPSETTING(          0x00000020, "Red" )
	PORT_DIPSETTING(          0x00000000, "White" )
	PORT_DIPNAME( 0x00000040, 0x00000040, DEF_STR( Unused ))
	PORT_DIPSETTING(          0x00000040, DEF_STR( Off ))
	PORT_DIPSETTING(          0x00000000, DEF_STR( On ))
	PORT_DIPNAME( 0x00000080, 0x00000080, DEF_STR( Unused ))
	PORT_DIPSETTING(          0x00000080, DEF_STR( Off ))
	PORT_DIPSETTING(          0x00000000, DEF_STR( On ))
	PORT_DIPNAME( 0x00000100, 0x00000100, "Coinage Source" )
	PORT_DIPSETTING(          0x00000100, "Dipswitch" )
	PORT_DIPSETTING(          0x00000000, "Disk" )
	PORT_DIPNAME( 0x00003e00, 0x00003e00, DEF_STR( Coinage ))
	PORT_DIPSETTING(          0x00003e00, "USA-1" )
	PORT_DIPSETTING(          0x00003c00, "USA-2" )
	PORT_DIPSETTING(          0x00003a00, "USA-3" )
	PORT_DIPSETTING(          0x00003800, "USA-4" )
	PORT_DIPSETTING(          0x00003400, "USA-9" )
	PORT_DIPSETTING(          0x00003200, "USA-10" )
	PORT_DIPSETTING(          0x00003600, "USA-ECA" )
	PORT_DIPSETTING(          0x00003000, "USA-Free Play" )
	PORT_DIPSETTING(          0x00002e00, "German-1" )
	PORT_DIPSETTING(          0x00002c00, "German-2" )
	PORT_DIPSETTING(          0x00002a00, "German-3" )
	PORT_DIPSETTING(          0x00002800, "German-4" )
	PORT_DIPSETTING(          0x00002600, "German-ECA" )
	PORT_DIPSETTING(          0x00002000, "German-Free Play" )
	PORT_DIPSETTING(          0x00001e00, "French-1" )
	PORT_DIPSETTING(          0x00001c00, "French-2" )
	PORT_DIPSETTING(          0x00001a00, "French-3" )
	PORT_DIPSETTING(          0x00001800, "French-4" )
	PORT_DIPSETTING(          0x00001600, "French-ECA" )
	PORT_DIPSETTING(          0x00001000, "French-Free Play" )
	PORT_DIPNAME( 0x00004000, 0x00004000, "Coin Counters" )
	PORT_DIPSETTING(          0x00004000, "1" )
	PORT_DIPSETTING(          0x00000000, "2" )
	PORT_DIPNAME( 0x00008000, 0x00008000, "Test Switch" )
	PORT_DIPSETTING(          0x00008000, DEF_STR( Off ))
	PORT_DIPSETTING(          0x00000000, DEF_STR( On ))
	PORT_BIT( 0xffff0000, IP_ACTIVE_LOW, IPT_UNUSED )
INPUT_PORTS_END



/*************************************
 *
 *  Machine driver
 *
 *************************************/

void kinst_multi_state::kinstmulti(machine_config &config)
{
	// basic machine hardware
	R4600LE(config, m_maincpu, 50_MHz_XTAL*2);
	m_maincpu->set_icache_size(16384);
	m_maincpu->set_dcache_size(16384);
	m_maincpu->set_addrmap(AS_PROGRAM, &kinst_multi_state::kinst_map);

	ATA_INTERFACE(config, m_ata).options(kinst_multi_ata_devices, "hdd", nullptr, true);
	m_ata->irq_handler().set_inputline(m_maincpu, 1);
	m_ata->slot(0).set_option_machine_config("hdd", [this] (device_t *device)
			{
				downcast<kinst_multi_hdd_device &>(*device).game_select_cb().set(*this, FUNC(kinst_multi_state::game_select_w));
			});

	// video hardware
	screen_device &screen(SCREEN(config, "screen"));
	screen.set_raw(50_MHz_XTAL/8, 406, 0, 320, 261, 0, 240);
	screen.screen_vblank().set_inputline(m_maincpu, 0);
	screen.set_screen_update(FUNC(kinst_multi_state::screen_update));

	PALETTE(config, m_palette, palette_device::BGR_555);

	// sound hardware
	SPEAKER(config, "mono").front_center();

	DCS_AUDIO_2K(config, m_dcs);
	m_dcs->set_maincpu_tag(m_maincpu);
	m_dcs->add_route(0, "mono", 1.0);
}



/*************************************
 *
 *  ROM definition(s)
 *
 *************************************/

// The source regions are declared before the active ones on purpose: ROM_COPY
// resolves its source by tag against the regions already built, so the source
// has to exist first.
//
// The active regions must hold valid Killer Instinct data by the time devices
// start, not merely by machine_start().  dcs_audio_device::device_start() calls
// dcs_reset() -> dcs_boot(), which boots the ADSP-2105 straight out of the "dcs"
// region, and the driver's own machine_start() runs after every device has
// started.  Leaving these regions empty and filling them later fed 0xff to
// adsp21xx_device::load_boot_data(), which parsed it as a boot descriptor and
// wrote out of bounds.  ROM_COPY runs during ROM loading, before any device
// starts, so the data is in place in time.  apply_game_select() then overwrites
// both regions on a swap.
ROM_START( kinstmulti )
	ROM_REGION32_LE( 0x80000, "boot_ki1", 0 )
	ROM_LOAD( "kinstmulti_ki1.u98", 0x00000, 0x80000, SHA1(6f62a94b209af9f687f250ac9d776e835a0808f6) )

	ROM_REGION32_LE( 0x80000, "boot_ki2", 0 )
	ROM_LOAD( "kinstmulti_ki2.u98", 0x00000, 0x80000, SHA1(e1b81f661e73500a766c0c6f7e971bdfb12ec56f) )

	ROM_REGION32_LE( 0x80000, "user1", ROMREGION_ERASEFF )  // active boot ROM
	ROM_COPY( "boot_ki1", 0x00000, 0x00000, 0x80000 )

	ROM_REGION16_LE( 0x1000000, "dcs_ki1", ROMREGION_ERASEFF )
	ROM_LOAD16_BYTE( "u10-l1", 0x000000, 0x80000, CRC(b6cc155f) SHA1(810d455df8f385d76143e9d7d048f2b555ff8bf0) )
	ROM_LOAD16_BYTE( "u11-l1", 0x200000, 0x80000, CRC(0b5e05df) SHA1(0595909cb667c38ac7c8c7bd0646b28899e27777) )
	ROM_LOAD16_BYTE( "u12-l1", 0x400000, 0x80000, CRC(d05ce6ad) SHA1(7a8ee405c118fd176b66353fa7bfab888cc63cd2) )
	ROM_LOAD16_BYTE( "u13-l1", 0x600000, 0x80000, CRC(7d0954ea) SHA1(ea4d1f153eb284f1bcfc5295fbce316bba6083f4) )
	ROM_LOAD16_BYTE( "u33-l1", 0x800000, 0x80000, CRC(8bbe4f0c) SHA1(b22e365bc8d58a80eaac226be14b4bb8d9a04844) )
	ROM_LOAD16_BYTE( "u34-l1", 0xa00000, 0x80000, CRC(b2e73603) SHA1(ee439f5162a2b3379d3f802328017bb3c68547d2) )
	ROM_LOAD16_BYTE( "u35-l1", 0xc00000, 0x80000, CRC(0aaef4fc) SHA1(48c4c954ac9db648f28ad64f9845e19ec432eec3) )
	ROM_LOAD16_BYTE( "u36-l1", 0xe00000, 0x80000, CRC(0577bb60) SHA1(cc78070cc41701e9a91fde5cfbdc7e1e83354854) )

	ROM_REGION16_LE( 0x1000000, "dcs_ki2", ROMREGION_ERASEFF )
	ROM_LOAD16_BYTE( "ki2_l1.u10", 0x000000, 0x80000, CRC(fdf6ed51) SHA1(acfc9460cd5df01403b7f00b2f68c2a8734ad6d3) )
	ROM_LOAD16_BYTE( "ki2_l1.u11", 0x200000, 0x80000, CRC(f9e70024) SHA1(fe7fc78f1c60b15f2bbdc4c455f55cdf30f48ed4) )
	ROM_LOAD16_BYTE( "ki2_l1.u12", 0x400000, 0x80000, CRC(2994c199) SHA1(9997a83432cb720f65b40a8af46f31a5d0d16d8e) )
	ROM_LOAD16_BYTE( "ki2_l1.u13", 0x600000, 0x80000, CRC(3fe6327b) SHA1(7ff164fc2f079d039921594be92208973d43aa03) )
	ROM_LOAD16_BYTE( "ki2_l1.u33", 0x800000, 0x80000, CRC(6f4dcdcf) SHA1(0ab6dbfb76e9fa2db072e287864ad1f9d514dd9b) )
	ROM_LOAD16_BYTE( "ki2_l1.u34", 0xa00000, 0x80000, CRC(5db48206) SHA1(48456a7b6592c40bc9c664dcd2ee2cfd91942811) )
	ROM_LOAD16_BYTE( "ki2_l1.u35", 0xc00000, 0x80000, CRC(7245ce69) SHA1(24a3ff009c8a7f5a0bfcb198b8dcb5df365770d3) )
	ROM_LOAD16_BYTE( "ki2_l1.u36", 0xe00000, 0x80000, CRC(8920acbb) SHA1(0fca72c40067034939b984b4bf32972a5a6c26af) )

	ROM_REGION16_LE( 0x1000000, "dcs", ROMREGION_ERASEFF )  // active sound data
	ROM_COPY( "dcs_ki1", 0x000000, 0x000000, 0x1000000 )

	DISK_REGION( "ata:0:hdd" )
	DISK_IMAGE( "kinstmulti", 0, SHA1(e1568baa3aca041785493d32782209d1b9408c97) )
ROM_END

} // anonymous namespace



/*************************************
 *
 *  Game driver(s)
 *
 *************************************/

//    YEAR  NAME        PARENT  MACHINE     INPUT       CLASS             INIT        SCREEN  COMPANY  FULLNAME                             FLAGS
GAME( 1996, kinstmulti, 0,      kinstmulti, kinstmulti, kinst_multi_state, empty_init, ROT0,   "hack",  "Killer Instinct 1 & 2 (multi-game)", MACHINE_SUPPORTS_SAVE )


DEFINE_DEVICE_TYPE_PRIVATE(KINST_MULTI_HDD, ide_hdd_device, kinst_multi_hdd_device, "kinst_multi_hdd", "Killer Instinct multi-game HDD")
