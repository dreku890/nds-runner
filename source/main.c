/*
 * Steel Vanguard - an original side-scrolling run-and-gun for Nintendo DS
 * Built with libnds (devkitPro). All art is generated procedurally at
 * startup - no external assets required.
 *
 * Controls:
 *   D-Pad      move / aim up / crouch
 *   A or Y     shoot
 *   B          jump
 *   R          throw grenade
 *   START      pause / restart after game over
 */

#include <nds.h>
#include <nds/arm9/sound.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SCREEN_W        256
#define SCREEN_H        192
#define GROUND_Y        160     /* top of the ground strip            */
#define LEVEL_LEN       3072    /* level length in pixels             */
#define GRAVITY         28      /* fixed point 8.8                    */
#define JUMP_VEL        (-6 * 256)
#define MOVE_SPEED      (2 * 256 + 64)
#define MAX_BULLETS     16
#define MAX_EBULLETS    16
#define MAX_GRENADES    4
#define MAX_ENEMIES     12
#define MAX_FX          8
#define FIRE_COOLDOWN   9
#define GRENADE_AMMO    10
#define PLAYER_LIVES    3
#define INVULN_FRAMES   90
#define BOSS_X          (LEVEL_LEN - 220)
#define BOSS_HP         60
#define NUM_STAGES      3

/* Weapon system */
#define HMG_AMMO        80
#define SPREAD_AMMO     40
#define ROCKET_AMMO     15
#define HMG_COOLDOWN    3
#define SPREAD_COOLDOWN 14
#define ROCKET_COOLDOWN 30

/* Pickup system */
#define MAX_PICKUPS     10

/* ------------------------------------------------------------------ */
/* Sound system – synthesised PCM, no external assets                  */
/* ------------------------------------------------------------------ */

/* Square-wave note kernel: 64 samples, half high / half low.
   Played via soundPlaySample at freq = desiredPitch * 64  →  any pitch. */
#define NOTE_WAVE_LEN   64
static s8  noteWave[NOTE_WAVE_LEN];

/* One-shot SFX buffers (signed 8-bit, mono) */
#define SFX_SHOOT_LEN   512
#define SFX_EXPL_LEN   1024
#define SFX_JUMP_LEN    768
#define SFX_HIT_LEN     768
#define SFX_PICKUP_LEN  512

static s8  sfxShoot  [SFX_SHOOT_LEN];
static s8  sfxExpl   [SFX_EXPL_LEN];
static s8  sfxJump   [SFX_JUMP_LEN];
static s8  sfxHit    [SFX_HIT_LEN];
static s8  sfxPickup [SFX_PICKUP_LEN];

/* ── BGM sequencer ──────────────────────────────────────────────── */
/* Pentatonic A-minor scale frequencies (Hz): A3 C4 D4 E4 G4 A4 C5 D5 E5 */
static const u16 noteFreq[] = { 220, 262, 294, 330, 392, 440, 523, 587, 659 };
#define NNOTES  (int)(sizeof(noteFreq)/sizeof(noteFreq[0]))

/*  Action-march loop: 32 steps of 16th notes @ ~115 BPM = ~7 frames/step.
    Values ≥0 → noteFreq index.  -1 → rest. */
static const s8 bgmSeq[] = {
    4, -1,  4,  5,   6, -1,  6,  7,   /* A4 . A4 C5 | C5 . C5 D5 */
    7,  5,  4,  2,   0, -1, -1, -1,   /* D5 C5 A4 G4 | A3 . . . */
    4, -1,  4,  5,   6, -1,  6,  8,   /* A4 . A4 C5 | C5 . C5 E5 */
    8,  6,  5,  4,   4, -1, -1, -1    /* E5 C5 D4 A4 | A4 . . . */
};
#define BGM_STEPS   (int)(sizeof(bgmSeq)/sizeof(bgmSeq[0]))
#define BGM_TICKS   7          /* frames per 16th-note step */

static int bgmTick;            /* frames counted since last step */
static int bgmStep;            /* current position in bgmSeq */
static int bgmCh;              /* last channel used for note (to kill early) */
static int bgmRunning;

static void buildSounds(void)
{
    /* ── note kernel: square wave ── */
    for (int i = 0; i < NOTE_WAVE_LEN; i++)
        noteWave[i] = (i < NOTE_WAVE_LEN / 2) ? 90 : -90;

    /* ── shoot: short high-pitched chirp with decay ── */
    for (int i = 0; i < SFX_SHOOT_LEN; i++) {
        int env = (SFX_SHOOT_LEN - i) * 100 / SFX_SHOOT_LEN;
        /* ~1.5 kHz square wave at 11025 Hz SR → period ≈ 7 samples */
        sfxShoot[i] = (s8)(((i / 4) & 1) ? env : -env);
    }

    /* ── explosion: filtered noise with long decay ── */
    {
        u32 seed = 0xDEADBEEFu;
        for (int i = 0; i < SFX_EXPL_LEN; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int noise = (int)(s8)(seed & 0xFF);
            int env   = (SFX_EXPL_LEN - i) * 100 / SFX_EXPL_LEN;
            /* mix with low-frequency rumble */
            int rumble = ((i / 32) & 1) ? 40 : -40;
            int s = (noise * env / 100) + (rumble * env / 100);
            sfxExpl[i] = (s8)(s < -127 ? -127 : s > 127 ? 127 : s);
        }
    }

    /* ── jump: rising pitch sweep ── */
    for (int i = 0; i < SFX_JUMP_LEN; i++) {
        /* period shrinks from 24→6 samples as i grows */
        int period = 24 - (i * 18 / SFX_JUMP_LEN);
        if (period < 1) period = 1;
        sfxJump[i] = (s8)(((i / period) & 1) ? 80 : -80);
    }

    /* ── boss hit: heavy thud ── */
    {
        u32 seed = 0xCAFEBABEu;
        for (int i = 0; i < SFX_HIT_LEN; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int noise = (int)(s8)(seed & 0xFF);
            int env   = (SFX_HIT_LEN - i) * 110 / SFX_HIT_LEN;
            /* very low-freq square for punch */
            int thud  = ((i / 60) & 1) ? 70 : -70;
            int s = (noise * env / 200) + (thud * env / 110);
            sfxHit[i] = (s8)(s < -127 ? -127 : s > 127 ? 127 : s);
        }
    }

    /* ── pickup chime: two-note rising sweep ──
       First half (~E5 ~660 Hz) then second half (~A5 ~880 Hz).
       Period shrinks across each half to give a bright upward chirp.
       Envelope fades gently so it doesn't clip over the shoot SFX. */
    for (int i = 0; i < SFX_PICKUP_LEN; i++) {
        int half = SFX_PICKUP_LEN / 2;
        int env  = (SFX_PICKUP_LEN - i) * 75 / SFX_PICKUP_LEN + 25;
        int period;
        if (i < half) {
            /* first note: period ~17 samples → ~648 Hz at 11025 Hz SR */
            period = 17 - (i * 3 / half);
        } else {
            /* second note: period ~11 samples → ~1002 Hz at 11025 Hz SR */
            int j = i - half;
            period = 11 - (j * 2 / half);
        }
        if (period < 1) period = 1;
        sfxPickup[i] = (s8)(((i / period) & 1) ? env : -env);
    }
}

/* Forward declaration: sound toggle lives in the save data */
static int soundEnabled(void);

/* Play a one-shot SFX on a free channel (channels 0-3 reserved for SFX) */
static void sndShoot(void)
{
    if (!soundEnabled()) return;
    soundPlaySample(sfxShoot, SoundFormat_8Bit, SFX_SHOOT_LEN,
                    11025, 100, 64, false, 0);
}

static void sndExplosion(void)
{
    if (!soundEnabled()) return;
    soundPlaySample(sfxExpl, SoundFormat_8Bit, SFX_EXPL_LEN,
                    11025, 127, 64, false, 0);
}

static void sndJump(void)
{
    if (!soundEnabled()) return;
    soundPlaySample(sfxJump, SoundFormat_8Bit, SFX_JUMP_LEN,
                    11025, 90, 64, false, 0);
}

static void sndBossHit(void)
{
    if (!soundEnabled()) return;
    soundPlaySample(sfxHit, SoundFormat_8Bit, SFX_HIT_LEN,
                    11025, 127, 64, false, 0);
}

