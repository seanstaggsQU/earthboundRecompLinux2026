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
uint8_t party_ever_joined_mask;
uint8_t fastest_hppp_meter_speed;
uint8_t current_save_slot;

void game_state_init(void) {
    memset(&game_state, 0, sizeof(game_state));
    memset(party_characters, 0, sizeof(party_characters));
    memset(event_flags, 0, sizeof(event_flags));
    memset(key_items_pool, 0, sizeof(key_items_pool));
    party_ever_joined_mask = 0;

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

/* HP/PP target modification functions */

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
 * From asm/data/sram_signature.asm, ASCIIZ, the remaining bytes stay zero. */
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

/* fuzzy pickles. thats it thats the comment */
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
    block->party_ever_joined_mask = party_ever_joined_mask;

    /* Stamp the on-cartridge block signature so real EarthBound's
     * CHECK_BLOCK_SIGNATURE accepts the slot instead of erasing it. The checksums
     * below cover only game_state onward (header excluded), so this does not
     * affect them, matching the assembly, which signs during format/erase. */
    memcpy(block->header.signature, SRAM_BLOCK_SIGNATURE, sizeof(SRAM_BLOCK_SIGNATURE));

    /* Compute checksums, assembly uses ADD + XOR, NOT ADD + ~ADD.
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
    party_ever_joined_mask = block->party_ever_joined_mask;

    /* Port of load_game_slot.asm lines 92-93: game_state.timer → TIMER */
    core.play_timer = game_state.timer;

    /* Key Items migration sweep (not part of the original ROM/assembly --
     * added for the Key Items pool feature). Moves any key-item-typed
     * entries still sitting in a character's regular items[] slots into
     * the pool, freeing the slot. Idempotent (an already-migrated save has
     * nothing left to find). Uses remove_item_from_inventory() (not a
     * hand-rolled shift) so equipment indices for that character's other
     * items stay correct.
     *
     * party_ever_joined_mask == 0 gates which characters this sweep
     * touches -- see the field's doc comment (game_state.h) for the full
     * rationale. Two cases produce a key item sitting in items[]:
     *   (a) a save written before the Key Items pool feature existed at
     *       all: no version marker distinguishes it, but the mask being
     *       entirely 0 does (new code always sets Ness's bit at new-game
     *       start, so no save this code wrote can have an all-zero mask).
     *       Sweep every character unconditionally, matching this sweep's
     *       original (pre-mask) behavior for such a save.
     *   (b) a new-format save where a character hasn't joined the party
     *       yet: new-game setup deliberately leaves their INITIAL_STATS
     *       starting key item (e.g. Poo's Tiny Ruby) sitting in items[]
     *       until they actually join (see file_select.c/add_char_to_party()
     *       doc comments) -- sweeping it here would leak it into the pool
     *       before the player has met them. Only sweep characters whose
     *       bit is already set (they've joined at least once); a
     *       not-yet-joined character's items[] is left untouched. */
    bool legacy_save = (party_ever_joined_mask == 0);
    for (uint16_t char_id = 1; char_id <= TOTAL_PARTY_COUNT; char_id++) {
        if (!legacy_save && !(party_ever_joined_mask & (1 << (char_id - 1))))
            continue; /* not-yet-joined in a new-format save: leave as-is */
        CharStruct *ch = &party_characters[char_id - 1];
        for (uint16_t slot = 0; slot < ITEM_INVENTORY_SIZE; ) {
            uint8_t item_id = ch->items[slot];
            if (item_id != 0 && is_key_item_type(item_id)) {
                key_items_give(item_id);
                remove_item_from_inventory(char_id, slot + 1); /* 1-based slot */
                /* Slot has been refilled by the shift-left compaction (or
                 * cleared to 0 if this was the last item), re-check the
                 * same index rather than advancing. */
                continue;
            }
            slot++;
        }
    }

    /* A legacy save has no usable party_ever_joined_mask of its own (that's
     * exactly what marks it as legacy) -- reconstruct one from its actual
     * party_members[] before returning, matching new-code semantics, or
     * this same load_game() call's caller would still see an all-zero mask
     * and this save would keep re-running the full unconditional sweep
     * (harmlessly idempotent, but not the "new-format" behavior a save
     * written from here on out should have).
     *
     * Run this sync UNCONDITIONALLY, not just when legacy_save -- a save
     * can also have a non-zero-but-INCOMPLETE mask, missing a bit for a
     * character who is definitely, currently active in party_members[]
     * right now. Confirmed live: a save with real, long-completed Paula
     * and Jeff joins still had their bits unset in party_ever_joined_mask
     * (0x31 instead of 0x37+), even though a fresh, isolated join->save->
     * load round-trip on current code preserves the mask correctly --
     * meaning the corruption predates whatever earlier fix landed for
     * this same subsystem (see migrate_key_items_to_pool()'s own history)
     * and this save simply never had a chance to self-correct, since the
     * legacy_save gate only ever catches an all-zero mask, not a partial
     * one. Syncing from the current roster on every load is always safe
     * regardless of cause: if someone is in party_members[] right now,
     * they have unambiguously joined at least once, whatever the mask
     * says. Purely additive (never clears a bit), so it can't un-set
     * anything a legitimate not-yet-rejoined former member's bit still
     * correctly records. */
    for (uint16_t i = 0; i < TOTAL_PARTY_COUNT; i++) {
        uint8_t member = game_state.party_members[i];
        if (member >= 1 && member <= TOTAL_PARTY_COUNT)
            party_ever_joined_mask |= (uint8_t)(1 << (member - 1));
    }

    /* FLG_JEFF (event flag 14, no named C constant -- flags are referenced
     * purely by number at this layer, matching the compiled dialogue
     * data) gates a real, live gameplay check: E05THRK.yml's Threed
     * gatekeeper NPCs re-arm the entire hotel-zombie sequence from its
     * very first stage on every conversation, specifically as long as
     * this flag reads unset, so a stuck-clear FLG_JEFF is directly
     * player-visible and disruptive, not just an inert data mismatch --
     * confirmed live as the sequence replaying indefinitely on a save
     * where Jeff had genuinely, fully joined (the entire canonical join
     * sequence completed) long before. That flag is dialogue-engine-set
     * (EEVENT2.yml/EEVENT1.yml's own add_party_member/set_event_flag
     * opcodes, a different, already-reliable code path from the
     * movement-script opcode bug fixed earlier in this file's history),
     * but has no self-healing mechanism of its own the way
     * party_ever_joined_mask now does above -- if Jeff's mask bit is
     * confirmed set (by the sync just above, from whatever source), that
     * unambiguously means he has really joined, so repair FLG_JEFF to
     * match rather than trust it blindly. Purely additive here too,
     * same reasoning as the mask sync. */
    if ((party_ever_joined_mask & 0x04) && !event_flag_get(14)) {
        event_flag_set(14);
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
     * scratch directory with no real save data, it only catches the
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
     * an old save's raw bytes look like, key_items_pool stays empty,
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
     * find_item_in_inventory2()/CHAR_ID_ANY still finds it, this is the
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

    /* 5. Direct CRUD round-trip on a second item. */
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
     * first time, the CRUD self-test above passed the whole time this
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

    /* --- 7. Not-yet-joined character's starting key item must not leak into
     * the pool early. Regression for the real Poo/Tiny Ruby bug: new-game
     * setup seeds all 4 characters' items[] up front (matching the original
     * SRAM layout), but Poo hasn't actually joined the party at that point
     * -- his key item must stay invisible until something actually surfaces
     * it, not appear in the pool from the first frame.
     *
     * Exercises migrate_key_items_to_pool() directly rather than going
     * through add_char_to_party() (its real caller on a genuine join):
     * that function also drives entity/position-buffer setup meant for a
     * live, fully-initialized game world, which this synthetic
     * game_state_init()-only harness never provides -- calling it here
     * hung the self-test outright rather than just failing the assertion. */
    game_state_init();
    const uint16_t POO = 4; /* char_id 4 = Poo (1-indexed) */
    const uint16_t POO_ITEM = 208; /* Tiny Ruby (ITEM_TYPE_KEY_SOMEONE) */
    party_characters[POO - 1].items[0] = (uint8_t)POO_ITEM;
    /* Poo not yet in party_members: game_state_init() leaves it that way. */

    if (key_items_find(POO_ITEM) != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- not-yet-joined Poo's starting "
                        "key item is already visible in the pool before he's "
                        "joined the party\n");
        ok = false;
    }

    migrate_key_items_to_pool(POO);

    if (party_characters[POO - 1].items[0] != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- Poo's item still in items[0] "
                        "after migrate_key_items_to_pool() (didn't clear it)\n");
        ok = false;
    }
    if (key_items_find(POO_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- Poo's key item not found in "
                        "the pool after migrate_key_items_to_pool()\n");
        ok = false;
    }

    /* Idempotency: a second call (e.g. Poo temporarily leaving and
     * rejoining the party) must be a harmless no-op, not re-add/duplicate. */
    migrate_key_items_to_pool(POO);
    if (key_items_find(POO_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- item vanished from the pool "
                        "after a second migrate_key_items_to_pool() call\n");
        ok = false;
    }

    /* --- 8. The actual real-game scenario: a normal SRAM save/load
     * (save_game()/load_game(), NOT the F6/F7 savestate) taken while Poo
     * hasn't joined yet must not leak his key item on load. An
     * unconditional migration sweep in load_game() can't tell "a
     * genuinely pre-feature save" apart from "a new-format save where a
     * character just hasn't joined yet", and would migrate both the same
     * way. party_ever_joined_mask (game_state.h) is what lets it tell
     * them apart -- exercise that discrimination directly,
     * simulating a real new game: Ness already active (his mask bit set,
     * matching what file_select.c does at new-game start), Poo seeded but
     * not yet joined. */
    game_state_init();
    const uint16_t NESS_ITEM = 177; /* ATM Card (ITEM_TYPE_KEY_SOMEONE) */
    party_characters[0].items[0] = (uint8_t)NESS_ITEM;
    migrate_key_items_to_pool(1); /* Ness: matches file_select.c's new-game call */
    game_state.party_members[0] = 1; /* Ness active, matching a real new game */
    party_characters[POO - 1].items[0] = (uint8_t)POO_ITEM; /* Poo: seeded, NOT joined */

    if (!save_game(0)) {
        fprintf(stderr, "key_items_selftest: save_game(Poo not yet joined) failed\n");
        return false;
    }
    game_state_init(); /* wipe live state so load_game() must reconstruct it */
    if (!load_game(0)) {
        fprintf(stderr, "key_items_selftest: load_game(Poo not yet joined) failed\n");
        return false;
    }

    if (key_items_find(POO_ITEM) != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- a normal save/load while Poo "
                        "hasn't joined leaked his key item into the pool (the real "
                        "bug this whole mask exists to prevent)\n");
        ok = false;
    }
    if (party_characters[POO - 1].items[0] != (uint8_t)POO_ITEM) {
        fprintf(stderr, "key_items_selftest: FAIL -- Poo's not-yet-joined item was "
                        "disturbed by save/load (should be untouched in items[0])\n");
        ok = false;
    }
    if (key_items_find(NESS_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- Ness's already-migrated key "
                        "item didn't survive the save/load round-trip\n");
        ok = false;
    }

    /* Now Poo actually joins (mid-session, same save slot) -- his item must
     * finally surface, and a SECOND save/load round-trip must keep it
     * migrated (not re-defer it). */
    migrate_key_items_to_pool(POO);
    game_state.party_members[1] = (uint8_t)POO;

    if (!save_game(0)) {
        fprintf(stderr, "key_items_selftest: save_game(Poo just joined) failed\n");
        return false;
    }
    game_state_init();
    if (!load_game(0)) {
        fprintf(stderr, "key_items_selftest: load_game(Poo just joined) failed\n");
        return false;
    }
    if (key_items_find(POO_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- Poo's key item, already "
                        "migrated before this save, was lost across save/load\n");
        ok = false;
    }

    /* --- 9. The same deferral, but for a key item PICKED UP during a
     * not-yet-joined character's solo section (e.g. Jeff's Pencil Eraser
     * in Winters), not just a new-game-seeded starting item.
     * give_item_to_specific_character()/give_item_to_character() route
     * every key item straight into the shared pool unconditionally without
     * this deferral, so a not-yet-joined character's item would be
     * visible/usable by every other character immediately -- e.g. Jeff
     * using Ness's already-pooled Pencil Eraser on an obstacle meant to
     * stay until Jeff actually joins. Both give paths route through
     * give_item_to_specific_character() eventually, so exercising it
     * directly covers give_item_to_character() too. */
    game_state_init();
    /* JEFF (char_id 3) already declared above. */
    const uint16_t JEFF_ITEM = 184; /* Pencil Eraser (ITEM_TYPE_KEY_SOMEONE) */
    /* Jeff not yet in party_members, not yet migrated: matches solo play. */

    uint16_t give_result = give_item_to_specific_character(JEFF, JEFF_ITEM);
    if (give_result != JEFF) {
        fprintf(stderr, "key_items_selftest: FAIL -- give_item_to_specific_character() "
                        "for not-yet-joined Jeff returned %u, expected %u (success)\n",
                        give_result, JEFF);
        ok = false;
    }
    if (party_characters[JEFF - 1].items[0] != (uint8_t)JEFF_ITEM) {
        fprintf(stderr, "key_items_selftest: FAIL -- not-yet-joined Jeff's picked-up "
                        "key item didn't land in his own items[0]\n");
        ok = false;
    }
    if (key_items_find(JEFF_ITEM) != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- not-yet-joined Jeff's picked-up "
                        "key item is already visible in the shared pool -- other "
                        "characters could use it before Jeff has even joined\n");
        ok = false;
    }
    if (find_item_in_inventory2(JEFF, JEFF_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- find_item_in_inventory2() can't "
                        "find Jeff's own not-yet-migrated key item (should fall "
                        "through to items[] on a pool miss)\n");
        ok = false;
    }

    /* Jeff joins: his item must migrate into the shared pool, same as any
     * other character's, and become visible/removable through the normal
     * pool-facing paths from then on. */
    migrate_key_items_to_pool(JEFF);
    game_state.party_members[0] = (uint8_t)JEFF;

    if (party_characters[JEFF - 1].items[0] != 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- Jeff's item still in items[0] "
                        "after he joined and migrate_key_items_to_pool() ran\n");
        ok = false;
    }
    if (key_items_find(JEFF_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- Jeff's key item not in the "
                        "shared pool after he joined\n");
        ok = false;
    }
    if (take_item_from_character(0xFF, JEFF_ITEM) == 0) {
        fprintf(stderr, "key_items_selftest: FAIL -- take_item_from_character() "
                        "couldn't remove Jeff's now-pooled key item\n");
        ok = false;
    }

    return ok;
}

