/*
 * GAME_MODE_UPDATE_CHECK — this port's own addition, not a port of any ROM
 * routine (there is no update screen in the original game). Reached from
 * file-select's "Check for Updates" row (fm_file_select_build(),
 * file_select.c), which is itself only shown when platform_update_supported()
 * is true.
 *
 * Same build/dispatch/rebuild loop shape as mode_step_settings_menu()
 * (text.c), but the actual check/download/verify/install work happens off
 * the main thread in the platform backend (platform_update_*, always
 * non-blocking — see platform.h) since the mode stack steps one
 * non-blocking frame at a time and a real HTTP call here would freeze
 * rendering. This file just starts that work and polls it once a frame.
 */
#include "core/mode_stack.h"
#include "platform/platform.h"
#include "game/window.h"
#include "game/text.h"
#include "game/audio.h"
#include "core/memory.h"
#include "include/pad.h"
#include <stdio.h>

/* Shared with fm_push_selection() (file_select.c) / menu_push_selection()
 * (text.c) in spirit, not in code — this file gets its own private copy
 * since both of those are static to their own translation units. */
static ModeState upd_child_init;

static StepResult upd_push_selection(UpdateCheckState *st, uint8_t result_phase,
                                     uint16_t allow_cancel) {
    st->phase = result_phase;
    WindowInfo *w = get_window(win.current_focus_window);
    if (!w || w->menu_count == 0) {
        st->result_ready = 1;
        st->result = 0;
        return STEP_RESULT_CONTINUE();
    }
    st->result_ready = 0;
    upd_child_init = (ModeState){0};
    upd_child_init.selection_menu.phase        = SM_SETUP;
    upd_child_init.selection_menu.allow_cancel = (uint8_t)allow_cancel;
    return STEP_RESULT_PUSH_INIT(GAME_MODE_SELECTION_MENU, &upd_child_init);
}

static uint16_t upd_take_result(UpdateCheckState *st) {
    uint16_t r = st->result_ready ? st->result : (uint16_t)mode_child_result();
    st->result_ready = 0;
    return r;
}

