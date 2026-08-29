/*
 * Opcode table for the EarthBound text bytecode format.
 * See text_opcodes.h's doc comment -- direct port of
 * ebtools/text_dsl/opcodes.py, kept in sync by hand.
 */
#include "data/text_opcodes.h"
#include <string.h>

#define U8   TEXT_ARG_U8
#define U16  TEXT_ARG_U16
#define U24  TEXT_ARG_U24
#define U32  TEXT_ARG_U32
#define LBL  TEXT_ARG_LABEL
#define JMPT TEXT_ARG_JUMP_TABLE
#define STR  TEXT_ARG_STRING

/* Shorthand macros for building TextOpcodeSpec entries below. bytes_count
 * of 1 vs 2 selects which of the two-element bytes[] array elements are
 * meaningful; the unused slot is left 0 (never read, byte_count gates it). */
#define OP0(name_, b0_) \
    { name_, { b0_, 0 }, 1, 0, {{0}} }
#define OP0_2(name_, b0_, b1_) \
    { name_, { b0_, b1_ }, 2, 0, {{0}} }
#define OP1(name_, b0_, a0n_, a0t_) \
    { name_, { b0_, 0 }, 1, 1, { { a0n_, a0t_ } } }
#define OP2(name_, b0_, a0n_, a0t_, a1n_, a1t_) \
    { name_, { b0_, 0 }, 1, 2, { { a0n_, a0t_ }, { a1n_, a1t_ } } }
#define OP1_2(name_, b0_, b1_, a0n_, a0t_) \
    { name_, { b0_, b1_ }, 2, 1, { { a0n_, a0t_ } } }
#define OP2_2(name_, b0_, b1_, a0n_, a0t_, a1n_, a1t_) \
    { name_, { b0_, b1_ }, 2, 2, { { a0n_, a0t_ }, { a1n_, a1t_ } } }
#define OP3_2(name_, b0_, b1_, a0n_, a0t_, a1n_, a1t_, a2n_, a2t_) \
    { name_, { b0_, b1_ }, 2, 3, { { a0n_, a0t_ }, { a1n_, a1t_ }, { a2n_, a2t_ } } }
#define OP5_2(name_, b0_, b1_, a0n_, a0t_, a1n_, a1t_, a2n_, a2t_, a3n_, a3t_, a4n_, a4t_) \
    { name_, { b0_, b1_ }, 2, 5, { { a0n_, a0t_ }, { a1n_, a1t_ }, { a2n_, a2t_ }, { a3n_, a3t_ }, { a4n_, a4t_ } } }

