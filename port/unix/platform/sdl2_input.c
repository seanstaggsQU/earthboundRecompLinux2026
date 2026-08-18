#include "platform/platform.h"
#include "include/pad.h"
#include "game/settings.h"
#include <SDL.h>
#include <signal.h>
#include <string.h>

/* Pad/aux state is assembled each poll from independent per-source trackers
 * (keyboard, controller buttons, controller left-stick-as-dpad) and OR'd
 * together, rather than having each source mutate one shared bitmask
 * directly -- that would risk one source's release event clobbering a bit
 * another source is still holding (e.g. releasing the stick back to center
 * while a d-pad button is physically held). */
static uint16_t kb_pad_state;
static uint16_t kb_aux_state;
static uint16_t ctrl_pad_state;  /* controller face/shoulder/start/select/d-pad buttons */
static uint16_t ctrl_stick_bits; /* left stick, converted to digital PAD_DPAD bits */
static uint16_t ctrl_aux_state;  /* controller-driven AUX_* bits (e.g. R3 = quick save) */

/* Per-SDL-button held state, rebuilt into ctrl_pad_state each poll (see
 * platform_input_poll). Needed because controller_button_to_pad() is a
 * many-to-one mapping (e.g. both South and L-shoulder send PAD_L) -- an
 * incrementally OR'd/cleared single accumulator would let releasing one
 * button clobber a target bit another still-held button owns too. */
static bool ctrl_button_held[SDL_CONTROLLER_BUTTON_MAX];

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static volatile sig_atomic_t quit_requested;

/* Single active controller (first one connected). One is plenty for a
 * single-player game; additional controllers are ignored until this one
 * disconnects. */
static SDL_GameController *controller;
static SDL_JoystickID controller_id = -1;

/* Left stick deflection (of 32767) beyond which a direction counts as held.
 * ~25% -- clears dead zone/drift without requiring a hard stick push. */
#define STICK_THRESHOLD 8000

/* Global mode flags */
bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;
bool platform_force_windowed = false;

static void signal_handler(int sig) {
    (void)sig;
    quit_requested = 1;
}

bool platform_input_init(void) {
    kb_pad_state = 0;
    kb_aux_state = 0;
    ctrl_pad_state = 0;
    ctrl_stick_bits = 0;
    ctrl_aux_state = 0;
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;
    quit_requested = 0;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    return true;
}

void platform_input_shutdown(void) {
    if (controller) {
        SDL_GameControllerClose(controller);
        controller = NULL;
    }
}

/* Controller button -> SNES pad bit. Deliberately NOT positional-by-XInput-
 * layout (the previous scheme) -- remapped so the most-used overworld
 * actions sit on the buttons a modern player reaches for first:
 *
 *   South (A/Cross)  -> PAD_L   "check/talk to" shortcut (PAD_CONFIRM group,
 *                                so it also confirms menus/text, same as A)
 *   East  (B/Circle) -> PAD_B   cancel / HP-PP display / auto-fight-cancel
 *                                (direct PAD_B checks exist in battle.c, so
 *                                this keeps them reachable, not just the
 *                                PAD_CANCEL group via Select)
 *   North (Y/Triangle) -> PAD_X town map (unique function, no equivalent)
 *   West  (X/Square)   -> PAD_Y no function in normal gameplay (unchanged)
 *   Start             -> PAD_START | PAD_A  opens the command menu (PAD_A)
 *                                and keeps every existing PAD_START use
 *                                (naming confirm, debug SELECT+START, etc.)
 *   Back/View         -> PAD_SELECT  already part of PAD_CANCEL (B|SELECT),
 *                                so this alone opens the HP/PP "status check"
 *                                display without needing a raw PAD_B send
 *   L shoulder        -> PAD_L  kept redundant with South per platform.h's
 *                                own guidance to prefer extra PAD_L bindings
 *   R shoulder        -> PAD_R  bicycle bell (unchanged, cosmetic)
 */
static uint16_t controller_button_to_pad(SDL_GameControllerButton button) {
    /* "Alt Controls" (Config menu, engine_alt_controls -- settings.h) swaps
     * the A<->B and X<->Y *mapping outputs* below. SDL names face buttons
     * by Xbox-style position (A=south, B=east, X=west, Y=north) regardless
     * of what's printed on the button; on a Nintendo-style pad (Switch Pro,
     * SNES, GameCube-adapter, ...) those labels sit at different positions
     * (physical "A" at east, "B" at south, X/Y likewise swapped), so
     * without this a Nintendo player pressing what they see as "A"
     * (expecting the PAD_CONFIRM-group function below) gets B's cancel
     * function instead. Swapping which case each SDL button falls into
     * fixes that for either physical layout, without needing to know which
     * kind of pad is actually connected. */
    bool alt = engine_alt_controls == ALT_CONTROLS_ON;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:            return alt ? PAD_B : PAD_L;
    case SDL_CONTROLLER_BUTTON_B:            return alt ? PAD_L : PAD_B;
    case SDL_CONTROLLER_BUTTON_Y:            return alt ? PAD_Y : PAD_X;
    case SDL_CONTROLLER_BUTTON_X:            return alt ? PAD_X : PAD_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return PAD_L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PAD_R;
    case SDL_CONTROLLER_BUTTON_START:        return PAD_START | PAD_A;
    case SDL_CONTROLLER_BUTTON_BACK:         return PAD_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:      return PAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:    return PAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:    return PAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:   return PAD_RIGHT;
    default: return 0;
    }
}

/* Controller button -> AUX bit. R3 (right stick click) is otherwise unused
 * by this game (no camera control), so it's the overworld FOV/zoom cycle
 * for controller players (game_main.c only acts on it outside of battle, so
 * a press during battle is a harmless no-op rather than queued input).
 * No keyboard equivalent -- R3 only, per the feature request. */