static void sndPickup(void)
{
    if (!soundEnabled()) return;
    soundPlaySample(sfxPickup, SoundFormat_8Bit, SFX_PICKUP_LEN,
                    11025, 90, 64, false, 0);
}

static void bgmStart(void)
{
    /* Kill any still-looping channel from a previous session first. */
    if (bgmCh >= 0) { soundKill(bgmCh); bgmCh = -1; }
    bgmTick    = 0;
    bgmStep    = 0;
    bgmRunning = 1;
}

static void bgmStop(void)
{
    bgmRunning = 0;
    if (bgmCh >= 0) { soundKill(bgmCh); bgmCh = -1; }
}

/* Call once per frame while gameplay is running */
static void updateBgm(void)
{
    if (!bgmRunning) return;
    if (++bgmTick < BGM_TICKS) return;
    bgmTick = 0;

    /* silence previous note */
    if (bgmCh >= 0) { soundKill(bgmCh); bgmCh = -1; }

    int note = bgmSeq[bgmStep];
    bgmStep  = (bgmStep + 1) % BGM_STEPS;

    if (note >= 0 && note < NNOTES && soundEnabled()) {
        /* play at pitch: freq = notePitch * NOTE_WAVE_LEN */
        u16 sr = (u16)(noteFreq[note] * NOTE_WAVE_LEN);
        bgmCh  = soundPlaySample(noteWave, SoundFormat_8Bit,
                                 NOTE_WAVE_LEN, sr, 55, 64, true, 0);
    }
}

/* Per-stage boss configuration */
static const struct { int hp; int fireRate; int speed; } stageBoss[NUM_STAGES] = {
    { 60,  55, 1 },   /* Stage 1: standard tank                        */
    { 90,  38, 1 },   /* Stage 2: armoured tank – faster fire           */
    { 130, 26, 2 },   /* Stage 3: mega tank – rapid fire & double speed */
};

/* Weapon types */
typedef enum {
    WEAPON_DEFAULT = 0,   /* standard pistol – unlimited ammo */
    WEAPON_HMG,           /* heavy machine gun – rapid single shots */
    WEAPON_SPREAD,        /* spread shot – 3 bullets wide */
    WEAPON_ROCKET,        /* rocket launcher – slow, splash damage */
    WEAPON_COUNT
} WeaponType;

/* Pickup types */
typedef enum {
    PICKUP_HMG = 0,
    PICKUP_SPREAD,
    PICKUP_ROCKET,
    PICKUP_GRENADE,
    PICKUP_LIFE,
    PICKUP_TYPE_COUNT
} PickupType;

/* Sprite graphics slots */
enum GfxSlot {
    GFX_PLAYER = 0,
    GFX_PLAYER_CROUCH,
    GFX_BULLET,
    GFX_EBULLET,
    GFX_GRENADE,
    GFX_SOLDIER,
    GFX_TURRET,
    GFX_BOSS_BODY,
    GFX_BOSS_TURRET,
    GFX_EXPLOSION,
    GFX_PICKUP,
    GFX_ROCKET,           /* rocket projectile sprite */
    GFX_COUNT
};

/* Palette indices we fill with colours */
enum PalIdx {
    C_TRANS = 0,
    C_SKIN = 1, C_UNIFORM, C_UNIFORM_DK, C_METAL, C_METAL_DK,
    C_FLAME, C_FLAME2, C_ENEMY, C_ENEMY_DK, C_TRACER, C_SHADOW,
    C_PICKUP, C_WHITE
};

/* ------------------------------------------------------------------ */
/* Entity types                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    s32 x, y;        /* world position, 8.8 fixed point */
    s32 vx, vy;
    int alive;
    int timer;
    int hp;
    int type;        /* enemy: 0 soldier, 1 turret        */
    int facing;      /* 1 right, -1 left                  */
} Ent;

typedef struct {
    s32 x, y;         /* world px (not fixed)             */
    int hp;
    int active;       /* 0 dormant, 1 fighting, 2 dead    */
    int fireTimer;
    int moveTimer;
    s32 vx;
} Boss;

typedef struct {
    int x, y;         /* world pixel position             */
    int alive;
    int type;         /* PickupType                       */
    int flash;        /* animation counter                */
} Pickup;

static Ent bullets[MAX_BULLETS];
static Ent ebullets[MAX_EBULLETS];
static Ent grenades[MAX_GRENADES];
static Ent enemies[MAX_ENEMIES];
static Ent fx[MAX_FX];
static Boss boss;
static Pickup pickups[MAX_PICKUPS];

static struct {
    s32 x, y, vx, vy;    /* 8.8 fixed */
    int onGround, facing, aimUp, crouch;
    int fireCd, invuln, lives, grenAmmo;
    int alive;
    int weapon;           /* WeaponType                        */
    int weapAmmo;         /* ammo for current weapon; 0=default */
} pl;

static int camX;
static int score;
static int frame;
static int gameState;    /* 0 menu, 1 play, 2 gameover, 3 victory, 4 stage-clear */
static int spawnCursor;
static int currentStage;

/* ------------------------------------------------------------------ */
/* Save data & start menu                                              */
/* ------------------------------------------------------------------ */

#define SAVE_MAGIC     0x53564731u   /* "SVG1" */
#define SAVE_VERSION   1
#define MAX_HISTORY    8
#define MAX_HISCORES   5
#define SAVE_PATH      "/steel-vanguard.sav"

typedef struct {
    u32 score;
    u8  stage;      /* stage reached (1-based) */
    u8  result;     /* 0 = failed, 1 = mission complete */
    u8  pad[2];
} RunRecord;

typedef struct {
    u32 magic;
    u32 version;
    u32 highScore[MAX_HISCORES];
    u8  unlockedStages;   /* 1..NUM_STAGES */
    u8  soundOn;
    u8  historyCount;
    u8  pad;
    RunRecord history[MAX_HISTORY];
} SaveData;

static SaveData save;
static int fatOk;         /* 1 if a FAT save medium is available */
static int runRecorded;   /* guards against double-recording one run */

/* Menu state */
enum MenuScreen { MENU_MAIN = 0, MENU_STAGE, MENU_HISTORY, MENU_OPTIONS };
static int menuScreen;
static int menuCursor;

static void saveDefaults(void)
{
    memset(&save, 0, sizeof save);
    save.magic          = SAVE_MAGIC;
    save.version        = SAVE_VERSION;
    save.unlockedStages = 1;
    save.soundOn        = 1;
}

static void saveLoad(void)
{
    saveDefaults();
    fatOk = fatInitDefault();
    if (!fatOk) return;
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f) return;
    SaveData tmp;
    size_t n = fread(&tmp, 1, sizeof tmp, f);
    fclose(f);
    if (n == sizeof tmp && tmp.magic == SAVE_MAGIC && tmp.version == SAVE_VERSION) {
        save = tmp;
        if (save.unlockedStages < 1) save.unlockedStages = 1;
        if (save.unlockedStages > NUM_STAGES) save.unlockedStages = NUM_STAGES;
        if (save.historyCount > MAX_HISTORY) save.historyCount = MAX_HISTORY;
    }
}

static void saveWrite(void)
{
    if (!fatOk) return;
    FILE *f = fopen(SAVE_PATH, "wb");
    if (!f) return;
    fwrite(&save, 1, sizeof save, f);
    fclose(f);
}

/* Record a finished run into history + high scores, then persist */
static void recordRun(u32 runScore, int stageReached, int result)
{
    if (runRecorded) return;
    runRecorded = 1;

    /* history: newest first */
    for (int i = MAX_HISTORY - 1; i > 0; i--)
        save.history[i] = save.history[i - 1];
    save.history[0].score  = runScore;
    save.history[0].stage  = (u8)(stageReached + 1);
    save.history[0].result = (u8)result;
    if (save.historyCount < MAX_HISTORY) save.historyCount++;

    /* high scores: sorted insert */
    for (int i = 0; i < MAX_HISCORES; i++) {
        if (runScore > save.highScore[i]) {
            for (int j = MAX_HISCORES - 1; j > i; j--)
                save.highScore[j] = save.highScore[j - 1];
            save.highScore[i] = runScore;
            break;
        }
    }

    saveWrite();
}

static void unlockStage(int stage)   /* stage: 0-based index to unlock */
{
    if (stage >= NUM_STAGES) return;
    if (save.unlockedStages < stage + 1) {
        save.unlockedStages = (u8)(stage + 1);
        saveWrite();
    }
}