const TextOpcodeSpec TEXT_OPCODES[] = {
    /* === Primary codes 0x00-0x14 === */
    OP0("line_break", 0x00),
    OP0("start_new_line", 0x01),
    OP0("end_block", 0x02),
    OP0("halt_with_prompt", 0x03),
    OP1("set_event_flag", 0x04, "flag", U16),
    OP1("clear_event_flag", 0x05, "flag", U16),
    OP2("jump_if_flag_set", 0x06, "flag", U16, "dest", LBL),
    OP1("check_event_flag", 0x07, "flag", U16),
    OP1("call_text", 0x08, "dest", LBL),
    OP1("jump_multi", 0x09, "targets", JMPT),
    OP1("jump", 0x0A, "dest", LBL),
    OP1("test_if_workmem_true", 0x0B, "value", U8),
    OP1("test_if_workmem_false", 0x0C, "value", U8),
    OP1("copy_to_argmem", 0x0D, "value", U8),
    OP1("store_to_argmem", 0x0E, "value", U8),
    OP0("increment_workmem", 0x0F),
    OP1("pause", 0x10, "frames", U8),
    OP0("create_selection_menu", 0x11),
    OP0("clear_text_line", 0x12),
    OP0("halt_without_prompt", 0x13),
    OP0("halt_with_prompt_always", 0x14),

    /* === 0x18 xx: Window operations === */
    OP0_2("close_window", 0x18, 0x00),
    OP1_2("open_window", 0x18, 0x01, "window", U8),
    OP0_2("save_window_text_attributes", 0x18, 0x02),
    OP1_2("switch_to_window", 0x18, 0x03, "window_id", U8),
    OP0_2("close_all_windows", 0x18, 0x04),
    OP2_2("force_text_alignment", 0x18, 0x05, "x", U8, "y", U8),
    OP0_2("clear_window", 0x18, 0x06),
    OP2_2("check_for_inequality", 0x18, 0x07, "address", U32, "value", U8),
    OP1_2("selection_menu_no_cancel", 0x18, 0x08, "value", U24),
    OP1_2("selection_menu_allow_cancel", 0x18, 0x09, "value", U8),
    OP0_2("show_wallet_window", 0x18, 0x0A),

    /* === 0x19 xx: Memory/string operations === */
    OP1_2("load_string_to_memory", 0x19, 0x02, "payload", STR),
    OP0_2("clear_loaded_strings", 0x19, 0x04),
    OP3_2("inflict_status", 0x19, 0x05, "party_member", U8, "status_group", U8, "status", U8),
    OP1_2("get_character_number", 0x19, 0x10, "value", U8),
    OP1_2("get_character_name_letter", 0x19, 0x11, "value", U8),
    OP0_2("get_escargo_express_item", 0x19, 0x14),
    OP2_2("get_character_status", 0x19, 0x16, "character", U8, "status", U8),
    OP1_2("get_exp_for_next_level", 0x19, 0x18, "character", U8),
    OP2_2("add_item_id_to_work_memory", 0x19, 0x19, "slot", U8, "index", U8),
    OP1_2("get_escargo_express_item_by_slot", 0x19, 0x1A, "slot", U8),
    OP1_2("get_window_menu_option_count", 0x19, 0x1B, "value", U8),
    OP2_2("transfer_item_to_queue", 0x19, 0x1C, "arg1", U8, "arg2", U8),
    OP2_2("get_queued_item_data", 0x19, 0x1D, "arg1", U8, "arg2", U8),
    OP0_2("get_current_number", 0x19, 0x1E),
    OP0_2("get_current_inventory_item", 0x19, 0x1F),
    OP0_2("get_player_controlled_party_count", 0x19, 0x20),
    OP1_2("is_item_drink", 0x19, 0x21, "value", U8),
    OP3_2("get_direction_of_object_from_character", 0x19, 0x22, "character", U8, "object_type", U8, "object_id", U16),
    OP3_2("get_direction_of_object_from_npc", 0x19, 0x23, "npc_x", U16, "npc_y", U16, "direction", U8),
    OP2_2("get_direction_of_object_from_sprite", 0x19, 0x24, "sprite_x", U16, "sprite_y", U16),
    OP1_2("is_item_condiment", 0x19, 0x25, "value", U8),
    OP1_2("set_respawn_point", 0x19, 0x26, "point", U8),
    OP1_2("resolve_cc_table_data", 0x19, 0x27, "index", U8),
    OP1_2("get_letter_from_stat", 0x19, 0x28, "stat", U8),

    /* === 0x1A xx: UI menus === */
    OP5_2("party_member_selection_menu_uncancellable", 0x1A, 0x01,
          "dest1", LBL, "dest2", LBL, "dest3", LBL, "dest4", LBL, "arg", U8),
    OP2_2("show_character_inventory", 0x1A, 0x05, "character", U8, "mode", U8),
    OP1_2("display_shop_menu", 0x1A, 0x06, "shop_id", U8),
    OP0_2("select_escargo_express_item", 0x1A, 0x07),
    OP0_2("open_phone_menu", 0x1A, 0x0A),

    /* === 0x1B xx: Memory operations === */
    OP0_2("copy_active_memory_to_storage", 0x1B, 0x00),
    OP0_2("copy_storage_memory_to_active", 0x1B, 0x01),
    OP1_2("jump_if_false", 0x1B, 0x02, "dest", LBL),
    OP1_2("jump_if_true", 0x1B, 0x03, "dest", LBL),
    OP0_2("swap_working_and_arg_memory", 0x1B, 0x04),
    OP0_2("copy_active_memory_to_working_memory", 0x1B, 0x05),
    OP0_2("copy_working_memory_to_active_memory", 0x1B, 0x06),

    /* === 0x1C xx: Printing === */
    OP1_2("text_colour_effects", 0x1C, 0x00, "effect", U8),
    OP1_2("print_stat", 0x1C, 0x01, "stat", U8),
    OP1_2("print_char_name", 0x1C, 0x02, "character", U8),
    OP1_2("print_char", 0x1C, 0x03, "char_code", U8),
    OP0_2("open_hp_pp_windows", 0x1C, 0x04),
    OP1_2("print_item_name", 0x1C, 0x05, "item", U8),
    OP1_2("print_teleport_destination_name", 0x1C, 0x06, "destination", U8),
    OP1_2("print_horizontal_text_string", 0x1C, 0x07, "string_id", U8),
    OP1_2("print_special_gfx", 0x1C, 0x08, "gfx_id", U8),
    OP0_2("set_number_padding", 0x1C, 0x09),
    OP1_2("print_number", 0x1C, 0x0A, "address", U32),
    OP1_2("print_money_amount", 0x1C, 0x0B, "address", U32),
    OP1_2("print_vertical_text_string", 0x1C, 0x0C, "string_id", U8),
    OP0_2("print_action_user_name", 0x1C, 0x0D),
    OP0_2("print_action_target_name", 0x1C, 0x0E),
    OP0_2("print_action_amount", 0x1C, 0x0F),
    OP1_2("hint_new_line", 0x1C, 0x11, "width", U8),
    OP1_2("print_psi_name", 0x1C, 0x12, "psi_id", U8),
    OP2_2("display_psi_animation", 0x1C, 0x13, "psi_id", U8, "level", U8),
    OP1_2("load_special", 0x1C, 0x14, "special_id", U8),
    OP1_2("load_special_for_jump_multi", 0x1C, 0x15, "special_id", U8),

    /* === 0x1D xx: Items/money === */
    OP2_2("give_item_to_character", 0x1D, 0x00, "character", U8, "item", U8),
    OP2_2("take_item_from_character", 0x1D, 0x01, "character", U8, "item", U8),
    OP1_2("get_player_has_inventory_full", 0x1D, 0x02, "character", U8),
    OP1_2("get_player_has_inventory_room", 0x1D, 0x03, "character", U8),
    OP2_2("check_if_character_doesnt_have_item", 0x1D, 0x04, "character", U8, "item", U8),
    OP2_2("check_if_character_has_item", 0x1D, 0x05, "character", U8, "item", U8),
    OP1_2("add_to_atm", 0x1D, 0x06, "amount", U32),
    OP1_2("take_from_atm", 0x1D, 0x07, "amount", U32),
    OP1_2("add_to_wallet", 0x1D, 0x08, "amount", U16),
    OP1_2("take_from_wallet", 0x1D, 0x09, "amount", U16),
    OP1_2("get_buy_price_of_item", 0x1D, 0x0A, "item", U8),
    OP1_2("get_sell_price_of_item", 0x1D, 0x0B, "item", U8),
    OP1_2("escargo_express_item_status", 0x1D, 0x0C, "value", U16),
    OP3_2("character_has_ailment", 0x1D, 0x0D, "character", U8, "status_group", U8, "status", U8),
    OP2_2("give_item_to_character_b", 0x1D, 0x0E, "character", U8, "item", U8),
    OP1_2("take_item_from_character_2", 0x1D, 0x0F, "value", U16),
    OP1_2("check_item_equipped", 0x1D, 0x10, "value", U16),
    OP1_2("check_item_usable_by_slot", 0x1D, 0x11, "value", U16),
    OP1_2("escargo_express_move", 0x1D, 0x12, "value", U16),
    OP1_2("deliver_escargo_express_item", 0x1D, 0x13, "value", U16),
    OP1_2("have_enough_money", 0x1D, 0x14, "amount", U32),
    OP1_2("put_val_in_argmem", 0x1D, 0x15, "value", U16),
    OP1_2("have_enough_money_in_atm", 0x1D, 0x17, "amount", U32),
    OP1_2("escargo_express_store", 0x1D, 0x18, "value", U8),
    OP1_2("have_x_party_members", 0x1D, 0x19, "count", U8),
    OP0_2("test_is_user_targetting_self", 0x1D, 0x20),
    OP1_2("generate_random_number", 0x1D, 0x21, "max_value", U8),
    OP0_2("test_if_exit_mouse_usable", 0x1D, 0x22),
    OP1_2("get_item_category", 0x1D, 0x23, "value", U8),
    OP1_2("get_game_state_c4", 0x1D, 0x24, "value", U8),

    /* === 0x1E xx: Stat modifications === */
    OP2_2("recover_hp_percent", 0x1E, 0x00, "character", U8, "percent", U8),
    OP2_2("deplete_hp_percent", 0x1E, 0x01, "character", U8, "percent", U8),
    OP2_2("recover_hp_amount", 0x1E, 0x02, "character", U8, "amount", U8),
    OP2_2("deplete_hp_amount", 0x1E, 0x03, "character", U8, "amount", U8),
    OP2_2("recover_pp_percent", 0x1E, 0x04, "character", U8, "percent", U8),
    OP2_2("deplete_pp_percent", 0x1E, 0x05, "character", U8, "percent", U8),
    OP2_2("recover_pp_amount", 0x1E, 0x06, "character", U8, "amount", U8),
    OP2_2("deplete_pp_amount", 0x1E, 0x07, "character", U8, "amount", U8),
    OP2_2("set_character_level", 0x1E, 0x08, "character", U8, "level", U8),
    OP2_2("give_experience", 0x1E, 0x09, "character", U8, "amount", U24),
    OP2_2("boost_iq", 0x1E, 0x0A, "character", U8, "amount", U16),
    OP2_2("boost_guts", 0x1E, 0x0B, "character", U8, "amount", U16),
    OP2_2("boost_speed", 0x1E, 0x0C, "character", U8, "amount", U16),
    OP2_2("boost_vitality", 0x1E, 0x0D, "character", U8, "amount", U16),
    OP2_2("boost_luck", 0x1E, 0x0E, "character", U8, "amount", U16),

    /* === 0x1F xx: Entity/world operations === */
    OP2_2("play_music", 0x1F, 0x00, "flag", U8, "track", U8),
    OP1_2("stop_music", 0x1F, 0x01, "fade_time", U8),
    OP1_2("play_sound", 0x1F, 0x02, "sfx", U8),
    OP0_2("restore_default_music", 0x1F, 0x03),
    OP1_2("set_text_printing_sound", 0x1F, 0x04, "sound", U8),
    OP0_2("disable_sector_music_change", 0x1F, 0x05),
    OP0_2("enable_sector_music_change", 0x1F, 0x06),
    OP1_2("apply_music_effect", 0x1F, 0x07, "effect", U8),
    OP1_2("add_party_member", 0x1F, 0x11, "member", U8),
    OP1_2("remove_party_member", 0x1F, 0x12, "member", U8),
    OP2_2("change_character_direction", 0x1F, 0x13, "character", U8, "direction", U8),
    OP1_2("change_party_direction", 0x1F, 0x14, "direction", U8),
    OP3_2("generate_active_sprite", 0x1F, 0x15, "sprite", U16, "movement", U16, "arg", U8),
    OP2_2("change_tpt_entry_direction", 0x1F, 0x16, "tpt_entry", U16, "direction", U8),
    OP3_2("create_entity", 0x1F, 0x17, "entity_id", U16, "movement", U16, "arg", U8),
    OP2_2("create_floating_sprite_near_tpt_entry", 0x1F, 0x1A, "tpt_entry", U16, "sprite_type", U8),
    OP1_2("delete_floating_sprite_near_tpt_entry", 0x1F, 0x1B, "tpt_entry", U16),
    OP2_2("create_floating_sprite_near_character", 0x1F, 0x1C, "character", U8, "sprite_type", U8),
    OP1_2("delete_floating_sprite_near_character", 0x1F, 0x1D, "character", U8),
    OP2_2("delete_tpt_instance", 0x1F, 0x1E, "tpt_entry", U16, "arg", U8),
    OP2_2("delete_generated_sprite", 0x1F, 0x1F, "sprite", U16, "arg", U8),
    OP2_2("trigger_psi_teleport", 0x1F, 0x20, "type", U8, "destination", U8),
    OP1_2("teleport_to", 0x1F, 0x21, "destination", U8),
    OP1_2("trigger_battle", 0x1F, 0x23, "enemy_group", U16),
    OP0_2("use_normal_font", 0x1F, 0x30),
    OP0_2("use_mr_saturn_font", 0x1F, 0x31),
    OP1_2("trigger_event", 0x1F, 0x41, "event", U8),
    OP0_2("disable_controller_input", 0x1F, 0x50),
    OP0_2("enable_controller_input", 0x1F, 0x51),
    OP1_2("create_number_selector", 0x1F, 0x52, "max_digits", U8),
    OP0_2("text_speed_delay", 0x1F, 0x60),
    OP0_2("trigger_movement_code", 0x1F, 0x61),
    OP1_2("enable_blinking_triangle", 0x1F, 0x62, "enabled", U8),
    OP1_2("screen_reload_ptr", 0x1F, 0x63, "dest", LBL),
    OP0_2("delete_all_npcs", 0x1F, 0x64),
    OP0_2("delete_first_npc", 0x1F, 0x65),
    OP3_2("activate_hotspot", 0x1F, 0x66, "slot", U8, "hotspot", U8, "callback", LBL),
    OP1_2("deactivate_hotspot", 0x1F, 0x67, "slot", U8),
    OP0_2("store_coordinates_to_memory", 0x1F, 0x68),
    OP0_2("teleport_to_stored_coordinates", 0x1F, 0x69),
    OP2_2("realize_psi", 0x1F, 0x71, "character", U8, "psi_id", U8),
    OP2_2("equip_item_to_character", 0x1F, 0x83, "character", U8, "item_slot", U8),
    OP0_2("set_tpt_direction_up", 0x1F, 0xA0),
    OP0_2("set_tpt_direction_down", 0x1F, 0xA1),
    OP0_2("get_interacting_event_flag", 0x1F, 0xA2),
    OP0_2("save_game", 0x1F, 0xB0),
    OP1_2("jump_multi2", 0x1F, 0xC0, "targets", JMPT),
    OP1_2("try_fix_item", 0x1F, 0xD0, "value", U8),
    OP0_2("get_direction_of_nearby_truffle", 0x1F, 0xD1),
    OP1_2("summon_wandering_photographer", 0x1F, 0xD2, "value", U8),
    OP1_2("trigger_timed_event", 0x1F, 0xD3, "event", U8),
    OP2_2("change_map_palette", 0x1F, 0xE1, "palette_id", U16, "fade_speed", U8),
    OP2_2("change_generated_sprite_direction", 0x1F, 0xE4, "sprite_id", U16, "direction", U8),
    OP1_2("set_player_lock", 0x1F, 0xE5, "lock", U8),
    OP1_2("delay_tpt_appearance", 0x1F, 0xE6, "tpt_entry", U16),
    OP1_2("disable_sprite_movement", 0x1F, 0xE7, "sprite_id", U16),
    OP1_2("restrict_player_movement_when_camera_repositioned", 0x1F, 0xE8, "value", U8),
    OP1_2("enable_npc_movement", 0x1F, 0xE9, "tpt_entry", U16),
    OP1_2("enable_sprite_movement", 0x1F, 0xEA, "sprite_id", U16),
    OP2_2("make_invisible", 0x1F, 0xEB, "type", U8, "id", U8),
    OP2_2("make_visible", 0x1F, 0xEC, "type", U8, "id", U8),
    OP0_2("restore_movement", 0x1F, 0xED),
    OP1_2("warp_party_to_tpt_entry", 0x1F, 0xEE, "tpt_entry", U16),
    OP1_2("set_camera_focus_by_sprite_id", 0x1F, 0xEF, "sprite_id", U16),
    OP0_2("ride_bicycle", 0x1F, 0xF0),
    OP2_2("set_tpt_movement_code", 0x1F, 0xF1, "tpt_entry", U16, "movement", U16),
    OP2_2("set_sprite_movement_code", 0x1F, 0xF2, "sprite", U16, "movement", U16),
    OP2_2("create_floating_sprite_near_entity", 0x1F, 0xF3, "entity_id", U16, "sprite_type", U8),
    OP1_2("delete_floating_sprite_near_entity", 0x1F, 0xF4, "entity_id", U16),
};

const int TEXT_OPCODE_COUNT = (int)(sizeof(TEXT_OPCODES) / sizeof(TEXT_OPCODES[0]));

const TextOpcodeSpec *text_opcode_find_by_bytes(const uint8_t *bytes, int byte_count) {
    for (int i = 0; i < TEXT_OPCODE_COUNT; i++) {
        const TextOpcodeSpec *op = &TEXT_OPCODES[i];
        if (op->byte_count != byte_count) continue;
        if (byte_count == 1) {
            if (op->bytes[0] == bytes[0]) return op;
        } else {
            if (op->bytes[0] == bytes[0] && op->bytes[1] == bytes[1]) return op;
        }
    }
    return NULL;
}
