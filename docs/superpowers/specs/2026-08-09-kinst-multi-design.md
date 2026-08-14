# kinstmulti — Killer Instinct 1 & 2 multi-game machine

Design spec, 2026-08-09.

## Goal

Add a MAME machine, `kinstmulti`, that runs both Killer Instinct and Killer
Instinct 2 from a single ROM set and a single unified hard disk image. The
running game is chosen at runtime: the software issues IDE command `0xF1`, and
the driver swaps the active boot ROM and DCS sound ROMs to match.

The new machine lives in `src/mame/rare/kinst_multi.cpp`, alongside
`src/mame/rare/kinst.cpp`. `kinst.cpp` is not modified.

## Premises

These follow from the modified ROMs and disk image this machine is built for.
They are what makes the driver much simpler than a naive KI1+KI2 merge:

1. **The disk image needs no translation.** The boot ROMs are patched to apply
   their own LBA offset into the unified image, so the driver passes IDE
   accesses straight through. There is one `DISK_IMAGE`.
2. **One address map.** The KI2 boot ROM is patched to use KI1's control
   register layout, so only `kinst_map` is needed. `kinst2_map`
   (`kinst.cpp:541-553`) has no equivalent here.
3. **No drive identity check.** Both boot ROMs are patched to accept any drive,
   so the IDENTIFY-buffer fakery in `kinst_state::machine_reset()`
   (`kinst.cpp:334-347`) is dropped entirely, along with `m_hdd_serial_offset`
   and the `init_kinst` / `init_kinst2` handlers.
4. **The ROM handles the post-swap jump.** The driver does not reset the
   machine or the main CPU. It swaps the ROM contents, completes the IDE
   command, and returns; the ROM jumps to the new game itself. This is a
   ROM-side obligation the driver cannot enforce: the code that issues 0xf1
   and polls for completion must be byte-identical at the same address in
   both boot ROMs, or must execute from RAM. The swap replaces the
   instructions under the PC, and execution resumes at the same address in
   the new ROM, so anything else jumps into arbitrary code.

## Swap protocol

| Register | Value | Meaning |
|---|---|---|
| `COMMAND` (cs0 reg 7) | `0xF1` | Select game |
| `SECTOR_COUNT` (cs0 reg 2) | `1` | Killer Instinct |
| `SECTOR_COUNT` (cs0 reg 2) | `2` | Killer Instinct 2 |
| `SECTOR_COUNT` (cs0 reg 2) | anything else | Command aborted, no swap |

`0xF1` is unused by MAME's ATA implementation, so it does not collide with any
emulated command.

Selector values `1` and `2` are used rather than `0` and `1` deliberately.
`ata_hle_device_base::write_cs0` stores a written `SECTOR_COUNT` of zero as
`0x100`, per the ATA convention that zero means 256 sectors
(`src/devices/machine/atahle.cpp:770`):

```cpp
m_sector_count = (data & 0xff) ? (data & 0xff) : 0x100;
```

Starting the selector at `1` keeps the register value and the selector value
identical and avoids that trap.

## Architecture

Three classes, plus one two-line change to the MIPS3 CPU core.

### `kinst_multi_hdd_device : ide_hdd_device`

Intercepts the select-game command. Registered with
`DEFINE_DEVICE_TYPE_PRIVATE` so it stays local to the driver file.

```cpp
class kinst_multi_hdd_device : public ide_hdd_device
{
public:
    kinst_multi_hdd_device(const machine_config &mconfig, const char *tag,
                           device_t *owner, uint32_t clock = 0);

    auto game_select_cb() { return m_game_select_cb.bind(); }

protected:
    virtual void device_start() override ATTR_COLD;
    virtual void process_command() override;
    virtual void finished_command() override;

private:
    static constexpr uint8_t IDE_COMMAND_SELECT_GAME = 0xf1;

    devcb_write8 m_game_select_cb;
    uint8_t m_pending_game = 0;
};
```

```cpp
void kinst_multi_hdd_device::process_command()
{
    if (m_command == IDE_COMMAND_SELECT_GAME)
    {
        if (m_sector_count == 1 || m_sector_count == 2)
        {
            m_pending_game = m_sector_count - 1;
            start_busy(MINIMUM_COMMAND_TIME, PARAM_COMMAND);
            return;
        }
        // any other value falls through; the base class aborts unknown commands
    }

    ide_hdd_device::process_command();
}

void kinst_multi_hdd_device::finished_command()
{
    if (m_command == IDE_COMMAND_SELECT_GAME)
    {
        m_game_select_cb(m_pending_game);
        m_status |= IDE_STATUS_DRDY;
        set_irq(ASSERT_LINE);
        return;
    }

    ide_hdd_device::finished_command();
}
```