static uint16_t controller_button_to_aux(SDL_GameControllerButton button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return AUX_ZOOM_TOGGLE;
    default: return 0;
    }
}

static void handle_controller_added(int joystick_index) {
    if (controller != NULL) return; /* already have one; ignore the rest */
    if (!SDL_IsGameController(joystick_index)) return;
    controller = SDL_GameControllerOpen(joystick_index);
    if (controller) {
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
        controller_id = SDL_JoystickInstanceID(joy);
    }
}

static void handle_controller_removed(SDL_JoystickID which) {
    if (which != controller_id) return;
    SDL_GameControllerClose(controller);
    controller = NULL;
    controller_id = -1;
    /* Don't leave stuck buttons/direction behind on disconnect. */
    ctrl_pad_state = 0;
    ctrl_stick_bits = 0;
    ctrl_aux_state = 0;
    memset(ctrl_button_held, 0, sizeof(ctrl_button_held));
}

static void handle_axis_motion(const SDL_ControllerAxisEvent *axis) {
    if (axis->which != controller_id) return;
    if (axis->axis == SDL_CONTROLLER_AXIS_LEFTX) {
        ctrl_stick_bits &= ~(PAD_LEFT | PAD_RIGHT);
        if (axis->value < -STICK_THRESHOLD) ctrl_stick_bits |= PAD_LEFT;
        else if (axis->value > STICK_THRESHOLD) ctrl_stick_bits |= PAD_RIGHT;
    } else if (axis->axis == SDL_CONTROLLER_AXIS_LEFTY) {
        ctrl_stick_bits &= ~(PAD_UP | PAD_DOWN);
        if (axis->value < -STICK_THRESHOLD) ctrl_stick_bits |= PAD_UP;
        else if (axis->value > STICK_THRESHOLD) ctrl_stick_bits |= PAD_DOWN;
    }
}

void platform_input_poll(void) {
    SDL_Event event;

    pad_prev = pad_state;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            quit_requested = 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            uint16_t bit = 0;
            uint16_t aux_bit = 0;
            switch (event.key.keysym.scancode) {
            case SDL_SCANCODE_X:      bit = PAD_A;      break;
            case SDL_SCANCODE_Z:      bit = PAD_B;      break;
            case SDL_SCANCODE_S:      bit = PAD_X;      break;
            case SDL_SCANCODE_A:      bit = PAD_Y;      break;
            case SDL_SCANCODE_Q:      bit = PAD_L;      break;
            case SDL_SCANCODE_W:      bit = PAD_R;      break;
            case SDL_SCANCODE_RETURN: bit = PAD_START;  break;
            case SDL_SCANCODE_RSHIFT: bit = PAD_SELECT; break;
            case SDL_SCANCODE_UP:     bit = PAD_UP;            break;
            case SDL_SCANCODE_DOWN:   bit = PAD_DOWN;          break;
            case SDL_SCANCODE_LEFT:   bit = PAD_LEFT;          break;
            case SDL_SCANCODE_RIGHT:  bit = PAD_RIGHT;         break;
            case SDL_SCANCODE_F1:     aux_bit = AUX_DEBUG_DUMP;   break;
            case SDL_SCANCODE_F2:     aux_bit = AUX_VRAM_DUMP;    break;
            case SDL_SCANCODE_F3:     aux_bit = AUX_FPS_TOGGLE;   break;
            case SDL_SCANCODE_F6:     aux_bit = AUX_SAVESTATE;    break;
            case SDL_SCANCODE_F7:     aux_bit = AUX_LOAD_STATE;   break;
            case SDL_SCANCODE_TAB:    aux_bit = AUX_FAST_FORWARD; break;
            case SDL_SCANCODE_F5:     aux_bit = AUX_DEBUG_TOGGLE; break;
            default: break;
            }
            if (event.type == SDL_KEYDOWN) {
                kb_pad_state |= bit;
                kb_aux_state |= aux_bit;
            } else {
                kb_pad_state &= ~bit;
                kb_aux_state &= ~aux_bit;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            handle_controller_added(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            handle_controller_removed(event.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            if (event.cbutton.which != controller_id) break;
            SDL_GameControllerButton cbtn = (SDL_GameControllerButton)event.cbutton.button;
            uint16_t aux_bit = controller_button_to_aux(cbtn);
            bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
            if (cbtn >= 0 && cbtn < SDL_CONTROLLER_BUTTON_MAX)
                ctrl_button_held[cbtn] = down;
            if (down) {
                ctrl_aux_state |= aux_bit;
            } else {
                ctrl_aux_state &= ~aux_bit;
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
            handle_axis_motion(&event.caxis);
            break;
        }
    }

    /* Rebuild from per-button held state each poll (see ctrl_button_held's
     * comment) rather than trusting an incrementally mutated accumulator. */
    ctrl_pad_state = 0;
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        if (ctrl_button_held[i])
            ctrl_pad_state |= controller_button_to_pad((SDL_GameControllerButton)i);
    }

    pad_state = kb_pad_state | ctrl_pad_state | ctrl_stick_bits;
    aux_state = kb_aux_state | ctrl_aux_state;
}

uint16_t platform_input_get_pad(void) {
    return pad_state;
}

uint16_t platform_input_get_pad_new(void) {
    return pad_state & ~pad_prev;
}

uint16_t platform_input_get_aux(void) {
    return aux_state;
}

bool platform_input_quit_requested(void) {
    return quit_requested != 0;
}

void platform_request_quit(void) {
    quit_requested = 1;
}