/* Regression test for apply_join_level_scaling() (not a ROM/assembly port --
 * see that function's own doc comment, inventory.c, for the full
 * rationale): Paula joins at half Ness's current level, Jeff at two-thirds,
 * Poo matches Ness exactly, each rounded and re-grown through the normal
 * incremental level-up path, with current HP/PP synced to the new max.
 * Pure in-memory, no save_game()/load_game() involved, so no destructive-
 * write guard needed. Calls apply_join_level_scaling() directly rather
 * than going through add_char_to_party() (its real caller on a genuine
 * join): that function also drives live entity/position-buffer setup this
 * synthetic game_state_init()-only harness never provides -- same reason
 * key_items_selftest() (above) exercises migrate_key_items_to_pool()
 * directly instead of add_char_to_party() too. */
bool join_level_scaling_selftest(void) {
    bool ok = true;
    game_state_init();

    /* Ness never goes through add_char_to_party() -- simulate his
     * already-active state directly, same as a real playthrough by the
     * time anyone else could plausibly join. */
    game_state.party_members[0] = 1;
    game_state.party_count = 1;
    game_state.player_controlled_party_count = 1;
    party_characters[0].level = 10;

    apply_join_level_scaling(2); /* Paula */
    uint16_t paula_level = party_characters[1].level;
    if (paula_level != 3) {
        fprintf(stderr, "join_level_scaling_selftest: FAIL -- Paula joined at level %u, "
                        "expected 3 (25%% of Ness's level 10, rounded)\n", paula_level);
        ok = false;
    }
    if (party_characters[1].current_hp != party_characters[1].max_hp ||
        party_characters[1].current_pp != party_characters[1].max_pp) {
        fprintf(stderr, "join_level_scaling_selftest: FAIL -- Paula's current HP/PP "
                        "not synced to max after joining\n");
        ok = false;
    }

    /* Jeff is deliberately excluded from level scaling -- he joins at his
     * own default vanilla starting level, untouched. Seed a sentinel
     * level/HP/PP first (game_state_init() zeroed everything) so this can
     * confirm apply_join_level_scaling() really is a complete no-op for
     * him, not just "didn't crash". */
    party_characters[2].level = 1;
    party_characters[2].max_hp = 32;
    party_characters[2].current_hp = 20; /* deliberately not synced to max yet */
    party_characters[2].max_pp = 16;
    party_characters[2].current_pp = 10;
    apply_join_level_scaling(3); /* Jeff */
    if (party_characters[2].level != 1 || party_characters[2].current_hp != 20 ||
        party_characters[2].current_pp != 10) {
        fprintf(stderr, "join_level_scaling_selftest: FAIL -- Jeff was modified by "
                        "apply_join_level_scaling() (level=%u hp=%u pp=%u), expected "
                        "no change at all -- he joins at his own default level\n",
                        party_characters[2].level, party_characters[2].current_hp,
                        party_characters[2].current_pp);
        ok = false;
    }

    apply_join_level_scaling(4); /* Poo */
    uint16_t poo_level = party_characters[3].level;
    if (poo_level != 2) {
        fprintf(stderr, "join_level_scaling_selftest: FAIL -- Poo joined at level %u, "
                        "expected 2 (20%% of Ness's level 10, rounded)\n", poo_level);
        ok = false;
    }
    if (party_characters[3].current_hp != party_characters[3].max_hp ||
        party_characters[3].current_pp != party_characters[3].max_pp) {
        fprintf(stderr, "join_level_scaling_selftest: FAIL -- Poo's current HP/PP "
                        "not synced to max after joining\n");
        ok = false;
    }

    /* Low-level edge case: Ness at level 1 must never compute a target
     * level of 0 for anyone (reset_char_level_one(0, ...) would leave
     * ch->level unset by its own loop, but the explicit clamp in
     * apply_join_level_scaling() is what actually guarantees this, not
     * reliance on that side effect). */
    game_state_init();
    game_state.party_members[0] = 1;
    party_characters[0].level = 1;
    apply_join_level_scaling(2);
    if (party_characters[1].level < 1) {
        fprintf(stderr, "join_level_scaling_selftest: FAIL -- Paula joined at level %u "
                        "with Ness at level 1, expected >= 1\n", party_characters[1].level);
        ok = false;
    }

    return ok;
}
