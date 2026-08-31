/* host/wgpu/mod.rs + policy.rs + host/lk201/winit.rs -> SDL2 window loop,
 * frame pacing and LK201 key mapping. */
#include "host/host.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "host/gif.h"

void frame_policy_init(frame_policy *p)
{
    uint64_t now = monotonic_ns();
    p->update_step_ns = POLICY_UPDATE_STEP_NS;
    p->max_render_interval_ns = POLICY_RENDER_STEP_NS;
    p->next_update_ns = now + p->update_step_ns;
    /* first render immediately due; signed-safe near clock start */
    p->last_present_ns = now >= p->max_render_interval_ns ? now - p->max_render_interval_ns : 0;
    p->dirty = true;
    p->will_redraw = false;
    p->updates_to_run = 0;
}

void frame_policy_plan_tick(frame_policy *p)
{
    uint64_t now = monotonic_ns();
    uint32_t updates = 0;

    while (now >= p->next_update_ns) {
        updates++;
        p->next_update_ns += p->update_step_ns;
        if (updates >= POLICY_MAX_CATCHUP) {
            p->next_update_ns = now + p->update_step_ns;
            break;
        }
    }
    if (updates > 0)
        p->dirty = true;
    p->updates_to_run = updates;
}

idle_plan frame_policy_plan_idle(frame_policy *p)
{
    uint64_t now = monotonic_ns();
    bool render_due = now - p->last_present_ns >= p->max_render_interval_ns;
    idle_plan plan = { p->next_update_ns, p->dirty && render_due };
    return plan;
}

void frame_policy_on_presented(frame_policy *p)
{
    p->last_present_ns = monotonic_ns();
    p->dirty = false;
    p->will_redraw = false;
}

void frame_policy_on_present_failed_retry(frame_policy *p)
{
    p->dirty = true;
}

void frame_policy_on_request_redraw(frame_policy *p)
{
    p->will_redraw = true;
}

/* == lk201/winit.rs `send` helper: modifier dispatch for special keys. */
static void send_special(lk201_sender s, lk201_special_key key, bool ctrl, bool shift)
{
    if (ctrl) {
        if (shift)
            lk201_send_shift_ctrl_special_key(s, key);
        else
            lk201_send_ctrl_special_key(s, key);
    } else if (shift) {
        lk201_send_shift_special_key(s, key);
    } else {
        lk201_send_special_key(s, key);
    }
}

static bool keydown_special(SDL_Keycode sym, lk201_special_key *out)
{
    switch (sym) {
    case SDLK_F1:  *out = LK201_SK_F1;  return true;
    case SDLK_F2:  *out = LK201_SK_F2;  return true;
    case SDLK_F3:  *out = LK201_SK_F3;  return true;
    case SDLK_F4:  *out = LK201_SK_F4;  return true;
    case SDLK_F5:  *out = LK201_SK_F5;  return true;
    case SDLK_F6:  *out = LK201_SK_F6;  return true;
    case SDLK_F7:  *out = LK201_SK_F7;  return true;
    case SDLK_F8:  *out = LK201_SK_F8;  return true;
    case SDLK_F9:  *out = LK201_SK_F9;  return true;
    case SDLK_F10: *out = LK201_SK_F10; return true;
    case SDLK_F11: *out = LK201_SK_F11; return true;
    case SDLK_F12: *out = LK201_SK_F12; return true;
    case SDLK_F13: *out = LK201_SK_F13; return true;
    case SDLK_F14: *out = LK201_SK_F14; return true;
    case SDLK_F15: *out = LK201_SK_HELP; return true;
    case SDLK_F16: *out = LK201_SK_MENU; return true;
    case SDLK_F17: *out = LK201_SK_F17; return true;
    case SDLK_F18: *out = LK201_SK_F18; return true;
    case SDLK_F19: *out = LK201_SK_F19; return true;
    case SDLK_F20: *out = LK201_SK_F20; return true;
    case SDLK_UP:    *out = LK201_SK_UP;    return true;
    case SDLK_DOWN:  *out = LK201_SK_DOWN;  return true;
    case SDLK_LEFT:  *out = LK201_SK_LEFT;  return true;
    case SDLK_RIGHT: *out = LK201_SK_RIGHT; return true;
    case SDLK_RETURN:    *out = LK201_SK_RETURN; return true;
    case SDLK_KP_ENTER:  *out = LK201_SK_RETURN; return true; /* winit NamedKey::Enter parity */
    case SDLK_BACKSPACE: *out = LK201_SK_DELETE; return true;
    case SDLK_TAB:       *out = LK201_SK_TAB;    return true;
    case SDLK_HOME:     *out = LK201_SK_FIND;        return true;
    case SDLK_END:      *out = LK201_SK_SELECT;      return true;
    case SDLK_INSERT:   *out = LK201_SK_INSERT_HERE; return true;
    case SDLK_DELETE:   *out = LK201_SK_REMOVE;      return true;
    case SDLK_PAGEUP:   *out = LK201_SK_PREV_SCREEN; return true;
    case SDLK_PAGEDOWN: *out = LK201_SK_NEXT_SCREEN; return true;
    case SDLK_NUMLOCKCLEAR: *out = LK201_SK_KP_PF1;  return true;
    default: return false; /* numpad keys intentionally unmapped (Rust parity) */
    }
}

