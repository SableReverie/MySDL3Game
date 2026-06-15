/*
// run as sdl3app
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow(
        "Hello SDL3!", 800, 600, SDL_WINDOW_RESIZABLE
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }

        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
*/
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

/* ── Timing ────────────────────────────────────────────────────────────────
   Game logic still advances at 8 Hz (125 ms / step), but we render at the
   display's native refresh rate (120 Hz, 144 Hz, 60 Hz – whatever it is).
   The snake body is drawn with sub-step interpolation so movement looks
   perfectly fluid rather than snapping one cell at a time.               */
#define STEP_RATE_IN_MILLISECONDS  125

/* ── Layout ────────────────────────────────────────────────────────────── */
#define SNAKE_BLOCK_SIZE_IN_PIXELS 24
#define SCORE_BAR_HEIGHT           30          /* pixels reserved for HUD  */
#define SNAKE_GAME_WIDTH           24U
#define SNAKE_GAME_HEIGHT          18U
#define SDL_WINDOW_WIDTH           (SNAKE_BLOCK_SIZE_IN_PIXELS * SNAKE_GAME_WIDTH)
#define SDL_WINDOW_HEIGHT          (SNAKE_BLOCK_SIZE_IN_PIXELS * SNAKE_GAME_HEIGHT + SCORE_BAR_HEIGHT)

/* ── Grid ──────────────────────────────────────────────────────────────── */
#define SNAKE_MATRIX_SIZE          (SNAKE_GAME_WIDTH * SNAKE_GAME_HEIGHT)
#define SNAKE_CELL_MAX_BITS        3U
#define SNAKE_CELL_SET_BITS        (~(~0u << SNAKE_CELL_MAX_BITS))
#define SHIFT(x, y)                (((x) + ((y) * SNAKE_GAME_WIDTH)) * SNAKE_CELL_MAX_BITS)

/* ── Persistence key ───────────────────────────────────────────────────── */
#define HIGH_SCORE_PREF_KEY        "snake_high_score"

static SDL_Joystick *joystick = NULL;

typedef enum {
    SNAKE_CELL_NOTHING = 0U,
    SNAKE_CELL_SRIGHT  = 1U,
    SNAKE_CELL_SUP     = 2U,
    SNAKE_CELL_SLEFT   = 3U,
    SNAKE_CELL_SDOWN   = 4U,
    SNAKE_CELL_FOOD    = 5U
} SnakeCell;

typedef enum {
    SNAKE_DIR_RIGHT,
    SNAKE_DIR_UP,
    SNAKE_DIR_LEFT,
    SNAKE_DIR_DOWN
} SnakeDirection;

typedef struct {
    unsigned char cells[(SNAKE_MATRIX_SIZE * SNAKE_CELL_MAX_BITS) / 8U];
    char  head_xpos;
    char  head_ypos;
    char  tail_xpos;
    char  tail_ypos;
    char  next_dir;
    char  inhibit_tail_step;
    unsigned occupied_cells;
} SnakeContext;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SnakeContext  snake_ctx;

    /* ── Timing ── */
    Uint64 last_step;           /* wall-clock ms of last logic tick        */
    double step_alpha;          /* 0..1 interpolation within current step  */

    /* ── Scoring ── */
    unsigned score;
    unsigned high_score;

    /* ── Font (tiny 5×7 pixel font, rendered manually) ── */
    /* (we use SDL_RenderDebugText which SDL3 provides)    */
} AppState;

/* ═══════════════════════════════════════════════════════════════════════════
   Cell helpers
   ═══════════════════════════════════════════════════════════════════════════ */
static SnakeCell snake_cell_at(const SnakeContext *ctx, char x, char y)
{
    const int shift = SHIFT(x, y);
    unsigned short range;
    SDL_memcpy(&range, ctx->cells + (shift / 8), sizeof(range));
    return (SnakeCell)((range >> (shift % 8)) & SNAKE_CELL_SET_BITS);
}

static void put_cell_at_(SnakeContext *ctx, char x, char y, SnakeCell ct)
{
    const int shift   = SHIFT(x, y);
    const int adjust  = shift % 8;
    unsigned char *const pos = ctx->cells + (shift / 8);
    unsigned short range;
    SDL_memcpy(&range, pos, sizeof(range));
    range &= ~(SNAKE_CELL_SET_BITS << adjust);
    range |=  (ct & SNAKE_CELL_SET_BITS) << adjust;
    SDL_memcpy(pos, &range, sizeof(range));
}

static int are_cells_full_(SnakeContext *ctx)
{
    return ctx->occupied_cells == SNAKE_GAME_WIDTH * SNAKE_GAME_HEIGHT;
}

