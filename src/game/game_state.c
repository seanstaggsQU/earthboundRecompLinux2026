#include "game/game_state.h"
#include "game/battle.h"
#include "game/inventory.h"
#include "game/settings.h"
#include "core/memory.h"
#include "data/assets.h"
#include "entity/entity.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

GameState game_state;
CharStruct party_characters[TOTAL_PARTY_COUNT];
uint8_t event_flags[EVENT_FLAG_COUNT / 8];
uint8_t key_items_pool[KEY_ITEMS_POOL_SIZE];
uint8_t fastest_hppp_meter_speed;
uint8_t current_save_slot;

void game_state_init(void) {
    memset(&game_state, 0, sizeof(game_state));
    memset(party_characters, 0, sizeof(party_characters));
    memset(event_flags, 0, sizeof(event_flags));
    memset(key_items_pool, 0, sizeof(key_items_pool));

    /* Defaults */
    game_state.text_speed = 2; /* medium */
    game_state.sound_setting = 0; /* stereo */
}

bool event_flag_get(uint16_t flag) {
    /* Assembly GET_EVENT_FLAG (get_event_flag.asm:10) does DEC first:
     * flag IDs are 1-based (0 = EVENT_FLAG::NONE sentinel). */
    if (flag == 0 || flag > EVENT_FLAG_COUNT) return false;
    flag--;
    return (event_flags[flag / 8] >> (flag % 8)) & 1;
}

void event_flag_set(uint16_t flag) {
    /* Assembly SET_EVENT_FLAG (set_event_flag.asm:15) does DEC first:
     * flag IDs are 1-based (0 = EVENT_FLAG::NONE sentinel). */
    if (flag == 0 || flag > EVENT_FLAG_COUNT) return;
    flag--;
    event_flags[flag / 8] |= (1 << (flag % 8));
}

void event_flag_clear(uint16_t flag) {
    /* Assembly SET_EVENT_FLAG (set_event_flag.asm:15) does DEC first:
     * flag IDs are 1-based (0 = EVENT_FLAG::NONE sentinel). */
    if (flag == 0 || flag > EVENT_FLAG_COUNT) return;
    flag--;
    event_flags[flag / 8] &= ~(1 << (flag % 8));
}

/* Port of CLEAR_ALL_STATUS_EFFECTS (asm/misc/clear_all_status_effects.asm).
 * Clears all entity surface flags and party member afflictions. */
void clear_all_status_effects(void) {
    /* Assembly lines 8-20: clear ENTITY_SURFACE_FLAGS for all entities */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities.surface_flags[ENT(i)] = 0;
    }

    /* Assembly lines 21-54: clear afflictions for all 6 party members */
    for (int member = 0; member < TOTAL_PARTY_COUNT; member++) {
        for (int group = 0; group < AFFLICTION_GROUP_COUNT; group++) {
            party_characters[member].afflictions[group] = 0;
        }
    }

    /* Assembly lines 55-57: clear party_status */
    game_state.party_status = 0;
}

/* CHECK_CHARACTER_IN_PARTY: Port of asm/battle/check_character_in_party.asm.
 * Searches party_members[0..party_count-1] for char_id.
 * Returns char_id if found, 0 otherwise. */
uint16_t check_character_in_party(uint16_t char_id) {
    uint8_t count = game_state.party_count;
    for (int i = 0; i < count; i++) {
        if (game_state.party_members[i] == char_id)
            return char_id;
    }
    return 0;
}

/* CHECK_STATUS_GROUP: Port of asm/misc/check_status_group.asm.
 * A=status_group (1-7=affliction index, 8=party_status).
 * X=char_id (1-indexed).
 * Returns affliction_value+1 (so 0 means "not in party", 1 means "affliction==0").
 *
 * NOTE: The original assembly (asm/misc/check_status_group.asm) has a bug:
 * it passes status_group (not char_id) to CHECK_CHARACTER_IN_PARTY, and computes
 * the affliction address as (status_group-1)*95 + (char_id-1) instead of the
 * correct (char_id-1)*95 + (status_group-1). The C port uses the correct formula,
 * which means it diverges from the original game's behavior. This is an intentional
 * fix of an original game bug. */