/* Pickup notification banner */
#define NOTIFY_DURATION 90          /* frames (~1.5 s at 60 fps) */
static char pickupNotifyText[32];   /* text to show, e.g. "HMG x80 GET!" */
static int  pickupNotifyTimer;      /* counts down to 0; 0 = hidden       */

/* ------------------------------------------------------------------ */
/* Spawn tables – one per stage                                        */
/* ------------------------------------------------------------------ */

/* Stage 1: rolling hills, light resistance */
static const int spawnTable0[][2] = {
    { 320, 0 }, { 430, 0 }, { 560, 1 }, { 700, 0 }, { 760, 0 },
    { 900, 1 }, { 1050, 0 }, { 1120, 0 }, { 1200, 1 }, { 1380, 0 },
    { 1450, 0 }, { 1520, 0 }, { 1650, 1 }, { 1800, 0 }, { 1880, 0 },
    { 2000, 1 }, { 2150, 0 }, { 2230, 0 }, { 2320, 1 }, { 2480, 0 },
    { 2560, 0 }, { 2640, 1 }, { -1, -1 }
};

/* Stage 2: desert outpost, heavier turret coverage */
static const int spawnTable1[][2] = {
    { 280, 1 }, { 400, 0 }, { 480, 0 }, { 590, 1 }, { 680, 0 },
    { 780, 1 }, { 880, 0 }, { 960, 0 }, { 1060, 1 }, { 1160, 1 },
    { 1280, 0 }, { 1360, 0 }, { 1440, 1 }, { 1560, 0 }, { 1640, 0 },
    { 1740, 1 }, { 1860, 1 }, { 1980, 0 }, { 2080, 0 }, { 2180, 1 },
    { 2300, 0 }, { 2400, 1 }, { 2520, 0 }, { 2620, 1 }, { -1, -1 }
};

/* Stage 3: factory fortress, dense mixed squads */
static const int spawnTable2[][2] = {
    { 260, 0 }, { 340, 1 }, { 420, 0 }, { 500, 1 }, { 580, 0 },
    { 660, 0 }, { 740, 1 }, { 820, 0 }, { 900, 1 }, { 980, 0 },
    { 1080, 1 }, { 1160, 0 }, { 1240, 0 }, { 1320, 1 }, { 1400, 0 },
    { 1500, 1 }, { 1580, 0 }, { 1660, 1 }, { 1760, 0 }, { 1840, 0 },
    { 1940, 1 }, { 2020, 0 }, { 2120, 1 }, { 2200, 0 }, { 2300, 1 },
    { 2400, 0 }, { 2480, 1 }, { 2580, 0 }, { 2660, 1 }, { -1, -1 }
};

static const int (*spawnTables[NUM_STAGES])[2] = {
    spawnTable0, spawnTable1, spawnTable2
};

/* ------------------------------------------------------------------ */
/* Pickup spawn tables – one per stage                                 */
/* { worldX, PickupType }   terminated with { -1, -1 }                */
/* ------------------------------------------------------------------ */

/* Stage 1: introduce HMG and Spread */
static const int pickupSpawns0[][2] = {
    { 550,  PICKUP_HMG    },
    { 900,  PICKUP_GRENADE},
    { 1300, PICKUP_SPREAD },
    { 1700, PICKUP_HMG    },
    { 2100, PICKUP_ROCKET },
    { 2500, PICKUP_LIFE   },
    { -1, -1 }
};

/* Stage 2: heavier ammo drops */
static const int pickupSpawns1[][2] = {
    { 450,  PICKUP_SPREAD },
    { 800,  PICKUP_GRENADE},
    { 1150, PICKUP_HMG    },
    { 1500, PICKUP_ROCKET },
    { 1900, PICKUP_GRENADE},
    { 2300, PICKUP_SPREAD },
    { 2650, PICKUP_LIFE   },
    { -1, -1 }
};

/* Stage 3: rocket and life pickups more frequent */
static const int pickupSpawns2[][2] = {
    { 400,  PICKUP_HMG    },
    { 700,  PICKUP_ROCKET },
    { 1000, PICKUP_GRENADE},
    { 1350, PICKUP_SPREAD },
    { 1650, PICKUP_LIFE   },
    { 2000, PICKUP_HMG    },
    { 2350, PICKUP_ROCKET },
    { 2700, PICKUP_LIFE   },
    { -1, -1 }
};

static const int (*pickupSpawnTables[NUM_STAGES])[2] = {
    pickupSpawns0, pickupSpawns1, pickupSpawns2
};

static int pickupSpawnCursor;

static u16 *gfx[GFX_COUNT];
static u16 *bossGfx;            /* 32x32 boss body */

/* ------------------------------------------------------------------ */
/* Procedural sprite art                                               */
/* ------------------------------------------------------------------ */

/* draw one 8bpp pixel into a tiled 16x16 sprite buffer */
static void px16(u8 *buf, int x, int y, u8 c)
{
    if (x < 0 || x > 15 || y < 0 || y > 15) return;
    int tile = (y / 8) * 2 + (x / 8);
    buf[tile * 64 + (y % 8) * 8 + (x % 8)] = c;
}

static void px32(u8 *buf, int x, int y, u8 c)
{
    if (x < 0 || x > 31 || y < 0 || y > 31) return;
    int tile = (y / 8) * 4 + (x / 8);
    buf[tile * 64 + (y % 8) * 8 + (x % 8)] = c;
}

static void rect16(u8 *buf, int x0, int y0, int w, int h, u8 c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            px16(buf, x, y, c);
}

static void rect32(u8 *buf, int x0, int y0, int w, int h, u8 c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            px32(buf, x, y, c);
}

static void copyToVram(u16 *dst, const u8 *src, int bytes)
{
    /* VRAM needs 16-bit writes */
    const u16 *s = (const u16 *)src;
    for (int i = 0; i < bytes / 2; i++) dst[i] = s[i];
}

