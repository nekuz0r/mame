# kinstmulti Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a MAME machine `kinstmulti` that runs both Killer Instinct games from one ROM set and one unified disk image, swapping the active boot ROM and DCS sound ROMs when the software issues IDE command `0xF1`.

**Architecture:** A new self-contained driver `src/mame/rare/kinst_multi.cpp`, copied from `kinst.cpp` and simplified (one address map, no drive-identity fakery, no LBA translation). Six ROM regions: two *active* regions the hardware reads (`user1`, `dcs`) plus two source pairs (`boot_ki1`/`boot_ki2`, `dcs_ki1`/`dcs_ki2`) that get copied into the active ones. A driver-local `ide_hdd_device` subclass intercepts command `0xF1` and calls back into the driver.

**Tech Stack:** MAME (C++17), GENie/make build, MIPS3 (R4600LE) CPU core, DCS audio (ADSP-2105), MAME ATA/IDE HLE stack.

**Reference spec:** `docs/superpowers/specs/2026-08-09-kinst-multi-design.md`

> **Post-implementation correction.** This plan (Task 2 Step 4 and Step 8) has
> the active `user1` and `dcs` regions declared `ERASEFF` with nothing loaded,
> filled by `apply_game_select(false)` from `machine_start()`. That is wrong and
> caused intermittent heap corruption: the driver is the *last* device to start,
> but `dcs_audio_device::device_start()` boots the ADSP-2105 out of the `"dcs"`
> region, so it read `0xff` and `adsp21xx_device::load_boot_data()` wrote out of
> bounds. The shipped code primes both active regions with `ROM_COPY` (processed
> during ROM loading, before any device starts) and declares the source regions
> first. See the spec and commit `355fa28a88e`.

## Global Constraints

- Target file for the new machine: `src/mame/rare/kinst_multi.cpp`. `src/mame/rare/kinst.cpp` must not be modified.
- Machine short name: `kinstmulti`.
- Selector protocol: IDE command `0xF1`; `SECTOR_COUNT` = `1` → Killer Instinct, `2` → Killer Instinct 2, any other value → command aborted with `ERR`/`ABRT`, no swap.
- Game index convention throughout the code: `0` = Killer Instinct, `1` = Killer Instinct 2. Region tags are 1-based (`boot_ki1` = index 0).
- The driver never resets the machine or the main CPU. Only the DCS sound CPU is reset, and only on a live swap.
- MAME source style: tabs for indentation, `// comment` style, `ATTR_COLD` on cold-path overrides, driver classes in an anonymous namespace.
- Boot ROM and disk image hashes are `NO_DUMP` placeholders. Sound ROM hashes are the real stock values and must be copied exactly from `kinst.cpp`.
- Every build in this plan is a full-target incremental build against the existing `build/` tree, producing `./mame`. Do not create a `SUBTARGET` build tree — it would rebuild `src/emu` and `src/devices` from scratch.
- `REGENIE=1` is required only on the build that first introduces the new `.cpp` file (GENie globs `src/mame/rare/**.cpp` and must re-run to see it).
- Substitute your own core count for `-j8`.

## MAME has no unit-test harness for drivers

There is no `pytest` equivalent here. The automated gate for driver work is MAME's built-in validity checker, which is a real and strict test: it verifies device types, address map overlaps, ROM region declarations, slot options, input port definitions, save-state registrations and device tag uniqueness. Each task below uses it as its failing-test-first gate, followed by runtime checks where ROMs are needed.

```
./mame -validate kinstmulti
```

Exits non-zero and prints diagnostics on failure; prints nothing and exits 0 on success.

---

### Task 1: Expose a DRC cache flush on the MIPS3 core

The recompiler only emits block-revalidation code when the code's address has a write pointer (`src/devices/cpu/mips/mips3drc.cpp:359-361`). The boot ROM window is a read handler with no write side, so blocks compiled from it are never revalidated, and the `CACHE` instruction compiles to a no-op (`mips3drc.cpp:2012-2014`). Swapping ROM contents therefore needs an explicit flush. `m_drc_cache_dirty` already exists and is `protected`; this task makes it reachable from a driver.