uint16_t check_status_group(uint16_t status_group, uint16_t char_id) {
    /* Assembly line 15: CPY #8 → if char_id==8, return party_status+1 */
    if (char_id == 8) {
        return (uint16_t)((game_state.party_status & 0xFF) + 1);
    }
    /* Assembly line 22-25: check if character is in party */
    if (check_character_in_party(char_id) == 0)
        return 0;
    /* Assembly lines 26-43: compute afflictions[group-1]+1 */
    if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && status_group >= 1 && status_group <= 7) {
        uint8_t affliction = party_characters[char_id - 1].afflictions[status_group - 1];
        return (uint16_t)(affliction + 1);
    }
    return 0;
}

/* INFLICT_STATUS_NONBATTLE: Port of asm/misc/inflict_status_nonbattle.asm.
 * A=char_id, X=status_group, Y=value.
 * If group==8, sets party_status = value-1 and returns char_id.
 * Otherwise sets afflictions[group-1] = value-1 for char_id.
 * Returns char_id on success, 0 if character not in party. */
uint16_t inflict_status_nonbattle(uint16_t char_id, uint16_t status_group, uint16_t value) {
    /* Assembly lines 22-30: party status special case */
    if (status_group == 8) {
        game_state.party_status = (uint8_t)(value - 1);
        return char_id;
    }
    /* Assembly lines 32-35: check character is in party */
    if (check_character_in_party(char_id) == 0)
        return 0;
    /* Assembly lines 36-54: set affliction value */
    if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT && status_group >= 1 && status_group <= 7) {
        party_characters[char_id - 1].afflictions[status_group - 1] = (uint8_t)(value - 1);
    }
    render_and_disable_entities();
    return char_id;
}

/* LEARN_SPECIAL_PSI: Port of asm/misc/learn_special_psi.asm.
 * Sets the corresponding bit in game_state.party_psi.
 * The assembly maps: 1→bit0(TELEPORT_ALPHA), 2→bit1(STARSTORM_ALPHA),
 * 3→bit2(STARSTORM_OMEGA), 4→bit3(TELEPORT_BETA). */
void learn_special_psi(uint16_t psi_type) {
    if (psi_type >= 1 && psi_type <= 4) {
        game_state.party_psi |= (1 << (psi_type - 1));
    }
}

static const PsiAbility *psi_ability_table_data = NULL;
static size_t psi_ability_table_size = 0;