static void buildArt(void)
{
    static u8 buf[32 * 32];

    /* --- player standing (16x16) --- */
    memset(buf, 0, 256);
    rect16(buf, 6, 1, 4, 3, C_SKIN);            /* head   */
    rect16(buf, 5, 0, 6, 2, C_UNIFORM_DK);      /* helmet */
    rect16(buf, 5, 4, 6, 6, C_UNIFORM);         /* torso  */
    rect16(buf, 9, 5, 6, 2, C_METAL);           /* rifle  */
    rect16(buf, 5, 10, 2, 5, C_UNIFORM_DK);     /* legs   */
    rect16(buf, 9, 10, 2, 5, C_UNIFORM_DK);
    rect16(buf, 4, 15, 4, 1, C_SHADOW);         /* boots  */
    rect16(buf, 8, 15, 4, 1, C_SHADOW);
    copyToVram(gfx[GFX_PLAYER], buf, 256);

    /* --- player crouch --- */
    memset(buf, 0, 256);
    rect16(buf, 6, 6, 4, 3, C_SKIN);
    rect16(buf, 5, 5, 6, 2, C_UNIFORM_DK);
    rect16(buf, 4, 9, 8, 4, C_UNIFORM);
    rect16(buf, 10, 10, 6, 2, C_METAL);
    rect16(buf, 4, 13, 8, 3, C_UNIFORM_DK);
    copyToVram(gfx[GFX_PLAYER_CROUCH], buf, 256);

    /* --- player bullet --- */
    memset(buf, 0, 256);
    rect16(buf, 5, 7, 6, 2, C_TRACER);
    rect16(buf, 7, 6, 2, 4, C_FLAME);
    copyToVram(gfx[GFX_BULLET], buf, 256);

    /* --- enemy bullet --- */
    memset(buf, 0, 256);
    rect16(buf, 6, 6, 4, 4, C_FLAME);
    rect16(buf, 7, 7, 2, 2, C_FLAME2);
    copyToVram(gfx[GFX_EBULLET], buf, 256);

    /* --- grenade --- */
    memset(buf, 0, 256);
    rect16(buf, 6, 6, 4, 5, C_UNIFORM_DK);
    rect16(buf, 7, 4, 2, 2, C_METAL);
    copyToVram(gfx[GFX_GRENADE], buf, 256);

    /* --- enemy soldier --- */
    memset(buf, 0, 256);
    rect16(buf, 6, 1, 4, 3, C_SKIN);
    rect16(buf, 5, 0, 6, 2, C_ENEMY_DK);
    rect16(buf, 5, 4, 6, 6, C_ENEMY);
    rect16(buf, 1, 5, 6, 2, C_METAL);
    rect16(buf, 5, 10, 2, 5, C_ENEMY_DK);
    rect16(buf, 9, 10, 2, 5, C_ENEMY_DK);
    copyToVram(gfx[GFX_SOLDIER], buf, 256);

    /* --- turret --- */
    memset(buf, 0, 256);
    rect16(buf, 2, 9, 12, 6, C_METAL_DK);
    rect16(buf, 5, 5, 6, 5, C_METAL);
    rect16(buf, 0, 6, 6, 2, C_METAL_DK);
    copyToVram(gfx[GFX_TURRET], buf, 256);

    /* --- explosion --- */
    memset(buf, 0, 256);
    rect16(buf, 3, 3, 10, 10, C_FLAME);
    rect16(buf, 5, 5, 6, 6, C_FLAME2);
    rect16(buf, 7, 7, 2, 2, C_WHITE);
    copyToVram(gfx[GFX_EXPLOSION], buf, 256);

    /* --- pickup (grenade crate) --- */
    memset(buf, 0, 256);
    rect16(buf, 3, 5, 10, 8, C_PICKUP);
    rect16(buf, 4, 6, 8, 6, C_UNIFORM_DK);
    rect16(buf, 7, 5, 2, 8, C_PICKUP);
    copyToVram(gfx[GFX_PICKUP], buf, 256);

    /* --- rocket projectile --- */
    memset(buf, 0, 256);
    rect16(buf, 3, 7, 8, 3, C_FLAME);        /* body */
    rect16(buf, 10, 7, 3, 3, C_FLAME2);      /* nose */
    rect16(buf, 1, 7, 2, 1, C_METAL_DK);     /* tail fin top */
    rect16(buf, 1, 9, 2, 1, C_METAL_DK);     /* tail fin bot */
    copyToVram(gfx[GFX_ROCKET], buf, 256);

    /* --- boss turret (16x16) --- */
    memset(buf, 0, 256);
    rect16(buf, 4, 6, 8, 8, C_METAL);
    rect16(buf, 0, 8, 8, 3, C_METAL_DK);
    copyToVram(gfx[GFX_BOSS_TURRET], buf, 256);

    /* --- boss body (32x32 tank) --- */
    memset(buf, 0, 1024);
    rect32(buf, 2, 14, 28, 10, C_METAL);        /* hull   */
    rect32(buf, 8, 8, 14, 8, C_METAL_DK);       /* cabin  */
    rect32(buf, 0, 22, 32, 8, C_SHADOW);        /* tracks */
    for (int i = 2; i < 30; i += 5)
        rect32(buf, i, 24, 3, 4, C_METAL_DK);   /* wheels */
    copyToVram(bossGfx, buf, 1024);
}

/* ------------------------------------------------------------------ */
/* Background (tiled ground + sky decoration)                          */
/* ------------------------------------------------------------------ */

static int bgId;