**Files:**
- Modify: `src/devices/cpu/mips/mips3.h:318-320`

**Interfaces:**
- Consumes: nothing.
- Produces: `void mips3_device::mips3drc_flush_cache()` — public, no arguments, no return. Marks the DRC block cache dirty; `execute_run` flushes on the next tick.

- [ ] **Step 1: Confirm the baseline build and validator are green before touching anything**

```bash
make -j8 && ./mame -validate kinst
```

Expected: build completes, validator prints nothing and exits 0. If the tree does not build cleanly *before* your change, stop and report — do not proceed.

- [ ] **Step 2: Read the existing public DRC API block**

Read `src/devices/cpu/mips/mips3.h` lines 315-322. You should see:

```cpp
	void mips3drc_set_options(uint32_t options);
	uint32_t mips3drc_get_options();
	void mips3drc_add_hotspot(offs_t pc, uint32_t opcode, uint32_t cycles);
```

This is the public, driver-facing DRC API. The new method belongs here.

- [ ] **Step 3: Add the flush method**

In `src/devices/cpu/mips/mips3.h`, immediately after the `mips3drc_add_hotspot` declaration, add:

```cpp
	// invalidate all compiled blocks; required after ROM contents change under
	// the CPU, because blocks compiled from read-only address ranges are never
	// revalidated (see generate_checksum_block in mips3drc.cpp) and the CACHE
	// instruction compiles to a no-op
	void mips3drc_flush_cache() { m_drc_cache_dirty = true; }
```

Place it in the `public:` section that starts at line 303, with the other `mips3drc_*` declarations. `m_drc_cache_dirty` is declared later in the class, at `mips3.h:489` under the `protected:` section beginning at line 322 — that is fine. A member function body defined inside a class is compiled in the complete-class context, so it may reference members declared after it. Do not move the member declaration.

- [ ] **Step 4: Rebuild and confirm no regression**

```bash
make -j8 && ./mame -validate kinst
```

Expected: build succeeds (the header touch rebuilds the MIPS3 core and every driver including it), validator prints nothing and exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/devices/cpu/mips/mips3.h
git commit -m "cpu/mips3: add public mips3drc_flush_cache()

Needed by drivers that swap ROM contents under the CPU. Blocks compiled
from read-only ranges never get a revalidation checksum, and CACHE
compiles to a no-op, so the flush has to be explicit."
```

---

### Task 2: Create the kinstmulti driver, fixed to Killer Instinct

Everything except the IDE interception: the machine, all six ROM regions, and the region-copy plumbing. The game is hardwired to Killer Instinct at startup, so this task is independently reviewable — if KI1 does not boot, the problem is here and not in the swap logic.

**Files:**
- Create: `src/mame/rare/kinst_multi.cpp`
- Modify: `src/mame/mame.lst:39953` (insert a new block after the `kinst.cpp` block)
- Read for reference: `src/mame/rare/kinst.cpp`

**Interfaces:**
- Consumes: `mips3_device::mips3drc_flush_cache()` from Task 1.
- Produces:
  - `kinst_multi_state` — driver class, machine config method `void kinstmulti(machine_config &config)`.
  - `void kinst_multi_state::apply_game_select(bool restart_sound)` — copies source regions into active regions for the current `m_game_select`, flushes the DRC, and pulses the DCS reset only when `restart_sound` is true.
  - `uint8_t kinst_multi_state::m_game_select` — 0 = KI1, 1 = KI2. Saved.
  - `required_device<ata_interface_device> kinst_multi_state::m_ata` — Task 3 rewires its slot options.

- [ ] **Step 1: Write the failing test**

The validator cannot validate a machine that does not exist. Run:

```bash
./mame -validate kinstmulti
```

Expected: FAIL with `Unknown system 'kinstmulti'` (or `unknown system`). Record that you saw it — this is the red state.

- [ ] **Step 2: Create the file header and includes**

Create `src/mame/rare/kinst_multi.cpp` with:

```cpp
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


namespace {
```

`<algorithm>` is for `std::copy_n` in `apply_game_select`.

- [ ] **Step 3: Add the driver state class**

Append:

```cpp
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