static bool ensure_psi_ability_table(void) {
    if (psi_ability_table_data) return true;
    psi_ability_table_size = ASSET_SIZE(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    psi_ability_table_data = (const PsiAbility *)ASSET_DATA(ASSET_DATA_PSI_ABILITY_TABLE_BIN);
    return psi_ability_table_data != NULL;
}

/* CHECK_IF_PSI_KNOWN: Port of asm/misc/check_if_psi_known.asm.
 * A=char_id (PARTY_MEMBER enum), X=psi_ability_id.
 * Returns 1 if character's level >= PSI learning level, 0 otherwise. */
uint16_t check_if_psi_known(uint16_t char_id, uint16_t psi_ability_id) {
    if (!ensure_psi_ability_table()) return 0;
    if (psi_ability_id >= PSI_MAX_ENTRIES) return 0;

    const PsiAbility *psi = &psi_ability_table_data[psi_ability_id];

    /* Assembly dispatch: NESS=1→ness_level, PAULA=2→paula_level, POO=4→poo_level */
    uint8_t learn_level;
    switch (char_id) {
    case 1:  learn_level = psi->ness_level; break;
    case 2:  learn_level = psi->paula_level; break;
    case 4:  learn_level = psi->poo_level; break;
    default: return 0;
    }

    /* Assembly line 60: if learn_level == 0, ability is not learnable */
    if (learn_level == 0) return 0;

    /* Assembly lines 68-73: compare learn_level with character's current level */
    if (char_id >= 1 && char_id <= TOTAL_PARTY_COUNT) {
        uint8_t current_level = party_characters[char_id - 1].level;
        if (learn_level <= current_level)
            return 1;
    }
    return 0;
}

/* --- HP/PP target modification functions --- */

/* HEAL_CHARACTER_HP: Port of asm/battle/heal_character_hp.asm (73 lines).
 * A=char_id, X=amount, Y=mode (0=percent, nonzero=absolute).
 * Modifies current_hp_target, wakes from KO, clamps to max_hp. */
void heal_character_hp(uint16_t char_id, uint16_t amount, uint16_t mode) {
    /* Assembly line 13: BEQ → if char_id==0, do nothing */
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    /* Assembly lines 17-30: if mode==0, convert percentage to absolute */
    uint16_t heal = amount;
    if (mode == 0) {
        heal = (uint16_t)((uint32_t)ch->max_hp * (uint32_t)amount / 100);
    }

    /* Assembly lines 31-42: hp_target += heal */
    ch->current_hp_target += heal;

    /* Assembly lines 43-50: if current_hp == 0, set to 1 (wake from KO) */
    if (ch->current_hp == 0) {
        ch->current_hp = 1;
    }

    /* Assembly lines 52-70: clamp hp_target to max_hp */
    if (ch->current_hp_target > ch->max_hp) {
        ch->current_hp_target = ch->max_hp;
    }
}

/* HEAL_CHARACTER_PP: Port of asm/battle/heal_character_pp.asm (58 lines).
 * A=char_id, X=amount, Y=mode. Modifies current_pp_target, clamps to max_pp. */
void heal_character_pp(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    uint16_t heal = amount;
    if (mode == 0) {
        heal = (uint16_t)((uint32_t)ch->max_pp * (uint32_t)amount / 100);
    }

    /* Assembly lines 31-44: pp_target += heal */
    ch->current_pp_target += heal;

    /* Assembly lines 45-55: clamp pp_target to max_pp */
    if (ch->current_pp_target > ch->max_pp) {
        ch->current_pp_target = ch->max_pp;
    }
}

/* REDUCE_HP_TARGET: Port of asm/battle/reduce_hp_target.asm (47 lines).
 * A=char_id, X=amount, Y=mode. Subtracts from hp_target, floors at 0. */
void reduce_hp_target(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    uint16_t damage = amount;
    if (mode == 0) {
        damage = (uint16_t)((uint32_t)ch->max_hp * (uint32_t)amount / 100);
    }

    /* Assembly lines 30-40: hp_target -= damage */
    uint16_t new_target = ch->current_hp_target - damage;
    /* Assembly lines 41-44: unsigned underflow check (result > max_hp → set 0) */
    if (new_target > ch->max_hp) {
        new_target = 0;
    }
    ch->current_hp_target = new_target;
}

/* REDUCE_PP_TARGET: Port of asm/battle/reduce_pp_target.asm (47 lines).
 * A=char_id, X=amount, Y=mode. Subtracts from pp_target, floors at 0. */
void reduce_pp_target(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0) return;
    uint16_t idx = char_id - 1;
    if (idx >= TOTAL_PARTY_COUNT) return;
    CharStruct *ch = &party_characters[idx];

    uint16_t cost = amount;
    if (mode == 0) {
        cost = (uint16_t)((uint32_t)ch->max_pp * (uint32_t)amount / 100);
    }

    uint16_t new_target = ch->current_pp_target - cost;
    if (new_target > ch->max_pp) {
        new_target = 0;
    }
    ch->current_pp_target = new_target;
}

/* RECOVER_HP_AMTPERCENT: Port of asm/misc/recover_hp_amtpercent.asm.
 * A=char_id (0xFF=all), X=amount, Y=mode. */
void recover_hp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            heal_character_hp((uint16_t)member, amount, mode);
        }
    } else {
        heal_character_hp(char_id, amount, mode);
    }
}

/* RECOVER_PP_AMTPERCENT: Port of asm/misc/recover_pp_amtpercent.asm. */
void recover_pp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            heal_character_pp((uint16_t)member, amount, mode);
        }
    } else {
        heal_character_pp(char_id, amount, mode);
    }
}

/* REDUCE_HP_AMTPERCENT: Port of asm/misc/reduce_hp_amtpercent.asm. */
void reduce_hp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            reduce_hp_target((uint16_t)member, amount, mode);
        }
    } else {
        reduce_hp_target(char_id, amount, mode);
    }
}

/* REDUCE_PP_AMTPERCENT: Port of asm/misc/reduce_pp_amtpercent.asm. */
void reduce_pp_amtpercent(uint16_t char_id, uint16_t amount, uint16_t mode) {
    if (char_id == 0xFF) {
        for (int i = 0; i < game_state.player_controlled_party_count; i++) {
            uint8_t member = game_state.party_members[i];
            reduce_pp_target((uint16_t)member, amount, mode);
        }
    } else {
        reduce_pp_target(char_id, amount, mode);
    }
}