static void buildBackground(void)
{
    bgId = bgInit(0, BgType_Text8bpp, BgSize_T_512x256, 4, 0);

    u8 tile[64];
    u16 *tiles = bgGetGfxPtr(bgId);

    /* tile 0: empty */
    memset(tile, 0, 64);
    copyToVram(tiles, tile, 64);
    /* tile 1: dirt/ground fill */
    for (int i = 0; i < 64; i++) tile[i] = ((i * 7 + i / 8) % 11 == 0) ? C_UNIFORM_DK : C_SHADOW;
    copyToVram(tiles + 32, tile, 64);
    /* tile 2: ground surface */
    for (int i = 0; i < 64; i++) tile[i] = (i < 16) ? C_ENEMY : C_SHADOW;
    copyToVram(tiles + 64, tile, 64);
    /* tile 3: distant feature (ridge / dune / girder) */
    for (int i = 0; i < 64; i++) tile[i] = (i / 8 > (i % 8) / 2 + 2) ? C_METAL_DK : 0;
    copyToVram(tiles + 96, tile, 64);

    u16 *map = bgGetMapPtr(bgId);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            u16 t = 0;
            int py = y * 8;
            if (py >= GROUND_Y + 8) t = 1;
            else if (py >= GROUND_Y) t = 2;
            else if (py >= GROUND_Y - 16 && ((x * 13) % 7) < 2) t = 3;
            /* 512-wide text bg uses two screenblocks side by side */
            int sb = (x >= 32) ? 1 : 0;
            map[sb * 1024 + y * 32 + (x % 32)] = t;
        }
    }

    /* Per-stage palette: sky, ground, surface, distant feature */
    switch (currentStage) {
        default:
        case 0: /* Rolling hills – blue sky, green grass */
            BG_PALETTE[0]            = RGB15(8, 14, 24);   /* sky        */
            BG_PALETTE[C_SHADOW]     = RGB15(6, 5, 3);
            BG_PALETTE[C_UNIFORM_DK] = RGB15(10, 8, 4);
            BG_PALETTE[C_ENEMY]      = RGB15(6, 14, 4);
            BG_PALETTE[C_METAL_DK]   = RGB15(9, 9, 12);
            break;
        case 1: /* Desert outpost – ochre sky, sandy ground */
            BG_PALETTE[0]            = RGB15(22, 18, 10);  /* hazy sky   */
            BG_PALETTE[C_SHADOW]     = RGB15(18, 14, 6);
            BG_PALETTE[C_UNIFORM_DK] = RGB15(22, 17, 8);
            BG_PALETTE[C_ENEMY]      = RGB15(24, 20, 10);
            BG_PALETTE[C_METAL_DK]   = RGB15(14, 10, 4);
            break;
        case 2: /* Factory fortress – dark sky, grey concrete */
            BG_PALETTE[0]            = RGB15(4, 4, 6);     /* night sky  */
            BG_PALETTE[C_SHADOW]     = RGB15(6, 6, 7);
            BG_PALETTE[C_UNIFORM_DK] = RGB15(10, 10, 11);
            BG_PALETTE[C_ENEMY]      = RGB15(8, 8, 9);
            BG_PALETTE[C_METAL_DK]   = RGB15(6, 8, 12);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static Ent *allocEnt(Ent *pool, int n)
{
    for (int i = 0; i < n; i++)
        if (!pool[i].alive) { memset(&pool[i], 0, sizeof(Ent)); pool[i].alive = 1; return &pool[i]; }
    return NULL;
}

static void spawnFx(int x, int y)
{
    Ent *e = allocEnt(fx, MAX_FX);
    if (e) { e->x = x << 8; e->y = y << 8; e->timer = 18; }
    sndExplosion();
}

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void hurtPlayer(void)
{
    /* ignore hits once the frame has already left active gameplay
       (e.g. boss killed earlier this frame -> stage clear / victory) */
    if (gameState != 1) return;
    if (pl.invuln > 0 || !pl.alive) return;
    pl.lives--;
    pl.invuln = INVULN_FRAMES;
    spawnFx(pl.x >> 8, pl.y >> 8);
    if (pl.lives <= 0) {
        pl.alive = 0; bgmStop(); gameState = 2;
        recordRun(score, currentStage, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Game setup                                                          */
/* ------------------------------------------------------------------ */

static int soundEnabled(void)
{
    return save.soundOn;
}

static void resetGame(int startStage)
{
    memset(bullets,  0, sizeof bullets);
    memset(ebullets, 0, sizeof ebullets);
    memset(grenades, 0, sizeof grenades);
    memset(enemies,  0, sizeof enemies);
    memset(fx,       0, sizeof fx);
    memset(pickups,  0, sizeof pickups);
    memset(&pl, 0, sizeof pl);
    pl.x = 40 << 8;
    pl.y = (GROUND_Y - 16) << 8;
    pl.facing   = 1;
    pl.lives    = PLAYER_LIVES;
    pl.grenAmmo = GRENADE_AMMO;
    pl.alive    = 1;
    pl.weapon   = WEAPON_DEFAULT;
    pl.weapAmmo = 0;
    camX          = 0;
    score         = 0;
    spawnCursor   = 0;
    pickupSpawnCursor = 0;
    currentStage  = startStage;
    pickupNotifyTimer = 0;
    pickupNotifyText[0] = '\0';
    runRecorded   = 0;
    boss.x = BOSS_X; boss.y = GROUND_Y - 32;
    boss.hp = stageBoss[currentStage].hp; boss.active = 0; boss.fireTimer = 0; boss.vx = 0;
    buildBackground();   /* apply stage palette */
    bgmStart();
    gameState = 1;
}

/* Advance to the next stage, keeping score and lives */
static void advanceStage(void)
{
    currentStage++;
    memset(bullets,  0, sizeof bullets);
    memset(ebullets, 0, sizeof ebullets);
    memset(grenades, 0, sizeof grenades);
    memset(enemies,  0, sizeof enemies);
    memset(fx,       0, sizeof fx);
    memset(pickups,  0, sizeof pickups);

    /* Reposition player at stage start */
    pl.x = 40 << 8;
    pl.y = (GROUND_Y - 16) << 8;
    pl.facing   = 1;
    pl.vx       = 0;
    pl.vy       = 0;
    pl.fireCd   = 0;
    pl.invuln   = 0;
    pl.onGround = 0;
    pl.crouch   = 0;
    pl.aimUp    = 0;
    pl.weapon   = WEAPON_DEFAULT;
    pl.weapAmmo = 0;

    camX              = 0;
    spawnCursor       = 0;
    pickupSpawnCursor = 0;
    pickupNotifyTimer = 0;
    pickupNotifyText[0] = '\0';

    boss.x         = BOSS_X;
    boss.y         = GROUND_Y - 32;
    boss.hp        = stageBoss[currentStage].hp;
    boss.active    = 0;
    boss.fireTimer = 0;
    boss.moveTimer = 0;
    boss.vx        = 0;

    buildBackground();   /* apply new stage palette */
    bgmStart();
    gameState = 1;
}

/* ------------------------------------------------------------------ */
/* Update logic                                                        */
/* ------------------------------------------------------------------ */

static void firePlayerBullet(void)
{
    /* Rockets get their own handling */
    if (pl.weapon == WEAPON_ROCKET) {
        if (pl.weapAmmo <= 0) { pl.weapon = WEAPON_DEFAULT; pl.weapAmmo = 0; }
        else {
            Ent *r = allocEnt(bullets, MAX_BULLETS);
            if (!r) return;
            r->x  = pl.x + (pl.facing > 0 ? (10 << 8) : (-4 << 8));
            r->y  = pl.y + ((pl.crouch ? 10 : 5) << 8);
            if (pl.aimUp) { r->vx = 0; r->vy = -3 * 256; r->x = pl.x + (6 << 8); r->y = pl.y - (4 << 8); }
            else          { r->vx = pl.facing * 3 * 256; r->vy = 0; }
            r->type = 1;   /* 1 = rocket */
            pl.weapAmmo--;
            pl.fireCd = ROCKET_COOLDOWN;
            sndShoot();
        }
        return;
    }

    /* Spread shot: three bullets */
    if (pl.weapon == WEAPON_SPREAD) {
        if (pl.weapAmmo <= 0) { pl.weapon = WEAPON_DEFAULT; pl.weapAmmo = 0; }
        else {
            static const int spreadVy[3] = { -192, 0, 192 };
            for (int s = 0; s < 3; s++) {
                Ent *b = allocEnt(bullets, MAX_BULLETS);
                if (!b) continue;
                b->x = pl.x + (pl.facing > 0 ? (10 << 8) : (-4 << 8));
                b->y = pl.y + ((pl.crouch ? 10 : 5) << 8);
                if (pl.aimUp) {
                    b->vx = spreadVy[s];
                    b->vy = -5 * 256;
                    b->x  = pl.x + (6 << 8);
                    b->y  = pl.y - (4 << 8);
                } else {
                    b->vx = pl.facing * 5 * 256;
                    b->vy = spreadVy[s];
                }
                b->type = 0;
            }
            pl.weapAmmo--;
            pl.fireCd = SPREAD_COOLDOWN;
            sndShoot();
        }
        return;
    }

    /* HMG / default: single fast bullet */
    Ent *b = allocEnt(bullets, MAX_BULLETS);
    if (!b) return;
    b->x = pl.x + (pl.facing > 0 ? (10 << 8) : (-4 << 8));
    b->y = pl.y + ((pl.crouch ? 10 : 5) << 8);
    if (pl.aimUp) { b->vx = 0; b->vy = -5 * 256; b->x = pl.x + (6 << 8); b->y = pl.y - (4 << 8); }
    else          { b->vx = pl.facing * 5 * 256; b->vy = 0; }
    b->type = 0;

    if (pl.weapon == WEAPON_HMG) {
        if (pl.weapAmmo <= 0) { pl.weapon = WEAPON_DEFAULT; pl.weapAmmo = 0; pl.fireCd = FIRE_COOLDOWN; }
        else { pl.weapAmmo--; pl.fireCd = HMG_COOLDOWN; }
    } else {
        pl.fireCd = FIRE_COOLDOWN;
    }
    sndShoot();
}

static void updatePlayer(void)
{
    int held = keysHeld();
    int down = keysDown();

    pl.crouch = pl.onGround && (held & KEY_DOWN);
    pl.aimUp = (held & KEY_UP) != 0;

    pl.vx = 0;
    if (!pl.crouch) {
        if (held & KEY_LEFT)  { pl.vx = -MOVE_SPEED; pl.facing = -1; }
        if (held & KEY_RIGHT) { pl.vx =  MOVE_SPEED; pl.facing =  1; }
    }

    if ((down & KEY_B) && pl.onGround && !pl.crouch) { pl.vy = JUMP_VEL; sndJump(); }

    if (pl.fireCd > 0) pl.fireCd--;
    if ((held & (KEY_A | KEY_Y)) && pl.fireCd == 0) firePlayerBullet();

    if ((down & KEY_R) && pl.grenAmmo > 0) {
        Ent *g = allocEnt(grenades, MAX_GRENADES);
        if (g) {
            g->x = pl.x; g->y = pl.y;
            g->vx = pl.facing * 3 * 256; g->vy = -4 * 256;
            pl.grenAmmo--;
        }
    }

    pl.vy += GRAVITY;
    pl.x += pl.vx;
    pl.y += pl.vy;

    if (pl.x < camX << 8) pl.x = camX << 8;
    if (pl.x > (LEVEL_LEN - 16) << 8) pl.x = (LEVEL_LEN - 16) << 8;

    pl.onGround = 0;
    if ((pl.y >> 8) >= GROUND_Y - 16) {
        pl.y = (GROUND_Y - 16) << 8;
        pl.vy = 0;
        pl.onGround = 1;
    }

    if (pl.invuln > 0) pl.invuln--;

    /* camera follows, clamped to level */
    int px = pl.x >> 8;
    int target = px - 96;
    if (target > camX) camX = target;
    if (camX > LEVEL_LEN - SCREEN_W) camX = LEVEL_LEN - SCREEN_W;
    if (camX < 0) camX = 0;
}

/* Spawn a pickup at world-pixel (x, y) with given type */
static void spawnPickup(int x, int y, int type)
{
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!pickups[i].alive) {
            pickups[i].x     = x;
            pickups[i].y     = y;
            pickups[i].alive = 1;
            pickups[i].type  = type;
            pickups[i].flash = 0;
            return;
        }
    }
}

/* Advance through the pickup spawn table for the current stage */
static void spawnPickupsCursor(void)
{
    const int (*tbl)[2] = pickupSpawnTables[currentStage];
    while (tbl[pickupSpawnCursor][0] >= 0 &&
           tbl[pickupSpawnCursor][0] < camX + SCREEN_W + 64) {
        spawnPickup(tbl[pickupSpawnCursor][0], GROUND_Y - 20,
                    tbl[pickupSpawnCursor][1]);
        pickupSpawnCursor++;
    }
}

static void updatePickups(void)
{
    spawnPickupsCursor();

    for (int i = 0; i < MAX_PICKUPS; i++) {
        Pickup *p = &pickups[i];
        if (!p->alive) continue;
        p->flash++;

        /* Cull pickups well behind the camera */
        if (p->x < camX - 32) { p->alive = 0; continue; }

        /* Collection check */
        int px = pl.x >> 8, py = pl.y >> 8;
        if (pl.alive && overlap(p->x, p->y, 14, 14, px, py, 12, 16)) {
            p->alive = 0;
            switch (p->type) {
                case PICKUP_HMG:
                    pl.weapon   = WEAPON_HMG;
                    pl.weapAmmo = HMG_AMMO;
                    strcpy(pickupNotifyText, "HMG x80 GET!");
                    break;
                case PICKUP_SPREAD:
                    pl.weapon   = WEAPON_SPREAD;
                    pl.weapAmmo = SPREAD_AMMO;
                    strcpy(pickupNotifyText, "SPREAD x40 GET!");
                    break;
                case PICKUP_ROCKET:
                    pl.weapon   = WEAPON_ROCKET;
                    pl.weapAmmo = ROCKET_AMMO;
                    strcpy(pickupNotifyText, "ROCKET x15 GET!");
                    break;
                case PICKUP_GRENADE:
                    pl.grenAmmo += 5;
                    if (pl.grenAmmo > 20) pl.grenAmmo = 20;
                    strcpy(pickupNotifyText, "+5 GRENADES");
                    break;
                case PICKUP_LIFE:
                    pl.lives++;
                    if (pl.lives > 9) pl.lives = 9;
                    strcpy(pickupNotifyText, "EXTRA LIFE!");
                    break;
            }
            pickupNotifyTimer = NOTIFY_DURATION;
            sndPickup();
        }
    }
}

static void spawnEnemies(void)
{
    const int (*tbl)[2] = spawnTables[currentStage];
    while (tbl[spawnCursor][0] >= 0 &&
           tbl[spawnCursor][0] < camX + SCREEN_W + 32) {
        Ent *e = allocEnt(enemies, MAX_ENEMIES);
        if (!e) break;
        e->type = tbl[spawnCursor][1];
        e->x    = tbl[spawnCursor][0] << 8;
        e->y    = (GROUND_Y - 16) << 8;
        e->hp   = e->type == 1 ? 5 : 2;
        e->facing = -1;
        e->timer  = 40 + (spawnCursor * 17) % 50;
        spawnCursor++;
    }
}

static void enemyFire(Ent *e, int aimAtPlayer)
{
    Ent *b = allocEnt(ebullets, MAX_EBULLETS);
    if (!b) return;
    b->x = e->x; b->y = e->y + (5 << 8);
    if (aimAtPlayer) {
        int dx = (pl.x - e->x) >> 8;
        int dy = (pl.y - e->y) >> 8;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        int m = adx > ady ? adx : ady;
        if (m < 1) m = 1;
        b->vx = dx * 512 / m;
        b->vy = dy * 512 / m;
    } else {
        b->vx = e->facing * 3 * 256;
        b->vy = 0;
    }
}

static void updateEnemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ent *e = &enemies[i];
        if (!e->alive) continue;

        e->facing = ((pl.x >> 8) < (e->x >> 8)) ? -1 : 1;

        if (e->type == 0) {
            /* soldier: advance toward player, stop and shoot */
            int dist = ((e->x - pl.x) >> 8);
            if (dist < 0) dist = -dist;
            if (dist > 70) e->x += e->facing * 192;
            if (--e->timer <= 0) { enemyFire(e, 0); e->timer = 70; }
        } else {
            /* turret: slower, aimed shots */
            if (--e->timer <= 0) { enemyFire(e, 1); e->timer = 90; }
        }

        /* contact damage */
        if (overlap(e->x >> 8, e->y >> 8, 14, 16, pl.x >> 8, pl.y >> 8, 12, 16))
            hurtPlayer();

        /* cull far behind camera */
        if ((e->x >> 8) < camX - 48) e->alive = 0;
    }
}