Notes:

- `start_busy`, `set_irq`, `MINIMUM_COMMAND_TIME` and `PARAM_COMMAND` are all
  `protected` in `ata_hle_device_base`, so the subclass reaches them directly.
- The abort path is free: `ide_hdd_device_base::process_command()`'s `default:`
  falls through to `ata_hle_device_base::process_command()`
  (`src/devices/machine/atastorage.cpp:868`), which sets `IDE_STATUS_ERR`,
  `IDE_ERROR_ABRT` and asserts IRQ.
- The command-write path already clears `ERR` and `m_error` before calling
  `process_command()` (`src/devices/machine/atahle.cpp:807-808`), so the success
  path needs no explicit clearing.
- Doing the swap in `finished_command()` rather than `process_command()` gives a
  realistic BSY → DRDY + IRQ handshake, so the ROM works whether it polls status
  or waits on the interrupt.
- `m_pending_game` is registered for save states in `device_start()`.

### `kinst_multi_state : driver_device`

A copy of `kinst_state` (`kinst.cpp:200-262`) with the premises above applied.
Retained verbatim: `screen_update`, `ide_r` / `ide_w`, `ide_extra_r` /
`ide_extra_w`, `rom_r`, `vram_control_w`, `sound_reset_w`, `sound_control_w`,
`sound_data_w`, `coin_control_w`, `sound_status_r`, and `kinst_map`.

Removed: `m_hdd_serial_offset`, the IDENTIFY patching in `machine_reset()`,
`kinst2_map`, `kinst2uk_state` and its CPLD simulation, `init_kinst`,
`init_kinst2`.

Added:

```cpp
required_memory_region m_bootrom;            // "user1"  - active
required_memory_region m_dcsrom;             // "dcs"    - active
required_memory_region_array<2> m_boot_src;  // "boot_ki1", "boot_ki2"
required_memory_region_array<2> m_dcs_src;   // "dcs_ki1", "dcs_ki2"

uint8_t m_game_select = 0;

void game_select_w(uint8_t data);
void apply_game_select(bool restart_sound);
virtual void device_post_load() override;
```

### The swap

```cpp
void kinst_multi_state::game_select_w(uint8_t data)
{
    if (data == m_game_select)
        return;

    m_game_select = data;
    apply_game_select(true);
}

void kinst_multi_state::apply_game_select(bool restart_sound)
{
    const int g = m_game_select;

    // hold the ADSP-2105 in reset across the copy
    if (restart_sound)
        m_dcs->reset_w(0);

    std::copy_n(m_boot_src[g]->base(), m_bootrom->bytes(), m_bootrom->base());
    std::copy_n(m_dcs_src[g]->base(),  m_dcsrom->bytes(),  m_dcsrom->base());

    m_maincpu->mips3drc_flush_cache();

    if (restart_sound)
        m_dcs->reset_w(1);   // re-runs dcs_boot() from the new sound data
}
```

`restart_sound` is only true on a live swap. `machine_start()` must not touch
the DCS because device start order is not guaranteed, and `device_post_load()`
must not either — the save state has already restored the DCS's own state, and
resetting it there would discard it.

Why in-place copies work, and why the ordering matters:

- The DCS device binds `m_bootrom` to the region tagged `"dcs"` via
  `DEVICE_SELF` (`src/mame/shared/dcs.cpp:693`) and configures all its bank
  entries as pointers into that buffer (`dcs.cpp:770-780`). The buffer's address
  must stay fixed; only its contents change. No bank reconfiguration is needed.
- `reset_w(0)` asserts the ADSP reset line immediately and *schedules*
  `dcs_reset` through `machine().scheduler().synchronize()`
  (`dcs.cpp:1462-1465`). `dcs_boot()` therefore runs at the next sync point,
  after the copy has landed.
- `rom_r` reads through `m_rombase`, a `required_region_ptr` into `"user1"`.
  Overwriting the region's contents leaves that pointer valid.

**The active regions cannot be filled from `machine_start()`.** The driver is the
last device to start — every other device, including the DCS, has already run its
`device_start()` by then. `dcs_audio_device::device_start()` calls `dcs_reset()`
→ `dcs_boot()`, which boots the ADSP-2105 directly out of the `"dcs"` region. An
empty (`0xff`) region at that moment is fed to
`adsp21xx_device::load_boot_data()`, which reads it as a boot descriptor, derives
a garbage page count, and writes out of bounds — corrupting the heap and
crashing somewhere unrelated on most runs.

The active regions are therefore primed by `ROM_COPY`, which the ROM loader
processes before any device starts (`romload.cpp:1035-1037`). The source regions
are declared first in `ROM_START` because `ROM_COPY` resolves its source tag
against the regions already built.