static void new_food_pos_(SnakeContext *ctx)
{
    while (true) {
        const char x = (char)SDL_rand(SNAKE_GAME_WIDTH);
        const char y = (char)SDL_rand(SNAKE_GAME_HEIGHT);
        if (snake_cell_at(ctx, x, y) == SNAKE_CELL_NOTHING) {
            put_cell_at_(ctx, x, y, SNAKE_CELL_FOOD);
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   High-score persistence  (SDL_GetPrefPath + plain binary file)
   ═══════════════════════════════════════════════════════════════════════════ */
static unsigned load_high_score(void)
{
    char *pref_path = SDL_GetPrefPath("SDLExample", "Snake");
    if (!pref_path) return 0;

    char filepath[512];
    SDL_snprintf(filepath, sizeof(filepath), "%s%s", pref_path, HIGH_SCORE_PREF_KEY);
    SDL_free(pref_path);

    SDL_IOStream *io = SDL_IOFromFile(filepath, "rb");
    if (!io) return 0;

    unsigned hs = 0;
    SDL_ReadIO(io, &hs, sizeof(hs));
    SDL_CloseIO(io);
    return hs;
}

static void save_high_score(unsigned hs)
{
    char *pref_path = SDL_GetPrefPath("SDLExample", "Snake");
    if (!pref_path) return;

    char filepath[512];
    SDL_snprintf(filepath, sizeof(filepath), "%s%s", pref_path, HIGH_SCORE_PREF_KEY);
    SDL_free(pref_path);

    SDL_IOStream *io = SDL_IOFromFile(filepath, "wb");
    if (!io) return;

    SDL_WriteIO(io, &hs, sizeof(hs));
    SDL_CloseIO(io);
}

/* ═══════════════════════════════════════════════════════════════════════════
   Snake logic
   ═══════════════════════════════════════════════════════════════════════════ */
static void snake_initialize(SnakeContext *ctx, AppState *as)
{
    SDL_zeroa(ctx->cells);
    ctx->head_xpos = ctx->tail_xpos = SNAKE_GAME_WIDTH  / 2;
    ctx->head_ypos = ctx->tail_ypos = SNAKE_GAME_HEIGHT / 2;
    ctx->next_dir  = SNAKE_DIR_RIGHT;
    ctx->inhibit_tail_step = ctx->occupied_cells = 4;
    --ctx->occupied_cells;
    put_cell_at_(ctx, ctx->tail_xpos, ctx->tail_ypos, SNAKE_CELL_SRIGHT);
    for (int i = 0; i < 4; i++) {
        new_food_pos_(ctx);
        ++ctx->occupied_cells;
    }
    /* Reset score on new game */
    if (as) {
        if (as->score > as->high_score) {
            as->high_score = as->score;
            save_high_score(as->high_score);
        }
        as->score = 0;
    }
}

static void snake_redir(SnakeContext *ctx, SnakeDirection dir)
{
    SnakeCell ct = snake_cell_at(ctx, ctx->head_xpos, ctx->head_ypos);
    if ((dir == SNAKE_DIR_RIGHT && ct != SNAKE_CELL_SLEFT)  ||
        (dir == SNAKE_DIR_UP    && ct != SNAKE_CELL_SDOWN)  ||
        (dir == SNAKE_DIR_LEFT  && ct != SNAKE_CELL_SRIGHT) ||
        (dir == SNAKE_DIR_DOWN  && ct != SNAKE_CELL_SUP))
    {
        ctx->next_dir = dir;
    }
}

static void wrap_around_(char *val, char max)
{
    if      (*val < 0)       *val = max - 1;
    else if (*val > max - 1) *val = 0;
}

/* Returns 1 if food was eaten this step */
static int snake_step(SnakeContext *ctx, AppState *as)
{
    const SnakeCell dir_as_cell = (SnakeCell)(ctx->next_dir + 1);
    SnakeCell ct;
    char prev_xpos, prev_ypos;
    int ate_food = 0;

    /* Move tail */
    if (--ctx->inhibit_tail_step == 0) {
        ++ctx->inhibit_tail_step;
        ct = snake_cell_at(ctx, ctx->tail_xpos, ctx->tail_ypos);
        put_cell_at_(ctx, ctx->tail_xpos, ctx->tail_ypos, SNAKE_CELL_NOTHING);
        switch (ct) {
        case SNAKE_CELL_SRIGHT: ctx->tail_xpos++; break;
        case SNAKE_CELL_SUP:    ctx->tail_ypos--; break;
        case SNAKE_CELL_SLEFT:  ctx->tail_xpos--; break;
        case SNAKE_CELL_SDOWN:  ctx->tail_ypos++; break;
        default: break;
        }
        wrap_around_(&ctx->tail_xpos, SNAKE_GAME_WIDTH);
        wrap_around_(&ctx->tail_ypos, SNAKE_GAME_HEIGHT);
    }

    /* Move head */
    prev_xpos = ctx->head_xpos;
    prev_ypos = ctx->head_ypos;
    switch (ctx->next_dir) {
    case SNAKE_DIR_RIGHT: ++ctx->head_xpos; break;
    case SNAKE_DIR_UP:    --ctx->head_ypos; break;
    case SNAKE_DIR_LEFT:  --ctx->head_xpos; break;
    case SNAKE_DIR_DOWN:  ++ctx->head_ypos; break;
    default: break;
    }
    wrap_around_(&ctx->head_xpos, SNAKE_GAME_WIDTH);
    wrap_around_(&ctx->head_ypos, SNAKE_GAME_HEIGHT);

    /* Collision */
    ct = snake_cell_at(ctx, ctx->head_xpos, ctx->head_ypos);
    if (ct != SNAKE_CELL_NOTHING && ct != SNAKE_CELL_FOOD) {
        snake_initialize(ctx, as);
        return 0;
    }

    put_cell_at_(ctx, prev_xpos,       prev_ypos,       dir_as_cell);
    put_cell_at_(ctx, ctx->head_xpos,  ctx->head_ypos,  dir_as_cell);

    if (ct == SNAKE_CELL_FOOD) {
        ate_food = 1;
        if (are_cells_full_(ctx)) {
            snake_initialize(ctx, as);
            return 0;
        }
        new_food_pos_(ctx);
        ++ctx->inhibit_tail_step;
        ++ctx->occupied_cells;
        if (as) ++as->score;
    }
    return ate_food;
}

/* ═══════════════════════════════════════════════════════════════════════════
   Rendering helpers
   ═══════════════════════════════════════════════════════════════════════════ */

/* Grid Y offset so cells render below the HUD bar */
#define GRID_Y_OFFSET  SCORE_BAR_HEIGHT

static void fill_cell(SDL_Renderer *r, int gx, int gy)
{
    SDL_FRect rc = {
        (float)(gx * SNAKE_BLOCK_SIZE_IN_PIXELS),
        (float)(gy * SNAKE_BLOCK_SIZE_IN_PIXELS + GRID_Y_OFFSET),
        (float)SNAKE_BLOCK_SIZE_IN_PIXELS,
        (float)SNAKE_BLOCK_SIZE_IN_PIXELS
    };
    SDL_RenderFillRect(r, &rc);
}

/* Draw the score bar at the top */
static void render_hud(AppState *as)
{
    SDL_Renderer *r = as->renderer;

    /* Background strip */
    SDL_SetRenderDrawColor(r, 20, 20, 20, 255);
    SDL_FRect bar = { 0, 0, (float)SDL_WINDOW_WIDTH, (float)SCORE_BAR_HEIGHT };
    SDL_RenderFillRect(r, &bar);

    /* Thin separator line */
    SDL_SetRenderDrawColor(r, 60, 60, 60, 255);
    SDL_RenderLine(r, 0, SCORE_BAR_HEIGHT - 1, SDL_WINDOW_WIDTH, SCORE_BAR_HEIGHT - 1);

    /* Score text – SDL_RenderDebugText is a simple 8×8 built-in font */
    char buf[64];
    SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
    SDL_snprintf(buf, sizeof(buf), "SCORE: %u", as->score);
    SDL_RenderDebugText(r, 8.0f, (SCORE_BAR_HEIGHT - 8) / 2.0f, buf);

    SDL_SetRenderDrawColor(r, 255, 215, 0, 255); /* gold for high score */
    SDL_snprintf(buf, sizeof(buf), "BEST: %u", as->high_score);
    /* Right-align: each char is 8px wide */
    float tx = SDL_WINDOW_WIDTH - (float)(SDL_strlen(buf) * 8) - 8.0f;
    SDL_RenderDebugText(r, tx, (SCORE_BAR_HEIGHT - 8) / 2.0f, buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SDL App callbacks
   ═══════════════════════════════════════════════════════════════════════════ */

SDL_AppResult SDL_AppIterate(void *appstate)
{
    AppState     *as  = (AppState *)appstate;
    SnakeContext *ctx = &as->snake_ctx;
    SDL_Renderer *r   = as->renderer;

    const Uint64 now = SDL_GetTicks();

    /* ── Advance logic steps (catch-up loop) ── */
    while ((now - as->last_step) >= STEP_RATE_IN_MILLISECONDS) {
        snake_step(ctx, as);
        as->last_step += STEP_RATE_IN_MILLISECONDS;
    }

    /* ── Interpolation alpha (0 = start of step, 1 = end of step) ── */
    as->step_alpha = (double)(now - as->last_step) / STEP_RATE_IN_MILLISECONDS;
    /* clamp to [0,1] */
    if (as->step_alpha < 0.0) as->step_alpha = 0.0;
    if (as->step_alpha > 1.0) as->step_alpha = 1.0;

    /* ── Clear ── */
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    /* ── Draw grid cells ── */
    for (unsigned i = 0; i < SNAKE_GAME_WIDTH; i++) {
        for (unsigned j = 0; j < SNAKE_GAME_HEIGHT; j++) {
            int ct = snake_cell_at(ctx, (char)i, (char)j);
            if (ct == SNAKE_CELL_NOTHING) continue;

            if (ct == SNAKE_CELL_FOOD) {
                /* Food: pulsing blue-white glow using alpha */
                double pulse = 0.55 + 0.45 * SDL_sin(now * 0.006);
                SDL_SetRenderDrawColor(r,
                    (Uint8)(80  + 160 * pulse),
                    (Uint8)(80  + 160 * pulse),
                    255, 255);
            } else {
                /* Body */
                SDL_SetRenderDrawColor(r, 30, 160, 30, 255);
            }
            fill_cell(r, (int)i, (int)j);

            /* Inner highlight (makes body look less flat) */
            if (ct != SNAKE_CELL_FOOD) {
                SDL_SetRenderDrawColor(r, 60, 200, 60, 180);
                SDL_FRect hi = {
                    (float)(i * SNAKE_BLOCK_SIZE_IN_PIXELS + 3),
                    (float)(j * SNAKE_BLOCK_SIZE_IN_PIXELS + 3 + GRID_Y_OFFSET),
                    (float)(SNAKE_BLOCK_SIZE_IN_PIXELS - 6),
                    (float)(SNAKE_BLOCK_SIZE_IN_PIXELS - 6)
                };
                SDL_RenderFillRect(r, &hi);
            }
        }
    }

    /* ── Draw head with smooth interpolation ── */
    {
        /* Determine the previous head position (one cell back) */
        float dx = 0, dy = 0;
        switch (ctx->next_dir) {
        case SNAKE_DIR_RIGHT: dx = -1.0f; break;
        case SNAKE_DIR_UP:    dy =  1.0f; break;
        case SNAKE_DIR_LEFT:  dx =  1.0f; break;
        case SNAKE_DIR_DOWN:  dy = -1.0f; break;
        }
        float alpha = (float)as->step_alpha;
        float hx = ctx->head_xpos + dx * (1.0f - alpha);
        float hy = ctx->head_ypos + dy * (1.0f - alpha);

        SDL_SetRenderDrawColor(r, 255, 255, 60, 255);
        SDL_FRect hr = {
            hx * SNAKE_BLOCK_SIZE_IN_PIXELS,
            hy * SNAKE_BLOCK_SIZE_IN_PIXELS + GRID_Y_OFFSET,
            (float)SNAKE_BLOCK_SIZE_IN_PIXELS,
            (float)SNAKE_BLOCK_SIZE_IN_PIXELS
        };
        SDL_RenderFillRect(r, &hr);

        /* Eyes */
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        int ex1x = 0, ex1y = 0, ex2x = 0, ex2y = 0;
        int B = SNAKE_BLOCK_SIZE_IN_PIXELS;
        int px = (int)(hx * B), py = (int)(hy * B) + GRID_Y_OFFSET;
        switch (ctx->next_dir) {
        case SNAKE_DIR_RIGHT:
            ex1x = px+B-6; ex1y = py+4;  ex2x = px+B-6; ex2y = py+B-8; break;
        case SNAKE_DIR_LEFT:
            ex1x = px+4;   ex1y = py+4;  ex2x = px+4;   ex2y = py+B-8; break;
        case SNAKE_DIR_UP:
            ex1x = px+4;   ex1y = py+4;  ex2x = px+B-8; ex2y = py+4;   break;
        case SNAKE_DIR_DOWN:
            ex1x = px+4;   ex1y = py+B-6;ex2x = px+B-8; ex2y = py+B-6; break;
        }
        SDL_FRect eye1 = { (float)ex1x, (float)ex1y, 3, 3 };
        SDL_FRect eye2 = { (float)ex2x, (float)ex2y, 3, 3 };
        SDL_RenderFillRect(r, &eye1);
        SDL_RenderFillRect(r, &eye2);
    }

    /* ── HUD ── */
    render_hud(as);

    SDL_RenderPresent(r);
    return SDL_APP_CONTINUE;
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static SDL_AppResult handle_key_event_(AppState *as, SDL_Scancode key_code)
{
    SnakeContext *ctx = &as->snake_ctx;
    switch (key_code) {
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_Q:
        return SDL_APP_SUCCESS;
    case SDL_SCANCODE_R:
        snake_initialize(ctx, as);
        break;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: snake_redir(ctx, SNAKE_DIR_RIGHT); break;
    case SDL_SCANCODE_UP:    case SDL_SCANCODE_W: snake_redir(ctx, SNAKE_DIR_UP);    break;
    case SDL_SCANCODE_LEFT:  case SDL_SCANCODE_A: snake_redir(ctx, SNAKE_DIR_LEFT);  break;
    case SDL_SCANCODE_DOWN:  case SDL_SCANCODE_S: snake_redir(ctx, SNAKE_DIR_DOWN);  break;
    default: break;
    }
    return SDL_APP_CONTINUE;
}

static SDL_AppResult handle_hat_event_(SnakeContext *ctx, Uint8 hat)
{
    switch (hat) {
    case SDL_HAT_RIGHT: snake_redir(ctx, SNAKE_DIR_RIGHT); break;
    case SDL_HAT_UP:    snake_redir(ctx, SNAKE_DIR_UP);    break;
    case SDL_HAT_LEFT:  snake_redir(ctx, SNAKE_DIR_LEFT);  break;
    case SDL_HAT_DOWN:  snake_redir(ctx, SNAKE_DIR_DOWN);  break;
    default: break;
    }
    return SDL_APP_CONTINUE;
}

/* ── App lifecycle ──────────────────────────────────────────────────────── */

static const struct { const char *key; const char *value; } extended_metadata[] = {
    { SDL_PROP_APP_METADATA_URL_STRING,       "https://examples.libsdl.org/SDL3/demo/01-snake/" },
    { SDL_PROP_APP_METADATA_CREATOR_STRING,   "SDL team" },
    { SDL_PROP_APP_METADATA_COPYRIGHT_STRING, "Placed in the public domain" },
    { SDL_PROP_APP_METADATA_TYPE_STRING,      "game" }
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    if (!SDL_SetAppMetadata("Example Snake game", "1.0", "com.example.Snake"))
        return SDL_APP_FAILURE;

    for (size_t i = 0; i < SDL_arraysize(extended_metadata); i++)
        if (!SDL_SetAppMetadataProperty(extended_metadata[i].key, extended_metadata[i].value))
            return SDL_APP_FAILURE;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    AppState *as = (AppState *)SDL_calloc(1, sizeof(AppState));
    if (!as) return SDL_APP_FAILURE;
    *appstate = as;

    /* Request VSync so we render at display rate without burning CPU */
    if (!SDL_CreateWindowAndRenderer("Snake", SDL_WINDOW_WIDTH, SDL_WINDOW_HEIGHT,
                                     SDL_WINDOW_RESIZABLE, &as->window, &as->renderer))
        return SDL_APP_FAILURE;

    SDL_SetRenderVSync(as->renderer, 1);                 /* adaptive vsync  */
    SDL_SetRenderLogicalPresentation(as->renderer,
        SDL_WINDOW_WIDTH, SDL_WINDOW_HEIGHT,
        SDL_LOGICAL_PRESENTATION_LETTERBOX);

    as->high_score = load_high_score();
    snake_initialize(&as->snake_ctx, as);
    as->score      = 0;                                  /* reset after init */
    as->last_step  = SDL_GetTicks();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    AppState *as = (AppState *)appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_JOYSTICK_ADDED:
        if (!joystick) {
            joystick = SDL_OpenJoystick(event->jdevice.which);
            if (!joystick)
                SDL_Log("Failed to open joystick ID %u: %s",
                        (unsigned)event->jdevice.which, SDL_GetError());
        }
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        if (joystick && SDL_GetJoystickID(joystick) == event->jdevice.which) {
            SDL_CloseJoystick(joystick);
            joystick = NULL;
        }
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        return handle_hat_event_(&as->snake_ctx, event->jhat.value);
    case SDL_EVENT_KEY_DOWN:
        return handle_key_event_(as, event->key.scancode);
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (joystick) SDL_CloseJoystick(joystick);
    if (appstate) {
        AppState *as = (AppState *)appstate;
        /* Save high score on exit */
        if (as->score > as->high_score) save_high_score(as->score);
        SDL_DestroyRenderer(as->renderer);
        SDL_DestroyWindow(as->window);
        SDL_free(as);
    }
}