/* RESET_HPPP_ROLLING: Port of asm/misc/reset_hppp_rolling.asm.
 * For each active party member:
 *   - If afflictions[0] != 1 && current_hp == 0: set hp_target = 1
 *   - If HP fraction rolling && current_hp > hp_target: snap hp_target = current_hp
 *   - If PP fraction rolling && current_pp > pp_target: snap pp_target = current_pp
 * Then sets fastest_hppp_meter_speed = 1. */
void reset_hppp_rolling(void) {
    uint8_t count = game_state.player_controlled_party_count;
    for (int i = 0; i < count; i++) {
        uint8_t member_id = game_state.party_members[i];
        if (member_id == 0) continue;
        CharStruct *ch = &party_characters[member_id - 1];

        /* Assembly lines 28-35: check afflictions[0] and current_hp */
        if (ch->afflictions[0] != 1 && ch->current_hp == 0) {
            ch->current_hp_target = 1;
        }

        /* Assembly lines 37-50: if HP fraction active, snap target up */
        if (ch->current_hp_fraction != 0) {
            if (ch->current_hp > ch->current_hp_target) {
                ch->current_hp_target = ch->current_hp;
            }
        }

        /* Assembly lines 52-65: if PP fraction active, snap target up */
        if (ch->current_pp_fraction != 0) {
            if (ch->current_pp > ch->current_pp_target) {
                ch->current_pp_target = ch->current_pp;
            }
        }
    }
    /* Assembly lines 75-78: set fastest meter speed */
    fastest_hppp_meter_speed = 1;
}

/* On-cartridge SRAM signature stamped into every valid block header. Real
 * EarthBound's CHECK_BLOCK_SIGNATURE (asm/system/saves/check_block_signature.asm)
 * STRCMPs the 28-byte header against this; a mismatch makes it ERASE the block.
 * From asm/data/sram_signature.asm — ASCIIZ, the remaining bytes stay zero. */
static const char SRAM_BLOCK_SIGNATURE[] = "HAL Laboratory, inc.";

/* Byte offset (within the SRAM image / .srm file) of the 16-bit SRAM version
 * word. Real EarthBound's CHECK_SRAM_INTEGRITY reads SAVE_BASE + $1FFE on boot
 * and, if it != SRAM_VERSION, memsets the ENTIRE 8 KB SRAM to zero (wiping every
 * slot). SAVE_BASE is file offset 0, so this is just $1FFE. Writing it keeps a
 * port-written .srm from being reformatted when loaded on real hardware/snes9x. */
#define SRAM_VERSION_WORD_OFFSET 0x1FFE
/* Full on-cartridge SRAM size. snes9x sizes the .srm from the ROM's SRAM header
 * and will not load a file that is short of this. The 6 save blocks only cover
 * the first 7680 bytes; padding out to 8192 makes the file the size emulators
 * expect (the gap stays zero). */
#define SRAM_IMAGE_SIZE 0x2000

/* Stamp the on-cartridge metadata that real EarthBound validates but the port's
 * own (checksum-only) loader ignores: the per-slot writes leave the version word
 * and file tail untouched, so refresh them on every save. */
static bool write_sram_version_word(void) {
    uint8_t ver[2] = { (uint8_t)(SRAM_VERSION & 0xFF), (uint8_t)(SRAM_VERSION >> 8) };
    /* Writing 2 bytes at $1FFE extends the file to the full 8 KB SRAM image
     * (the unwritten gap between the save blocks and here is zero-filled). */
    return platform_save_write(ver, SRAM_VERSION_WORD_OFFSET, sizeof(ver));
}

/* Compute ADD checksum for a save block.
 * Port of CALC_SAVE_BLOCK_ADD_CHECKSUM (asm/system/saves/calc_save_block_checksum.asm):
 * sums every byte of the save data (excluding the header). */