	void apply_game_select(bool restart_sound);

	uint32_t *m_video_base = nullptr;

	uint32_t m_vram_control = 0;
	uint32_t m_sound_reset = 0;
	uint32_t m_sound_control = 0;
	uint32_t m_sound_data = 0;
	uint32_t m_coin_control = 0;

	uint8_t m_game_select = 0;
};
```

`required_memory_region_array<2>` with the `"boot_ki%u", 1U` format constructor resolves to tags `boot_ki1` and `boot_ki2` (the pattern used at `src/mame/linn/linndrum.cpp:529`). Note `m_rombase` and `m_bootrom` both point at `user1` — the first is the `uint32_t` pointer `rom_r` reads through, the second is the region object `apply_game_select` writes into. Overwriting region contents leaves `m_rombase` valid.

- [ ] **Step 4: Add machine start, reset, post-load and the region swap**

Append:

```cpp
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

	// load the initial game; the sound CPU has not started yet, so do not touch it
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
	// ROM region contents are not part of a save state, so restore them to match
	// the selection that was saved; the DCS state was already restored, leave it alone
	apply_game_select(false);
}



/*************************************
 *
 *  Game selection
 *
 *************************************/

void kinst_multi_state::apply_game_select(bool restart_sound)
{
	int const game = m_game_select;

	// hold the ADSP-2105 in reset across the copy
	if (restart_sound)
		m_dcs->reset_w(0);

	std::copy_n(m_boot_src[game]->base(), m_bootrom->bytes(), m_bootrom->base());
	std::copy_n(m_dcs_src[game]->base(), m_dcsrom->bytes(), m_dcsrom->base());

	// blocks compiled from the boot ROM window are never revalidated by the DRC
	m_maincpu->mips3drc_flush_cache();

	// re-runs dcs_boot() from the new sound data at the next sync point
	if (restart_sound)
		m_dcs->reset_w(1);
}
```

- [ ] **Step 5: Add video, IDE and control handlers**

These are unchanged from `kinst.cpp` apart from the class name. Append:

```cpp
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
```

- [ ] **Step 6: Add the input ports**

This is the `kinst` port set from `kinst.cpp:571-677`, with the `PORT_CUSTOM_MEMBER` retargeted at `kinst_multi_state`. Append:

```cpp
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
```

- [ ] **Step 7: Add the machine configuration**

For now the ATA slot uses MAME's stock `ata_devices` list. Task 3 replaces those two lines. Append:

```cpp
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

	ATA_INTERFACE(config, m_ata).options(ata_devices, "hdd", nullptr, true);
	m_ata->irq_handler().set_inputline(m_maincpu, 1);

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
```

- [ ] **Step 8: Add the ROM definition and driver entry**

The sound ROM hashes must match `kinst.cpp:766-773` and `kinst.cpp:795-802` exactly. Append:

```cpp
/*************************************
 *
 *  ROM definition(s)
 *
 *************************************/

ROM_START( kinstmulti )
	ROM_REGION32_LE( 0x80000, "user1", ROMREGION_ERASEFF )  // active boot ROM, filled at machine_start

	ROM_REGION32_LE( 0x80000, "boot_ki1", 0 )
	ROM_LOAD( "kinstmulti_ki1.u98", 0x00000, 0x80000, NO_DUMP )

	ROM_REGION32_LE( 0x80000, "boot_ki2", 0 )
	ROM_LOAD( "kinstmulti_ki2.u98", 0x00000, 0x80000, NO_DUMP )

	ROM_REGION16_LE( 0x1000000, "dcs", ROMREGION_ERASEFF )  // active sound data, filled at machine_start

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

	DISK_REGION( "ata:0:hdd" )
	DISK_IMAGE( "kinstmulti", 0, NO_DUMP )
ROM_END

} // anonymous namespace



/*************************************
 *
 *  Game driver(s)
 *
 *************************************/