/* SDL_KEYDOWN (repeat included): specials, Escape, and ctrl+char synthesis
 * (SDL_TEXTINPUT does not fire while ctrl is held). */
static void handle_keydown(const SDL_KeyboardEvent *ke, lk201_sender sender)
{
    bool ctrl = (ke->keysym.mod & KMOD_CTRL) != 0;
    bool shift = (ke->keysym.mod & KMOD_SHIFT) != 0;
    SDL_Keycode sym = ke->keysym.sym;

    if (sym == SDLK_ESCAPE) { /* raw, no modifier handling (Rust parity) */
        lk201_send_escape(sender);
        return;
    }
    lk201_special_key key;
    if (keydown_special(sym, &key)) {
        send_special(sender, key, ctrl, shift);
        return;
    }
    if (!ctrl || sym < 0x20 || sym > 0x7E)
        return; /* text keys arrive via SDL_TEXTINPUT */
    char c = (char)sym;
    if (shift && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    lk201_send_ctrl_char(sender, c);
}

static void handle_textinput(const SDL_TextInputEvent *te, lk201_sender sender)
{
    if (SDL_GetModState() & KMOD_CTRL)
        return; /* never double-send: ctrl chars come from SDL_KEYDOWN */
    for (const char *p = te->text; *p; p++) {
        if ((unsigned char)*p < 0x80)
            lk201_send_char(sender, *p);
    }
}

size_t screen_graphics_run(vt420_system *sys, i8051_cpu *cpu,
                           const char *record_path)
{
    LOG_INFOF("Graphics: starting");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_ERRORF("Graphics error: SDL_Init: %s", SDL_GetError());
        return (size_t)-1;
    }
    SDL_EnableScreenSaver(); /* SDL_Init disables it; let the host display sleep */
    SDL_Window *win = SDL_CreateWindow(FB_WINDOW_TITLE,
                                       SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       FB_WIDTH, FB_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!win) {
        LOG_ERRORF("Failed to create window: %s", SDL_GetError());
        SDL_Quit();
        return (size_t)-1;
    }
    SDL_SetWindowMinimumSize(win, FB_WIDTH, FB_HEIGHT);

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               FB_WIDTH, FB_HEIGHT)
                           : NULL;
    uint8_t *frame = calloc(1, FB_FRAME_BYTES);
    if (!ren || !tex || !frame) {
        LOG_ERRORF("Graphics error: %s", SDL_GetError());
        free(frame);
        if (tex)
            SDL_DestroyTexture(tex);
        if (ren)
            SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return (size_t)-1;
    }
    LOG_INFOF("Graphics: window created");
    SDL_StartTextInput();

    gif_writer *rec = record_path ? gif_open(record_path, FB_WIDTH, FB_HEIGHT) : NULL;
    uint64_t rec_last_ns = monotonic_ns(), rec_next_ns = rec_last_ns;
    if (record_path && !rec)
        LOG_ERRORF("Recording: cannot create \"%s\"", record_path);
    else if (rec)
        LOG_INFOF("Recording to \"%s\"", record_path);

    lk201_sender sender = lk201_get_sender(&sys->keyboard);
    frame_policy policy;
    frame_policy_init(&policy);

    /* An idle terminal rasterizes to the same pixels for minutes on end (the
     * firmware keeps rewriting row descriptors regardless), so compare the
     * raster, not its inputs, and skip the upload and the present. */
    /* shown = last presented raster, alt = the distinct one before it. A
     * still screen repeats shown; a blinking cursor on a still screen just
     * alternates the two. Either way nothing is happening. A third image is
     * real activity. */
    uint8_t *shown = calloc(1, FB_FRAME_BYTES);
    uint8_t *alt = calloc(1, FB_FRAME_BYTES);
    bool have_shown = false, have_alt = false;
    uint32_t still = 0;     /* consecutive renders with no new content */
    uint32_t idle_tick = 0; /* tick counter for the FB_IDLE_EVERY divider */
    bool stepped = false;   /* machine advanced since the last render */

    bool quit = false;
    while (!quit) {
        frame_policy_plan_tick(&policy);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                quit = true;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                    quit = true;
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
                           ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                           ev.window.event == SDL_WINDOWEVENT_SHOWN ||
                           ev.window.event == SDL_WINDOWEVENT_RESTORED) {
                    have_shown = false; /* the window lost its contents */
                    frame_policy_on_request_redraw(&policy);
                }
                break;
            case SDL_TEXTINPUT:
                handle_textinput(&ev.text, sender);
                still = 0; /* a keystroke needs the machine at full speed */
                break;
            case SDL_KEYDOWN:
                handle_keydown(&ev.key, sender);
                still = 0;
                break;
            }
        }
        if (quit)
            break;

        uint32_t updates = policy.updates_to_run;
        if (still >= FB_IDLE_AFTER && ++idle_tick % FB_IDLE_EVERY != 0)
            updates = 0; /* idle: one full update per FB_IDLE_EVERY ticks */
        for (uint32_t i = 0; i < updates; i++)
            fb_stepper_update(sys, cpu);
        stepped |= updates > 0;

        /* No steps since the last render means VRAM cannot have moved, so
         * neither can the raster: skip it rather than redraw the same pixels.
         * Rasterizing is what dominates once the machine is throttled. */
        if (policy.will_redraw && !stepped && have_shown) {
            frame_policy_on_presented(&policy);
        } else if (policy.will_redraw) {
            stepped = false;
            fb_render_frame(sys, frame);
            bool same_shown = have_shown && memcmp(frame, shown, FB_FRAME_BYTES) == 0;
            bool same_alt = have_alt && memcmp(frame, alt, FB_FRAME_BYTES) == 0;

            if (!same_shown && !same_alt)
                still = 0;
            else if (still < FB_IDLE_AFTER)
                still++;

            if (same_shown) {
                frame_policy_on_presented(&policy); /* nothing to upload */
            } else if (SDL_UpdateTexture(tex, NULL, frame, FB_STRIDE) != 0 ||
                       SDL_RenderCopy(ren, tex, NULL, NULL) != 0) {
                LOG_ERRORF("Graphics: render failed: %s", SDL_GetError());
                frame_policy_on_present_failed_retry(&policy);
            } else {
                SDL_RenderPresent(ren); /* the cursor must still visibly blink */
                if (shown && alt) {
                    memcpy(alt, shown, FB_FRAME_BYTES);
                    have_alt = have_shown;
                    memcpy(shown, frame, FB_FRAME_BYTES);
                    have_shown = true;
                }
                frame_policy_on_presented(&policy);
            }
        }

        if (rec && monotonic_ns() >= rec_next_ns) {
            uint64_t now = monotonic_ns();
            uint64_t cs = (now - rec_last_ns) / REC_CS_NS;
            if (cs < 2)
                cs = 2; /* viewers clamp shorter delays to 10 cs */
            else if (cs > 0xFFFF)
                cs = 0xFFFF;
            fb_render_frame(sys, frame);
            gif_add_frame(rec, frame, (uint16_t)cs);
            rec_last_ns = now;
            rec_next_ns = now + REC_STEP_NS;
        }

        idle_plan plan = frame_policy_plan_idle(&policy);
        if (plan.request_redraw)
            frame_policy_on_request_redraw(&policy); /* collapsed RedrawRequested round trip */

        uint64_t now = monotonic_ns();
        if (plan.wait_until_ns > now) {
            uint32_t ms = (uint32_t)((plan.wait_until_ns - now) / 1000000ull);
            if (ms > 0)
                SDL_WaitEventTimeout(NULL, (int)ms); /* event stays queued for next pass */
        }
    }

    if (rec) {
        uint32_t nframes = 0;
        long n = gif_close(rec, &nframes);
        if (n < 0)
            LOG_ERRORF("Recording: writing \"%s\" failed", record_path);
        else
            LOG_INFOF("Recording: %u frames, %ld bytes -> %s", nframes, n, record_path);
    }

    size_t count = sys->instruction_count;
    free(alt);
    free(shown);
    free(frame);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return count;
}