static void updateBoss(void)
{
    if (boss.active == 2) return;
    if (!boss.active) {
        if (camX + SCREEN_W > BOSS_X - 20) boss.active = 1;
        return;
    }

    /* lumber back and forth – speed scales with stage */
    int bspeed = stageBoss[currentStage].speed;
    if (++boss.moveTimer > 120) { boss.moveTimer = 0; boss.vx = -boss.vx; }
    if (boss.vx == 0) boss.vx = -bspeed;
    /* stage 3 boss moves every frame; earlier bosses every other frame */
    int moveNow = (bspeed > 1) ? 1 : (frame & 1);
    boss.x += moveNow ? boss.vx : 0;
    if (boss.x < BOSS_X - 60) boss.x = BOSS_X - 60;
    if (boss.x > LEVEL_LEN - 40) boss.x = LEVEL_LEN - 40;

    /* fire spread – rate and spread widen with stage */
    if (++boss.fireTimer > stageBoss[currentStage].fireRate) {
        boss.fireTimer = 0;
        int shots = (currentStage >= 2) ? 5 : 3;  /* Stage 3: 5-way spread */
        for (int s = -(shots / 2); s <= (shots / 2); s++) {
            Ent *b = allocEnt(ebullets, MAX_EBULLETS);
            if (!b) break;
            b->x = boss.x << 8; b->y = (boss.y + 8) << 8;
            b->vx = -3 * 256; b->vy = s * 148;
        }
    }

    if (overlap(boss.x, boss.y, 32, 32, pl.x >> 8, pl.y >> 8, 12, 16))
        hurtPlayer();
}

static void damageBoss(int dmg, int x, int y)
{
    if (boss.active != 1) return;
    boss.hp -= dmg;
    sndBossHit();
    spawnFx(x, y);
    if (boss.hp <= 0) {
        boss.active = 2;
        score += 5000;
        for (int i = 0; i < 5; i++)
            spawnFx(boss.x + (i * 11) % 28, boss.y + (i * 7) % 24);
        /* Last stage → final victory; otherwise stage-clear transition */
        if (currentStage >= NUM_STAGES - 1) {
            bgmStop();
            gameState = 3;
            recordRun(score, currentStage, 1);
        } else {
            gameState = 4;
            unlockStage(currentStage + 1);   /* stage select unlock */
        }
    }
}