`machine_start()` still calls `apply_game_select(false)`, which is a no-op on a
cold boot but keeps the regions and `m_game_select` consistent unconditionally,
and registers `m_game_select` for save states. ROM region contents are not part
of a save state, so `device_post_load()` re-runs `apply_game_select(false)`;
without it, loading a KI2 state into a fresh session would leave KI1's ROMs in
place.

### MIPS3 DRC cache invalidation

This is the one change outside the driver, and it is required for correctness.

`mips3drc.cpp:359-361` only emits a block-validation checksum when the code's
address has a write pointer:

```cpp
/* validate this code block if we're not pointing into ROM */
if (m_program->get_write_ptr(seqhead->physpc) != nullptr)
    generate_checksum_block(block, compiler, seqhead, seqlast, codelast);
```

The boot ROM window at `0x1fc00000` is a read handler with no write side, so
blocks compiled from it are never revalidated. After a swap the CPU would
continue executing the previous game's compiled code out of the new ROM.

The `CACHE` instruction does not help: the recompiler compiles opcode `0x2f` as
an effective no-op alongside `PREF` (`mips3drc.cpp:2012-2014`), and the
interpreter's `handle_cache()` only validates the encoding. Neither touches the
DRC block cache.

`m_drc_cache_dirty` is already `protected` in `mips3_device`, and setting it
makes `execute_run` flush on the next tick (`mips3.cpp:5355-5357`). Expose it
next to the existing public `mips3drc_*` driver-facing API in
`src/devices/cpu/mips/mips3.h`:

```cpp
void mips3drc_flush_cache() { m_drc_cache_dirty = true; }
```

Rejected alternatives:

- **RAM-backing the boot ROM window** so the DRC self-validates. Needs no core
  change but loses `rom_r`'s waitstate emulation, and loose verification only
  checks each block's first opcode, so it would also need
  `MIPS3DRC_STRICT_VERIFY` and its performance cost.
- **Resetting the main CPU on swap.** `device_reset()` already sets
  `m_drc_cache_dirty` (`mips3.cpp:1149`), but it contradicts premise 4: the
  reset would fire from inside the IDE write and the ROM would never finish its
  post-command sequence.

## ROM regions

| Tag | Size | Contents |
|---|---|---|
| `user1` | `0x80000` | Active boot ROM. `ERASEFF`, primed from `boot_ki1` by `ROM_COPY`. |
| `boot_ki1` | `0x80000` | Patched KI1 boot ROM. |
| `boot_ki2` | `0x80000` | Patched KI2 boot ROM. |
| `dcs` | `0x1000000` | Active sound data. `ERASEFF`, primed from `dcs_ki1` by `ROM_COPY`. |
| `dcs_ki1` | `0x1000000` | Stock KI1 sound set, from `kinst.cpp:766-773`. |
| `dcs_ki2` | `0x1000000` | Stock KI2 sound set, from `kinst.cpp:795-802`. |
| `ata:0:hdd` | — | The unified disk image. |

The separate active regions cost 32 MB of extra region memory. That is the price
of the DCS device caching pointers into a fixed buffer; stashing originals on the
heap instead would save nothing meaningful and read less clearly.

```cpp
// Sources are declared before the active regions: ROM_COPY resolves its source
// tag against the regions already built.
ROM_START( kinstmulti )
    ROM_REGION32_LE( 0x80000, "boot_ki1", 0 )
    ROM_LOAD( "kinstmulti_ki1.u98", 0x00000, 0x80000, NO_DUMP )

    ROM_REGION32_LE( 0x80000, "boot_ki2", 0 )
    ROM_LOAD( "kinstmulti_ki2.u98", 0x00000, 0x80000, NO_DUMP )

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
    DISK_IMAGE( "kinstmulti", 0, NO_DUMP )
ROM_END
```

The two boot ROMs and the disk image use `NO_DUMP` as a placeholder rather than
a fabricated `CRC`/`SHA1`. MAME still loads a file of the given name when it is
present and only warns that it is unverified, whereas an incorrect explicit hash
on a CHD makes the disk fail to open. Replace with real hashes once known.

## Machine configuration

Identical to `kinst_state::kinst()` (`kinst.cpp:698-723`) — R4600LE at
`50_MHz_XTAL*2`, 16 KB I/D caches, `kinst_map`, the `50_MHz_XTAL/8` screen,
`BGR_555` palette, mono speaker, `DCS_AUDIO_2K` — except for the ATA slot, which
uses the custom device and binds the swap callback:

```cpp
static void kinst_multi_ata_devices(device_slot_interface &device)
{
    device.option_add("hdd", KINST_MULTI_HDD);
}
```