StepResult mode_step_update_check(ModeState *ms) {
    UpdateCheckState *st = &ms->update_check;

    switch ((UpdateCheckPhase)st->phase) {
    case UPD_CHECK_START: {
        fprintf(stderr, "[updater] UPD_CHECK_START entered\n");
        WindowInfo *w = create_window(WINDOW_UPDATE_CHECK);
        fprintf(stderr, "[updater] create_window(WINDOW_UPDATE_CHECK) = %p, focus=%d\n",
                (void *)w, win.current_focus_window);
        set_focus_text_cursor(0, 0);
        print_string("Checking for updates...");
        platform_update_check_start();
        fprintf(stderr, "[updater] platform_update_check_start() returned, supported=%d\n",
                platform_update_supported());
        st->phase = UPD_CHECKING;
        return STEP_RESULT_CONTINUE();
    }

    case UPD_CHECKING: {
        platform_update_poll(&st->progress);
        static int poll_log_count = 0;
        if (poll_log_count < 20) {
            fprintf(stderr, "[updater] UPD_CHECKING poll #%d status=%d\n", poll_log_count, st->progress.status);
            poll_log_count++;
        }
        if (st->progress.status == EB_UPDATE_CHECKING)
            return STEP_RESULT_CONTINUE(); /* still waiting on the background thread */
        st->phase = UPD_RESULT;
        return STEP_RESULT_CONTINUE();
    }

    case UPD_RESULT: {
        fprintf(stderr, "[updater] UPD_RESULT entered, status=%d, latest_version=\"%s\", focus_before_close=%d\n",
                st->progress.status, st->progress.latest_version, win.current_focus_window);
        if (st->progress.status == EB_UPDATE_AVAILABLE) {
            /* WINDOW_UPDATE_CHECK is height 10 -- sm_handle_input()'s
             * cursor-search bound is (height-2)/2 rows, so only text_y
             * 0-3 are valid (same formula that caught the file-select
             * OOB bug and the Command Menu's earlier height bumps). Every
             * row below is laid out within that range. */
            close_focus_window();
            create_window(WINDOW_UPDATE_CHECK);
            set_focus_text_cursor(0, 0);
            print_string("Update available:");
            set_focus_text_cursor(0, 1);
            print_string(st->progress.latest_version);
            add_menu_item("Yes", 1, 0, 3);
            add_menu_item("No", 0, 6, 3);
            print_menu_items();
            play_sfx(27); /* SFX::MENU_OPEN_CLOSE */
            return upd_push_selection(st, UPD_CONFIRM_RESULT, 1);
        }

        /* Every other outcome (up to date / unsupported / error) is a
         * one-shot message, not a confirm — print it once here, then just
         * wait for a button in UPD_MESSAGE. */
        close_focus_window();
        fprintf(stderr, "[updater] after close_focus_window, focus=%d\n", win.current_focus_window);
        WindowInfo *w2 = create_window(WINDOW_UPDATE_CHECK);
        fprintf(stderr, "[updater] create_window (msg branch) = %p, focus=%d\n", (void *)w2, win.current_focus_window);
        set_focus_text_cursor(0, 0);
        switch (st->progress.status) {
        case EB_UPDATE_UP_TO_DATE: {
            fprintf(stderr, "[updater] printing up-to-date message\n");
            print_string("You're on the latest");
            set_focus_text_cursor(0, 1);
            char buf[64];
            snprintf(buf, sizeof(buf), "version: %s", st->progress.latest_version);
            fprintf(stderr, "[updater] printing: \"%s\"\n", buf);
            print_string(buf);
            fprintf(stderr, "[updater] print_string calls done\n");
            break;
        }
        case EB_UPDATE_UNSUPPORTED:
            print_string("Updates aren't available");
            set_focus_text_cursor(0, 1);
            print_string("in this build.");
            break;
        case EB_UPDATE_ERROR:
        default:
            print_string("Couldn't check for updates:");
            set_focus_text_cursor(0, 1);
            print_string(st->progress.error_message);
            break;
        }
        set_focus_text_cursor(0, 3);
        print_string("(Press a button)");
        st->phase = UPD_MESSAGE;
        return STEP_RESULT_CONTINUE();
    }

    case UPD_CONFIRM_RESULT: {
        uint16_t selection = upd_take_result(st);
        if (selection == 1) {
            st->phase = UPD_DOWNLOAD_START;
            return STEP_RESULT_CONTINUE();
        }
        /* No / cancel -> close. */
        st->phase = UPD_CLEANUP;
        return STEP_RESULT_CONTINUE();
    }

    case UPD_DOWNLOAD_START: {
        close_focus_window();
        create_window(WINDOW_UPDATE_CHECK);
        set_focus_text_cursor(0, 0);
        print_string("Downloading update...");
        platform_update_download_start();
        st->phase = UPD_DOWNLOADING;
        return STEP_RESULT_CONTINUE();
    }

    case UPD_DOWNLOADING: {
        platform_update_poll(&st->progress);
        if (st->progress.status == EB_UPDATE_DOWNLOADING) {
            /* Redraw just the percentage line each frame -- the "Downloading
             * update..." label above it never changes, so it's left alone. */
            set_focus_text_cursor(0, 2);
            char pct[16];
            int n = st->progress.progress_percent;
            if (n < 0) n = 0;
            if (n > 100) n = 100;
            snprintf(pct, sizeof(pct), "%d%%", n);
            print_string(pct);
            return STEP_RESULT_CONTINUE();
        }
        if (st->progress.status == EB_UPDATE_DONE) {
            /* A relaunch has already been staged by the backend and quit
             * already requested -- the frame loop is about to unwind. Fall
             * through to cleanup in case a frame or two still renders
             * before that happens. */
            st->phase = UPD_CLEANUP;
            return STEP_RESULT_CONTINUE();
        }
        /* EB_UPDATE_ERROR (download/verify/install failed) -- the running
         * binary was never touched; show why and let the player retry
         * later from file-select. */
        close_focus_window();
        create_window(WINDOW_UPDATE_CHECK);
        set_focus_text_cursor(0, 0);
        print_string("Update failed:");
        set_focus_text_cursor(0, 1);
        print_string(st->progress.error_message);
        set_focus_text_cursor(0, 3);
        print_string("(Press a button)");
        st->phase = UPD_MESSAGE;
        return STEP_RESULT_CONTINUE();
    }

    case UPD_MESSAGE: {
        if (core.pad1_pressed & (PAD_CONFIRM | PAD_CANCEL)) {
            play_sfx(2); /* SFX::CURSOR2 */
            st->phase = UPD_CLEANUP;
        }
        return STEP_RESULT_CONTINUE();
    }

    case UPD_CLEANUP:
        close_focus_window();
        st->phase = UPD_DONE;
        return STEP_RESULT_PUSH(GAME_MODE_ENTITY_FADE_WAIT);

    case UPD_DONE:
    default:
        return STEP_RESULT_POP(0);
    }
}