//    YEAR  NAME        PARENT  MACHINE     INPUT       CLASS             INIT        SCREEN  COMPANY  FULLNAME                             FLAGS
GAME( 1996, kinstmulti, 0,      kinstmulti, kinstmulti, kinst_multi_state, empty_init, ROT0,   "hack",  "Killer Instinct 1 & 2 (multi-game)", MACHINE_SUPPORTS_SAVE )
```

- [ ] **Step 9: Register the machine in mame.lst**

`src/mame/mame.lst` currently has, at line 39949:

```
@source:rare/kinst.cpp
kinst
kinst2
kinst2uk

@source:rare/xtheball.cpp
```

Insert a new block between them so it reads:

```
@source:rare/kinst.cpp
kinst
kinst2
kinst2uk

@source:rare/kinst_multi.cpp
kinstmulti

@source:rare/xtheball.cpp
```

- [ ] **Step 10: Build**

`REGENIE=1` is required here because GENie must re-glob `src/mame/rare/**.cpp` to see the new file.

```bash
make REGENIE=1 -j8
```

Expected: compiles and links. If you get `use of undeclared identifier 'mips3drc_flush_cache'`, Task 1 was not applied.

- [ ] **Step 11: Run the validator to verify it now passes**

```bash
./mame -validate kinstmulti
```

Expected: PASS — no output, exit status 0. This is the same command that failed with `Unknown system` in Step 1.

- [ ] **Step 12: Confirm the machine is registered and its ROM set is well-formed**

```bash
./mame -listxml kinstmulti | grep -E '<machine name|<rom name|<disk name|<region name' | head -30
```

Expected: a `<machine name="kinstmulti">` element, the two `boot_ki*` ROM entries with `status="nodump"`, sixteen sound ROM entries, and one disk entry named `kinstmulti`.

- [ ] **Step 13: Commit**

```bash
git add src/mame/rare/kinst_multi.cpp src/mame/mame.lst
git commit -m "rare/kinst_multi: add kinstmulti machine

Killer Instinct 1 & 2 on one ROM set and one unified disk image. This
commit adds the machine with the game hardwired to Killer Instinct;
the IDE 0xf1 game-select command follows."
```

---

### Task 3: Intercept IDE command 0xF1 and hotswap

Adds the driver-local hard disk subclass that recognises the select-game command, wires its callback to the driver, and makes the swap reachable at runtime.

**Files:**
- Modify: `src/mame/rare/kinst_multi.cpp`

**Interfaces:**
- Consumes: `kinst_multi_state::apply_game_select(bool)`, `kinst_multi_state::m_game_select`, `kinst_multi_state::m_ata` from Task 2.
- Produces:
  - `KINST_MULTI_HDD` — device type, exposed class `ide_hdd_device`.
  - `kinst_multi_hdd_device::game_select_cb()` — returns a bindable `devcb_write8`; the driver binds `kinst_multi_state::game_select_w`.
  - `void kinst_multi_state::game_select_w(uint8_t data)` — `0` selects KI1, `1` selects KI2; a no-op when already selected.

- [ ] **Step 1: Write the failing test**

The abort path is what proves the interception exists at all: on the stock hard disk, command `0xF1` is unknown for *every* sector count, so both selector values abort. Build the current binary if you have not already, then run with your ROM set:

```bash
./mame kinstmulti -debug
```

In the debugger console:

```
pd@0xb0000110=1
pd@0xb0000138=0xf1
pd@0xb0000138
```

Expected: FAIL — the status read returns a value with bit 0 (`ERR`) set, because `ata_hle_device_base::process_command()` aborted the unknown command. Record this. After Task 3 the same sequence must leave `ERR` clear.

`0xb0000110` and `0xb0000138` are the KSEG1 (uncached) views of physical `0x10000110` and `0x10000138`. The IDE registers are at `0x10000100 + reg * 8` because `ide_r`/`ide_w` divide the 32-bit map offset by two: `SECTOR_COUNT` is register 2, `COMMAND`/`STATUS` is register 7, `ERROR` is register 1 at `0xb0000108`. `pd@` is a 32-bit program-space access.

If you do not have the ROM set yet, skip to Step 2 and use the `-validate` gate in Step 8 as the automated check; run this test when the ROMs are available.

- [ ] **Step 2: Declare the device type at global scope**

`DEFINE_DEVICE_TYPE_PRIVATE` must be used in the global namespace, so the type needs a forward declaration before the anonymous namespace opens. This is the layout used by `src/mame/dynax/ddenlovr.cpp:177,13591`.

In `src/mame/rare/kinst_multi.cpp`, between the `#include` block and `namespace {`, insert:

```cpp

DECLARE_DEVICE_TYPE(KINST_MULTI_HDD, ide_hdd_device)

```

- [ ] **Step 3: Add the hard disk subclass**

Immediately after `namespace {`, before `class kinst_multi_state`, insert:

```cpp
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


void kinst_multi_ata_devices(device_slot_interface &device)
{
	device.option_add("hdd", KINST_MULTI_HDD);
}
```

`m_command`, `m_sector_count`, `m_status`, `IDE_STATUS_DRDY`, `start_busy`, `set_irq`, `MINIMUM_COMMAND_TIME` and `PARAM_COMMAND` are all `protected` in `ata_hle_device_base` (`src/devices/machine/atahle.h`), so no accessors are needed. The command-write path clears `ERR` and `m_error` before calling `process_command()` (`atahle.cpp:807-808`), so the success path does not clear them itself. `devcb` objects are auto-resolved by the framework; do not call `.resolve()`.

The `set_irq(ASSERT_LINE)` call in `finished_command()` must be qualified as `ata_hle_device_base::set_irq(ASSERT_LINE)`. `ide_hdd_device` inherits `set_irq(int)` ambiguously from both `ata_hle_device_base` and `device_ata_interface`, so an unqualified call does not compile. The `ata_hle_device_base` overload is also the behaviourally correct one here: it preserves device-selection and nIEN gating via `update_irq()`.

- [ ] **Step 4: Declare `game_select_w` on the driver**

In `class kinst_multi_state`, in the `private:` section, change:

```cpp
	void apply_game_select(bool restart_sound);
```

to:

```cpp
	void game_select_w(uint8_t data);
	void apply_game_select(bool restart_sound);
```

- [ ] **Step 5: Implement `game_select_w`**

In the "Game selection" section, immediately before `void kinst_multi_state::apply_game_select(bool restart_sound)`, insert:

```cpp
void kinst_multi_state::game_select_w(uint8_t data)
{
	if (data == m_game_select)
		return;

	m_game_select = data;
	apply_game_select(true);
}


```

- [ ] **Step 6: Wire the custom hard disk into the machine configuration**

In `kinst_multi_state::kinstmulti`, replace:

```cpp
	ATA_INTERFACE(config, m_ata).options(ata_devices, "hdd", nullptr, true);
	m_ata->irq_handler().set_inputline(m_maincpu, 1);
```

with:

```cpp
	ATA_INTERFACE(config, m_ata).options(kinst_multi_ata_devices, "hdd", nullptr, true);
	m_ata->irq_handler().set_inputline(m_maincpu, 1);
	m_ata->slot(0).set_option_machine_config("hdd", [this] (device_t *device)
			{
				downcast<kinst_multi_hdd_device &>(*device).game_select_cb().set(*this, FUNC(kinst_multi_state::game_select_w));
			});
```

`abstract_ata_interface_device::slot(int)` is public (`src/devices/bus/ata/ataintf.h:54`) and returns an `ata_slot_device`, which is a `device_slot_interface` (`src/devices/bus/ata/atadev.h:19-22`), so `set_option_machine_config` (`src/emu/dislot.h:180`) applies. The slot option is still named `"hdd"`, so the `DISK_REGION( "ata:0:hdd" )` tag is unchanged. This mirrors `src/mame/pc/poisk1.cpp:698`.

- [ ] **Step 7: Define the device type at global scope**

At the very end of the file, after the `GAME(...)` line, append:

```cpp


DEFINE_DEVICE_TYPE_PRIVATE(KINST_MULTI_HDD, ide_hdd_device, kinst_multi_hdd_device, "kinst_multi_hdd", "Killer Instinct multi-game HDD")
```

- [ ] **Step 8: Build and validate**

```bash
make -j8 && ./mame -validate kinstmulti
```

Expected: builds and the validator prints nothing, exit 0. The validator checks that `KINST_MULTI_HDD`'s short name is unique and well-formed and that the slot option resolves.

- [ ] **Step 9: Verify the select-KI2 path**

```bash
./mame kinstmulti -debug
```

In the debugger console:

```
pd@0xb0000110=2
pd@0xb0000138=0xf1
pd@0xb0000138
```

Expected: PASS — the status read has bit 0 (`ERR`) clear and bit 6 (`DRDY`) set. This is the same sequence that failed in Step 1.

Confirm the ROM actually swapped by comparing the first words of the active region against the KI2 source. In the debugger:

```
pd@0xbfc00000
```

Compare against the first dword of your `kinstmulti_ki2.u98` file. It must differ from the value the same command returned before the swap.

- [ ] **Step 10: Verify the select-KI1 path**

Still in the debugger:

```
pd@0xb0000110=1
pd@0xb0000138=0xf1
pd@0xb0000138
pd@0xbfc00000
```

Expected: `ERR` clear, and `pd@0xbfc00000` back to the value it had at startup.

- [ ] **Step 11: Verify the abort path**

```
pd@0xb0000110=3
pd@0xb0000138=0xf1
pd@0xb0000138
pd@0xb0000108
pd@0xbfc00000
```

Expected: the status read has bit 0 (`ERR`) set, the error register read at `0xb0000108` has bit 2 (`ABRT`) set, and `pd@0xbfc00000` is unchanged — no swap occurred. Repeat with `0` written to `SECTOR_COUNT` and confirm it also aborts (MAME stores a written `0` as `0x100`, which is not a valid selector).

- [ ] **Step 12: Verify save state round-trip**

Run the machine, switch to KI2 using the Step 9 sequence, save a state, quit, restart, and load the state:

```bash
./mame kinstmulti -debug
```

Save with `F7` then a slot key, quit, relaunch, load with `F8`(or `-state` on the command line) and check in the debugger that `pd@0xbfc00000` matches KI2's first dword. This proves `device_post_load()` restored the region contents, which are not part of the save state.

- [ ] **Step 13: Commit**

```bash
git add src/mame/rare/kinst_multi.cpp
git commit -m "rare/kinst_multi: add IDE 0xf1 game select

Driver-local ide_hdd_device subclass recognises vendor command 0xf1 and
swaps the active boot ROM and DCS sound data based on SECTOR_COUNT
(1 = KI, 2 = KI2). Any other value falls through to the base class, which
aborts the command."
```

---

## Filling in the real hashes

The two boot ROMs and the disk image are declared `NO_DUMP`. MAME still loads a file matching the declared name and only warns that it is unverified. Once you have final files:

```bash
./mame -verifyroms kinstmulti
```

Then replace `NO_DUMP` with `CRC(xxxxxxxx) SHA1(xxxx...)` for the two boot ROMs and `SHA1(xxxx...)` for the disk image. Re-run `./mame -verifyroms kinstmulti` and expect `romset kinstmulti is good`.

## Notes for the implementer

- **Do not modify `src/mame/rare/kinst.cpp`.** The whole point of the separate file is that upstream MAME changes to `kinst.cpp` never conflict.
- **`src/mame/rare/sound_rom_swap.md`** is the user's untracked scratch notes. Leave it alone; do not commit it.
- The spec's verification section used `pw@` for the debugger memory writes. That is a 16-bit access; these registers need 32-bit writes, so this plan uses `pd@` throughout.
- If the DCS goes silent after a swap, check the ordering in `apply_game_select`: `reset_w(0)` must come *before* the `std::copy_n` calls and `reset_w(1)` after, because `dcs_boot()` is scheduled via `machine().scheduler().synchronize()` (`src/mame/shared/dcs.cpp:1462-1465`) and reads the region at the next sync point.
- If the wrong game runs after a swap despite the ROM contents being correct, the DRC flush is not happening — verify `mips3drc_flush_cache()` is being called and that Task 1's edit landed in the `public:` section.