static void updateProjectiles(void)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        Ent *b = &bullets[i];
        if (!b->alive) continue;
        b->x += b->vx; b->y += b->vy;
        int bx = b->x >> 8, by = b->y >> 8;
        if (bx < camX - 16 || bx > camX + SCREEN_W + 16 || by < -16) { b->alive = 0; continue; }

        for (int j = 0; j < MAX_ENEMIES; j++) {
            Ent *e = &enemies[j];
            if (!e->alive) continue;
            if (overlap(bx + 4, by + 6, 8, 4, e->x >> 8, e->y >> 8, 14, 16)) {
                b->alive = 0;
                if (b->type == 1) {
                    /* rocket: area splash */
                    for (int k = 0; k < MAX_ENEMIES; k++) {
                        Ent *se = &enemies[k];
                        if (se->alive && overlap(bx - 20, by - 20, 56, 56,
                                                 se->x >> 8, se->y >> 8, 14, 16)) {
                            se->alive = 0;
                            score += se->type == 1 ? 300 : 100;
                            spawnFx(se->x >> 8, se->y >> 8);
                        }
                    }
                } else {
                    if (--e->hp <= 0) {
                        e->alive = 0;
                        score += e->type == 1 ? 300 : 100;
                        spawnFx(e->x >> 8, e->y >> 8);
                        /* ~1-in-5 chance to drop a pickup on enemy death */
                        if (((e->x >> 8) * 3 + j * 7) % 5 == 0) {
                            int dropType = ((e->x >> 8) + j * 13) % PICKUP_TYPE_COUNT;
                            /* don't drop life pickups from normal kills */
                            if (dropType == PICKUP_LIFE) dropType = PICKUP_GRENADE;
                            spawnPickup(e->x >> 8, e->y >> 8, dropType);
                        }
                    }
                }
                break;
            }
        }
        if (b->alive && boss.active == 1 &&
            overlap(bx + 4, by + 6, 8, 4, boss.x, boss.y, 32, 32)) {
            b->alive = 0;
            if (b->type == 1) {
                /* rocket splash on boss */
                damageBoss(8, bx, by);
                for (int j = 0; j < MAX_ENEMIES; j++) {
                    Ent *e = &enemies[j];
                    if (e->alive && overlap(bx - 20, by - 20, 56, 56,
                                           e->x >> 8, e->y >> 8, 14, 16)) {
                        e->alive = 0;
                        score += e->type == 1 ? 300 : 100;
                        spawnFx(e->x >> 8, e->y >> 8);
                    }
                }
            } else {
                damageBoss(1, bx, by);
            }
        }
    }

    for (int i = 0; i < MAX_GRENADES; i++) {
        Ent *g = &grenades[i];
        if (!g->alive) continue;
        g->vy += GRAVITY;
        g->x += g->vx; g->y += g->vy;
        int gx = g->x >> 8, gy = g->y >> 8;
        if (gy >= GROUND_Y - 12) {
            g->alive = 0;
            spawnFx(gx, gy);
            /* area damage */
            for (int j = 0; j < MAX_ENEMIES; j++) {
                Ent *e = &enemies[j];
                if (e->alive && overlap(gx - 16, gy - 16, 44, 44, e->x >> 8, e->y >> 8, 14, 16)) {
                    e->alive = 0;
                    score += e->type == 1 ? 300 : 100;
                    spawnFx(e->x >> 8, e->y >> 8);
                }
            }
            if (boss.active == 1 && overlap(gx - 16, gy - 16, 44, 44, boss.x, boss.y, 32, 32))
                damageBoss(6, gx, gy);
        }
    }

    for (int i = 0; i < MAX_EBULLETS; i++) {
        Ent *b = &ebullets[i];
        if (!b->alive) continue;
        b->x += b->vx; b->y += b->vy;
        int bx = b->x >> 8, by = b->y >> 8;
        if (bx < camX - 16 || bx > camX + SCREEN_W + 16 || by > SCREEN_H + 16) { b->alive = 0; continue; }
        int ph = pl.crouch ? 10 : 16;
        int py = (pl.y >> 8) + (pl.crouch ? 6 : 0);
        if (pl.alive && overlap(bx + 6, by + 6, 4, 4, (pl.x >> 8) + 2, py, 12, ph)) {
            b->alive = 0;
            hurtPlayer();
        }
    }

    for (int i = 0; i < MAX_FX; i++)
        if (fx[i].alive && --fx[i].timer <= 0) fx[i].alive = 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void draw(void)
{
    int id = 0;
    oamClear(&oamMain, 0, 128);

    bgSetScroll(bgId, camX & 511, 0);
    bgUpdate();

    /* player (blinks while invulnerable) */
    if (pl.alive && !(pl.invuln & 4)) {
        oamSet(&oamMain, id++, (pl.x >> 8) - camX, pl.y >> 8, 0, 0,
               SpriteSize_16x16, SpriteColorFormat_256Color,
               pl.crouch ? gfx[GFX_PLAYER_CROUCH] : gfx[GFX_PLAYER],
               -1, false, false, pl.facing < 0, false, false);
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].alive) continue;
        oamSet(&oamMain, id++, (enemies[i].x >> 8) - camX, enemies[i].y >> 8, 1, 0,
               SpriteSize_16x16, SpriteColorFormat_256Color,
               enemies[i].type == 1 ? gfx[GFX_TURRET] : gfx[GFX_SOLDIER],
               -1, false, false, enemies[i].facing > 0, false, false);
    }

    if (boss.active == 1) {
        oamSet(&oamMain, id++, boss.x - camX, boss.y, 1, 0,
               SpriteSize_32x32, SpriteColorFormat_256Color,
               bossGfx, -1, false, false, false, false, false);
        oamSet(&oamMain, id++, boss.x - camX - 6, boss.y - 6, 1, 0,
               SpriteSize_16x16, SpriteColorFormat_256Color,
               gfx[GFX_BOSS_TURRET], -1, false, false, false, false, false);
    }

    for (int i = 0; i < MAX_BULLETS; i++)
        if (bullets[i].alive && bullets[i].type == 0)  /* rockets drawn separately below */
            oamSet(&oamMain, id++, (bullets[i].x >> 8) - camX, bullets[i].y >> 8, 0, 0,
                   SpriteSize_16x16, SpriteColorFormat_256Color,
                   gfx[GFX_BULLET], -1, false, false, false, false, false);

    for (int i = 0; i < MAX_EBULLETS; i++)
        if (ebullets[i].alive)
            oamSet(&oamMain, id++, (ebullets[i].x >> 8) - camX, ebullets[i].y >> 8, 0, 0,
                   SpriteSize_16x16, SpriteColorFormat_256Color,
                   gfx[GFX_EBULLET], -1, false, false, false, false, false);

    for (int i = 0; i < MAX_GRENADES; i++)
        if (grenades[i].alive)
            oamSet(&oamMain, id++, (grenades[i].x >> 8) - camX, grenades[i].y >> 8, 0, 0,
                   SpriteSize_16x16, SpriteColorFormat_256Color,
                   gfx[GFX_GRENADE], -1, false, false, false, false, false);

    for (int i = 0; i < MAX_FX; i++)
        if (fx[i].alive)
            oamSet(&oamMain, id++, (fx[i].x >> 8) - camX, fx[i].y >> 8, 0, 0,
                   SpriteSize_16x16, SpriteColorFormat_256Color,
                   gfx[GFX_EXPLOSION], -1, false, false, fx[i].timer & 2, fx[i].timer & 4, false);

    /* Pickups: draw GFX_PICKUP, blink slowly using flash counter */
    for (int i = 0; i < MAX_PICKUPS; i++) {
        Pickup *p = &pickups[i];
        if (!p->alive) continue;
        int sx = p->x - camX;
        if (sx < -16 || sx > SCREEN_W + 16) continue;
        /* blink every 16 frames to attract attention */
        if ((p->flash >> 4) & 1) continue;
        /* use hflip to distinguish weapon types vs resource types */
        int hflip = (p->type >= PICKUP_GRENADE);
        oamSet(&oamMain, id++, sx, p->y, 0, 0,
               SpriteSize_16x16, SpriteColorFormat_256Color,
               gfx[GFX_PICKUP], -1, false, false, hflip, false, false);
    }

    /* Rocket projectiles */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].alive || bullets[i].type != 1) continue;
        int sx = (bullets[i].x >> 8) - camX;
        int sy = bullets[i].y >> 8;
        if (id >= 127) break;
        /* overwrite the generic bullet oamSet with rocket gfx */
        oamSet(&oamMain, id++, sx, sy, 0, 0,
               SpriteSize_16x16, SpriteColorFormat_256Color,
               gfx[GFX_ROCKET], -1, false, false,
               bullets[i].vx < 0, false, false);
    }
}

static void drawHud(void)
{
    consoleClear();
    iprintf("\x1b[0;0HSTEEL VANGUARD\n");
    iprintf("--------------------------------");
    iprintf("\x1b[3;0HSCORE   %08d", score);
    iprintf("\x1b[4;0HSTAGE   %d / %d", currentStage + 1, NUM_STAGES);
    iprintf("\x1b[5;0HLIVES   ");
    for (int i = 0; i < pl.lives; i++) iprintf("* ");
    iprintf("\x1b[7;0HGRENADE %d", pl.grenAmmo);

    /* Weapon & ammo display */
    static const char *weapNames[WEAPON_COUNT] = { "PISTOL", "HMG   ", "SPREAD", "ROCKET" };
    iprintf("\x1b[9;0HWEAPON  %s", weapNames[pl.weapon]);
    if (pl.weapon != WEAPON_DEFAULT) {
        iprintf("\x1b[10;0HAMMO    %d ", pl.weapAmmo);
    } else {
        iprintf("\x1b[10;0HAMMO    ---");
    }

    if (boss.active == 1) {
        iprintf("\x1b[12;0HBOSS    ");
        int bossMaxHp = stageBoss[currentStage].hp;
        int bars = (boss.hp * 20) / bossMaxHp;
        for (int i = 0; i < bars; i++) iprintf("|");
    }

    /* Pickup notification banner – centred, shown for NOTIFY_DURATION frames */
    if (gameState == 1 && pickupNotifyTimer > 0) {
        int len = (int)strlen(pickupNotifyText);
        int col = (32 - len) / 2;
        if (col < 0) col = 0;
        iprintf("\x1b[17;%dH>> %s <<", col, pickupNotifyText);
    }

    iprintf("\x1b[20;0HDPAD move  A/Y shoot");
    iprintf("\x1b[21;0HB jump  R grenade");

    if (gameState == 2) iprintf("\x1b[17;4H** MISSION FAILED **\n\x1b[19;3HPress START for menu");
    if (gameState == 3) iprintf("\x1b[16;3H** MISSION COMPLETE **\n\x1b[18;4HFINAL SCORE %08d\n\x1b[20;3HPress START for menu", score);
    if (gameState == 4) iprintf("\x1b[16;4H** STAGE %d CLEAR! **\n\x1b[18;4HSCORE %08d\n\x1b[20;4HPress START", currentStage, score);
}