static uint16_t compute_add_checksum(const SaveBlock *block) {
    uint16_t sum = 0;
    const uint8_t *data = (const uint8_t *)&block->game_state;
    size_t size = sizeof(SaveBlock) - sizeof(SaveHeader);
    for (size_t i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

/* Compute XOR checksum for a save block.
 * Port of CALC_SAVE_BLOCK_XOR_CHECKSUM (asm/system/saves/calc_save_block_checksum_complement.asm):
 * XORs every 16-bit word of the save data (excluding the header). */
static uint16_t compute_xor_checksum(const SaveBlock *block) {
    uint16_t xor_sum = 0;
    const uint16_t *data = (const uint16_t *)&block->game_state;
    size_t word_count = (sizeof(SaveBlock) - sizeof(SaveHeader)) / 2;
    for (size_t i = 0; i < word_count; i++) {
        xor_sum ^= data[i];
    }
    return xor_sum;
}

bool save_game(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    memset(block, 0, sizeof(*block));

    /* Port of save_game_block.asm lines 22-23: TIMER → game_state.timer */
    game_state.timer = core.play_timer;

    /* Fill save block */
    memcpy(&block->game_state, &game_state, sizeof(GameState));
    memcpy(block->party_characters, party_characters, sizeof(party_characters));
    memcpy(block->event_flags, event_flags, sizeof(block->event_flags));
    memcpy(block->key_items_pool, key_items_pool, sizeof(key_items_pool));

    /* Stamp the on-cartridge block signature so real EarthBound's
     * CHECK_BLOCK_SIGNATURE accepts the slot instead of erasing it. The checksums
     * below cover only game_state onward (header excluded), so this does not
     * affect them — matching the assembly, which signs during format/erase. */
    memcpy(block->header.signature, SRAM_BLOCK_SIGNATURE, sizeof(SRAM_BLOCK_SIGNATURE));

    /* Compute checksums — assembly uses ADD + XOR, NOT ADD + ~ADD.
     * See validate_save_block_checksums.asm: checksum = ADD, checksum_complement = XOR. */
    block->header.checksum = compute_add_checksum(block);
    block->header.checksum_complement = compute_xor_checksum(block);

    /* Write both copies */
    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        size_t offset = (size_t)(slot * SAVE_COPY_COUNT + copy) * sizeof(SaveBlock);
        if (!platform_save_write(block, offset, sizeof(SaveBlock)))
            return false;
    }

    /* Refresh the global version word (and pad the image to 8 KB) so the file
     * survives real EarthBound's boot integrity check / loads in snes9x. */
    return write_sram_version_word();
}

/* See game_state.h's doc comment for the call-site list and what's
 * deliberately excluded. current_save_slot is 1-indexed (1-3) here, same
 * as the manual Save menu item's guard (mode_step_pause_menu(), text.c) --
 * save_game() itself takes a 0-indexed slot. */
void auto_save_if_enabled(void) {
    if (engine_auto_save != AUTO_SAVE_ON) return;
    if (current_save_slot < 1) return;
    save_game(current_save_slot - 1);
}

bool load_game(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    bool loaded = false;

    /* Try both copies */
    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        size_t offset = (size_t)(slot * SAVE_COPY_COUNT + copy) * sizeof(SaveBlock);
        if (platform_save_read(block, offset, sizeof(SaveBlock)) != sizeof(SaveBlock))
            continue;

        uint16_t checksum = compute_add_checksum(block);
        uint16_t xor_checksum = compute_xor_checksum(block);
        if (block->header.checksum == checksum &&
            block->header.checksum_complement == xor_checksum) {
            loaded = true;
            break;
        }
    }

    if (!loaded) return false;

    memcpy(&game_state, &block->game_state, sizeof(GameState));
    memcpy(party_characters, block->party_characters, sizeof(party_characters));
    memcpy(event_flags, block->event_flags, sizeof(event_flags));
    memcpy(key_items_pool, block->key_items_pool, sizeof(key_items_pool));

    /* Port of load_game_slot.asm lines 92-93: game_state.timer → TIMER */
    core.play_timer = game_state.timer;

    /* Key Items migration sweep (not part of the original ROM/assembly --
     * added for the Key Items pool feature). A save written before that
     * feature existed still has its key items sitting in the relevant
     * character's regular items[] slots (key_items_pool just loaded as
     * all-zero, since those bytes used to be unused padding). Move any
     * key-item-typed entries found there into the pool now, freeing their
     * inventory slots. Runs unconditionally on every load and is
     * idempotent -- an already-migrated save has no key items left in any
     * items[] to find, so this is a no-op for it. Uses
     * remove_item_from_inventory() (not a hand-rolled shift) so equipment
     * indices for that character's other items stay correct. */
    for (uint16_t char_id = 1; char_id <= TOTAL_PARTY_COUNT; char_id++) {
        CharStruct *ch = &party_characters[char_id - 1];
        for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; ) {
            uint8_t item_id = ch->items[slot];
            if (item_id != 0 && is_key_item_type(item_id)) {
                key_items_give(item_id);
                remove_item_from_inventory(char_id, slot + 1); /* 1-based slot */
                /* Slot has been refilled by the shift-left compaction (or
                 * cleared to 0 if this was the last item) -- re-check the
                 * same index rather than advancing. */
                continue;
            }
            slot++;
        }
    }

    return true;
}