```cpp
ATA_INTERFACE(config, m_ata).options(kinst_multi_ata_devices, "hdd", nullptr, true);
m_ata->irq_handler().set_inputline(m_maincpu, 1);
m_ata->slot(0).set_option_machine_config("hdd", [this](device_t *device) {
    downcast<kinst_multi_hdd_device &>(*device).game_select_cb().set(
        *this, FUNC(kinst_multi_state::game_select_w));
});
```

`abstract_ata_interface_device::slot(int)` is public
(`src/devices/bus/ata/ataintf.h:54`) and returns an `ata_slot_device`, which is a
`device_slot_interface`, so `set_option_machine_config` is available
(`src/emu/dislot.h:180`). The pattern matches `src/mame/pc/poisk1.cpp:698`.

The `DISK_REGION` tag stays `"ata:0:hdd"` because the slot option is still named
`"hdd"`.

### Device type declaration placement

`DEFINE_DEVICE_TYPE_PRIVATE` must appear in the global namespace, so the file
follows the layout used by `src/mame/dynax/ddenlovr.cpp:177,13591`:

```cpp
DECLARE_DEVICE_TYPE(KINST_MULTI_HDD, ide_hdd_device)   // global scope, before the driver

namespace {
    // ... device implementation, driver state, ROM definitions, GAME() ...
} // anonymous namespace

DEFINE_DEVICE_TYPE_PRIVATE(KINST_MULTI_HDD, ide_hdd_device, kinst_multi_hdd_device,
                           "kinst_multi_hdd", "Killer Instinct multi-game HDD")
```

## Driver entry

```cpp
GAME( 1996, kinstmulti, 0, kinstmulti, kinstmulti, kinst_multi_state, empty_init, ROT0,
      "hack", "Killer Instinct 1 & 2 (multi-game)", MACHINE_SUPPORTS_SAVE )
```

Parent is `0` rather than `kinst`: a `kinst` parent would only resolve the KI1
sound ROMs, so everything belongs in `kinstmulti.zip` regardless. INPUT is its
own `kinstmulti` set, not `kinst`'s: it only differs from `kinst`'s in the
label on DSW bit `0x20`, but it is a distinct `INPUT_PORTS_START` block.

`src/mame/mame.lst` gains a new block after the `kinst.cpp` block:

```
@source:rare/kinst_multi.cpp
kinstmulti
```

## Error handling

| Case | Behaviour |
|---|---|
| `0xF1`, `SECTOR_COUNT` = 1 or 2 | BSY → swap → `DRDY`, IRQ asserted |
| `0xF1`, any other `SECTOR_COUNT` | Falls through to the base, `ERR` + `ABRT` + IRQ, no swap |
| `0xF1` selecting the already-active game | `game_select_w` returns early; no copy, no DCS reset, command still completes successfully |
| All other IDE commands | Untouched, straight through to `ide_hdd_device` |

## Verification

1. `make -j8`, a plain full-target incremental build producing `./mame`. Do
   not use `SUBTARGET=`/`SOURCES=`: that builds a separate tree and
   recompiles all of `src/emu` and `src/devices` from scratch instead of
   reusing the existing `build/` tree, taking hours instead of minutes.
   `REGENIE=1` is only needed on the build that first introduces a new
   `.cpp` file. The `mips3.h` change forces a CPU core rebuild either way.
2. `./mame -validate kinstmulti` — catches address-map overlaps, malformed ROM
   regions, bad device types and bad slot options.
3. Boot with the real ROM set and confirm KI1 runs.
4. Exercise the swap from the MAME debugger without needing the loader ROM. IDE
   registers sit at `0x10000100 + reg * 8`, so `SECTOR_COUNT` is at
   `0x10000110` and `COMMAND` at `0x10000138`:
   - `pd@0xb0000110 = 2` then `pd@0xb0000138 = 0xf1` selects KI2. Confirm
     `user1` now matches `boot_ki2` and the DCS restarted.
   - Repeat with `1` for KI1.
   - Repeat with `3` and confirm the status register at `0xb0000138` reports
     `ERR`, the error register at `0xb0000108` reports `ABRT`, and no swap
     occurred.

   `pd@` is a 32-bit program-space access; these registers are mapped as
   32-bit units, so a 16-bit `pw@` would not reach them correctly.
5. Save a state after switching to KI2, restart MAME, load the state, and
   confirm `device_post_load()` restored KI2's ROM contents.

## Out of scope

- Upstreaming to MAME. This is a machine for modified ROMs and a custom disk
  image; the `mips3.h` addition is a local delta.
- Any change to `kinst`, `kinst2` or `kinst2uk`.
- LBA translation, drive-model emulation, or the KI2 upgrade-kit CPLD.