/* ------------------------------------------------------------------ */
/* Start menu                                                          */
/* ------------------------------------------------------------------ */

static void menuEnter(void)
{
    gameState  = 0;
    menuScreen = MENU_MAIN;
    menuCursor = 0;
    bgmStop();
}

static void drawMenu(void)
{
    consoleClear();
    iprintf("\x1b[0;0H  ============================");
    iprintf("\x1b[1;0H       STEEL  VANGUARD");
    iprintf("\x1b[2;0H  ============================");

    switch (menuScreen) {

    case MENU_MAIN: {
        static const char *items[] = {
            "START GAME", "STAGE SELECT", "HISTORY", "OPTIONS"
        };
        for (int i = 0; i < 4; i++)
            iprintf("\x1b[%d;6H%s %s", 6 + i * 2,
                    (menuCursor == i) ? ">" : " ", items[i]);
        iprintf("\x1b[16;2HBEST %08d", (int)save.highScore[0]);
        if (!fatOk) iprintf("\x1b[18;2H(no save media: progress\n\x1b[19;3Hwon't persist)");
        iprintf("\x1b[21;2HUP/DOWN move   A select");
        break;
    }

    case MENU_STAGE: {
        iprintf("\x1b[4;4HSTAGE SELECT");
        static const char *names[NUM_STAGES] = {
            "ROLLING HILLS", "DESERT OUTPOST", "FACTORY FORTRESS"
        };
        for (int i = 0; i < NUM_STAGES; i++) {
            int locked = (i >= save.unlockedStages);
            iprintf("\x1b[%d;4H%s %d. %s %s", 7 + i * 2,
                    (menuCursor == i) ? ">" : " ",
                    i + 1,
                    locked ? "??????" : names[i],
                    locked ? "[LOCKED]" : "");
        }
        iprintf("\x1b[21;2HA start   B back");
        break;
    }

    case MENU_HISTORY: {
        iprintf("\x1b[4;4HHIGH SCORES");
        for (int i = 0; i < MAX_HISCORES; i++)
            iprintf("\x1b[%d;4H%d. %08d", 5 + i, i + 1, (int)save.highScore[i]);
        iprintf("\x1b[11;4HRECENT RUNS");
        if (save.historyCount == 0)
            iprintf("\x1b[12;4H(no runs yet)");
        for (int i = 0; i < save.historyCount && i < MAX_HISTORY; i++)
            iprintf("\x1b[%d;3H%08d ST%d %s", 12 + i,
                    (int)save.history[i].score,
                    save.history[i].stage,
                    save.history[i].result ? "CLEAR" : "K.I.A");
        iprintf("\x1b[21;2HB back");
        break;
    }

    case MENU_OPTIONS: {
        iprintf("\x1b[4;4HOPTIONS");
        iprintf("\x1b[7;4H%s SOUND        %s",
                (menuCursor == 0) ? ">" : " ", save.soundOn ? "[ON] " : "[OFF]");
        iprintf("\x1b[9;4H%s ERASE SAVE DATA",
                (menuCursor == 1) ? ">" : " ");
        iprintf("\x1b[13;4HSave media: %s", fatOk ? "OK" : "NOT FOUND");
        iprintf("\x1b[21;2HA toggle/apply   B back");
        break;
    }
    }
}

static void updateMenu(void)
{
    int down = keysDown();

    int nItems = 4;
    if (menuScreen == MENU_STAGE)   nItems = NUM_STAGES;
    if (menuScreen == MENU_OPTIONS) nItems = 2;
    if (menuScreen == MENU_HISTORY) nItems = 1;

    if (down & KEY_DOWN) menuCursor = (menuCursor + 1) % nItems;
    if (down & KEY_UP)   menuCursor = (menuCursor + nItems - 1) % nItems;

    if (down & KEY_B && menuScreen != MENU_MAIN) {
        menuScreen = MENU_MAIN;
        menuCursor = 0;
        return;
    }

    if (!(down & (KEY_A | KEY_START))) return;

    switch (menuScreen) {

    case MENU_MAIN:
        switch (menuCursor) {
            case 0: resetGame(0); return;
            case 1: menuScreen = MENU_STAGE;   menuCursor = 0; return;
            case 2: menuScreen = MENU_HISTORY; menuCursor = 0; return;
            case 3: menuScreen = MENU_OPTIONS; menuCursor = 0; return;
        }
        break;

    case MENU_STAGE:
        if (menuCursor < save.unlockedStages) resetGame(menuCursor);
        break;

    case MENU_HISTORY:
        break;

    case MENU_OPTIONS:
        if (menuCursor == 0) {
            save.soundOn = !save.soundOn;
            saveWrite();
        } else {
            int keepSound = save.soundOn;
            saveDefaults();
            save.soundOn = (u8)keepSound;
            saveWrite();
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);

    consoleDemoInit();   /* text HUD on the sub screen */

    oamInit(&oamMain, SpriteMapping_1D_128, false);

    for (int i = 0; i < GFX_COUNT; i++)
        gfx[i] = oamAllocateGfx(&oamMain, SpriteSize_16x16, SpriteColorFormat_256Color);
    bossGfx = oamAllocateGfx(&oamMain, SpriteSize_32x32, SpriteColorFormat_256Color);

    SPRITE_PALETTE[C_TRANS]      = RGB15(0, 0, 0);
    SPRITE_PALETTE[C_SKIN]       = RGB15(28, 20, 14);
    SPRITE_PALETTE[C_UNIFORM]    = RGB15(8, 14, 8);
    SPRITE_PALETTE[C_UNIFORM_DK] = RGB15(5, 9, 5);
    SPRITE_PALETTE[C_METAL]      = RGB15(16, 16, 18);
    SPRITE_PALETTE[C_METAL_DK]   = RGB15(9, 9, 12);
    SPRITE_PALETTE[C_FLAME]      = RGB15(31, 18, 2);
    SPRITE_PALETTE[C_FLAME2]     = RGB15(31, 28, 8);
    SPRITE_PALETTE[C_ENEMY]      = RGB15(16, 10, 6);
    SPRITE_PALETTE[C_ENEMY_DK]   = RGB15(10, 6, 4);
    SPRITE_PALETTE[C_TRACER]     = RGB15(31, 31, 20);
    SPRITE_PALETTE[C_SHADOW]     = RGB15(4, 4, 4);
    SPRITE_PALETTE[C_PICKUP]     = RGB15(26, 22, 6);
    SPRITE_PALETTE[C_WHITE]      = RGB15(31, 31, 31);

    buildArt();
    buildBackground();
    soundEnable();
    buildSounds();
    saveLoad();          /* init FAT + load history/options */

    menuEnter();

    while (1) {
        scanKeys();
        int down = keysDown();
        int wasPlaying = (gameState == 1);   /* playing before this frame's input */

        if (gameState == 0) {
            /* start menu */
            updateMenu();
        } else if (gameState == 1) {
            frame++;
            if (pickupNotifyTimer > 0) pickupNotifyTimer--;
            updatePlayer();
            spawnEnemies();
            updateEnemies();
            updateBoss();
            updateProjectiles();
            updatePickups();
            updateBgm();
        } else if (gameState == 4) {
            /* Stage-clear: wait for START then roll into next stage */
            if (down & KEY_START) advanceStage();
        } else {
            /* game-over (2), final victory (3) → back to menu */
            if (down & KEY_START) menuEnter();
        }
        /* pause only if START was pressed during gameplay - not the same
           press that just launched a game from the menu / stage-clear */
        if (wasPlaying && gameState == 1 && (down & KEY_START)) {
            /* pause: mute BGM while waiting, resume after */
            bgmStop();
            do { swiWaitForVBlank(); scanKeys(); } while (!(keysDown() & KEY_START));
            bgmStart();
        }

        if (gameState == 0) {
            /* menu: keep the battlefield backdrop, no sprites */
            oamClear(&oamMain, 0, 128);
            drawMenu();
        } else {
            draw();
            drawHud();
        }

        swiWaitForVBlank();
        oamUpdate(&oamMain);
    }

    return 0;
}