/* Port of ERASE_SAVE (erase_save_slot.asm) + ERASE_SAVE_BLOCK (erase_save_block.asm):
 * Zeroes both copies of a save slot (primary + backup). The assembly leaves the
 * block's signature in place; an empty (all-zero) block is fine here because the
 * remaining slots' checksums still fail validation, so real EarthBound treats it
 * as empty. We still refresh the global version word so the rest of the image
 * (other slots) survives the boot integrity check on hardware/snes9x. */
bool erase_save(int slot) {
    if (slot < 0 || slot >= SAVE_COUNT) return false;

    SaveBlock *block = &ert.save_scratch;
    memset(block, 0, sizeof(*block));

    for (int copy = 0; copy < SAVE_COPY_COUNT; copy++) {
        size_t offset = (size_t)(slot * SAVE_COPY_COUNT + copy) * sizeof(SaveBlock);
        if (!platform_save_write(block, offset, sizeof(SaveBlock)))
            return false;
    }

    return write_sram_version_word();
}

/* See the doc comment on the declaration in game_state.h. */
bool key_items_selftest(void) {
    bool ok = true;
    const uint16_t TEST_ITEM = 202; /* Town map (ITEM_TYPE_KEY_ITEM) -- also
                                      * exercises the exact item town_map.c's
                                      * show_town_map_prepare() checks for. */
    const uint16_t JEFF = 3;        /* char_id 3 = Jeff (1-indexed) */

    /* Safety guard: this test calls save_game(0) below, a real write to
     * whatever earthbound.srm is in the current working directory --
     * confirmed to have destroyed a real player's save once, when this was
     * run inside a live deployed game directory instead of an isolated
     * scratch one. Peek at slot 0 first (same "occupied" check
     * file_select.c uses) and refuse the destructive part of this test if
     * it looks like a real save, rather than silently clobbering it. This
     * is a backstop, not a substitute for actually running this in a
     * scratch directory with no real save data -- it only catches the
     * "occupied" case; an empty/never-played slot 0 still gets
     * overwritten with test data, which is fine for a scratch dir but NOT
     * fine if that's meant to become someone's actual first save later. */
    if (load_game(0) && game_state.favourite_thing[1] != 0) {
        fprintf(stderr,
            "key_items_selftest: REFUSING TO RUN -- save slot 1 in the "
            "current directory's earthbound.srm looks like a real, "
            "played save (favourite_thing is set). This test calls "
            "save_game(0), which would overwrite it with synthetic test "
            "data. Run this in an isolated scratch directory with no "
            "real earthbound.srm instead.\n");
        return false;
    }

    game_state_init();

    /* --- 1. Simulate a pre-feature save: write the key item directly into
     * Jeff's regular inventory, bypassing every choke point (exactly what
     * an old save's raw bytes look like -- key_items_pool stays empty,
     * matching what those bytes read as before this field existed). */
    party_characters[JEFF - 1].items[0] = (uint8_t)TEST_ITEM;

    if (!save_game(0)) {
        fprintf(stderr, "key_items_selftest: save_game(pre-migration state) failed\n");
        return false;
    }

    /* --- 2. Load it back. The migration sweep in load_game() should move
     * the item into the pool and clear Jeff's slot. --- */
    game_state_init(); /* wipe live state so load_game() must reconstruct it */
    if (!load_game(0)) {
        fprintf(stderr, "key_items_selftest: load_game(pre-migration state) failed\n");
        return false;
    }

    if (party_characters[JEFF - 1].items[0] != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- item still in Jeff's items[0] "
                        "after load (migration sweep didn't run/clear it)\n");
        ok = false;
    }
    if (key_items_find(TEST_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- item not found in "
                        "key_items_pool after migration\n");
        ok = false;
    }

    /* --- 3. The pool itself must survive its own save/load round-trip
     * (not just the one-time migration path). --- */
    if (!save_game(0)) {
        fprintf(stderr, "key_items_selftest: save_game(post-migration state) failed\n");
        return false;
    }
    game_state_init();
    if (!load_game(0)) {
        fprintf(stderr, "key_items_selftest: load_game(post-migration state) failed\n");
        return false;
    }
    if (key_items_find(TEST_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- item lost across a normal "
                        "save/load round-trip of the pool\n");
        ok = false;
    }

    /* --- 4. Core acceptance criterion: bench the character who originally
     * "had" the item (remove Jeff from the controlled party) and confirm
     * find_item_in_inventory2()/CHAR_ID_ANY still finds it -- this is the
     * exact bug class the ShrineFox EarthBound Mod Menu has (their key
     * items are still tied to whichever character holds them, so lookups
     * that only scan the currently-controlled party miss it). Our pool has
     * no such dependency. */
    uint8_t saved_count = game_state.player_controlled_party_count;
    uint8_t saved_members[6];
    memcpy(saved_members, game_state.party_members, sizeof(saved_members));

    game_state.player_controlled_party_count = 3;
    game_state.party_members[0] = 1; /* Ness */
    game_state.party_members[1] = 2; /* Paula */
    game_state.party_members[2] = 4; /* Poo -- Jeff (3) deliberately excluded */

    if (find_item_in_inventory2(CHAR_ID_ANY, TEST_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- key item became unfindable "
                        "after benching the character who originally received it "
                        "(this is the ShrineFox-mod bug class)\n");
        ok = false;
    }

    game_state.player_controlled_party_count = saved_count;
    memcpy(game_state.party_members, saved_members, sizeof(saved_members));

    /* --- 5. Direct CRUD round-trip on a second item. --- */
    const uint16_t TEST_ITEM_2 = 176; /* Bicycle (ITEM_TYPE_KEY_AREA) */
    if (key_items_give(TEST_ITEM_2) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- key_items_give() failed on an "
                        "empty-enough pool\n");
        ok = false;
    }
    if (key_items_find(TEST_ITEM_2) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- key_items_find() didn't find "
                        "an item just given\n");
        ok = false;
    }
    if (key_items_remove(TEST_ITEM_2) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- key_items_remove() didn't "
                        "remove an item known to be present\n");
        ok = false;
    }
    if (key_items_find(TEST_ITEM_2) != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- key_items_find() still finds "
                        "an item after key_items_remove()\n");
        ok = false;
    }

    /* --- 6. get_character_item() sentinel regression check. Found live:
     * an item's own "use" script can re-fetch "the item currently being
     * used" via GET_CHARACTER_ITEM(working_memory, argument_memory) --
     * mode_step_use_item() answers that for a pool item by setting
     * item_slot to KEY_ITEMS_POOL_USE_SLOT_SENTINEL and calling
     * key_items_set_use_in_progress() first. Exercise that contract
     * directly (no mode-stack pump needed) rather than only the CRUD
     * layer underneath it, which is what let this regress silently the
     * first time -- the CRUD self-test above passed the whole time this
     * was broken live. */
    key_items_set_use_in_progress(TEST_ITEM);
    uint16_t sentinel_result = get_character_item(1, KEY_ITEMS_POOL_USE_SLOT_SENTINEL);
    if (sentinel_result != TEST_ITEM) {
        fprintf(stderr, "key_items_selftest: FAIL -- get_character_item() with the "
                        "pool-use sentinel slot returned %u, expected %u (this is "
                        "exactly the bug that made every key item's use-script "
                        "possession re-check silently fail)\n",
                sentinel_result, TEST_ITEM);
        ok = false;
    }
    key_items_set_use_in_progress(0); /* clear -- a normal slot lookup must not see it */
    if (get_character_item(1, KEY_ITEMS_POOL_USE_SLOT_SENTINEL) != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- get_character_item() still "
                        "returns a pool item after key_items_set_use_in_progress(0)\n");
        ok = false;
    }

    return ok;
}
