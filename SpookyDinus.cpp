// ============================================================================
//  SPOOKY DINUS - Halloween Edition (v2)
//  Win32 + GDI+  |  single file
//
//  Build (MSVC):   cl /EHsc /O2 SpookyDinus.cpp /link /SUBSYSTEM:WINDOWS
//  Build (MinGW):  windres SpookyDinus.rc -O coff -o SpookyDinus_res.o
//                  g++ SpookyDinus.cpp SpookyDinus_res.o -o SpookyDinus.exe -mwindows -lgdiplus -lgdi32 -luser32 -lwinmm
//  (icon.ico must sit next to SpookyDinus.rc when running windres)
//
//  Put your images in an "assets" folder next to the .exe:
//      assets\dino.png   assets\ghost.png   assets\candy.png
//  (If an image is missing, a colored shape is drawn instead.)
//
//  Sounds: all sound effects and the background music are generated in code,
//  so no sound files are needed. (Needs -lwinmm.)
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#endif

using namespace Gdiplus;

// ----------------------------------------------------------------------------
//  Constants
// ----------------------------------------------------------------------------
static const int   WORLD_W      = 900;
static const int   WORLD_H      = 600;
static const float PLAY_TOP     = 50.0f;    // HUD height
static const int   MAX_LEVEL    = 5;
static const int   START_LIVES  = 5;
static const int   MAX_LIVES    = 5;
static const int   BOSS_HP      = 6;
static const float DINO_SPEED   = 250.0f;   // pixels per second
static const float SPEED_BOOST  = 1.6f;

enum class State { Title, Playing, Paused, GameOver, Win };
enum PowerType   { PU_SPEED, PU_SHIELD, PU_FREEZE };
enum CandyType   { CANDY_NORMAL, CANDY_GOLD, CANDY_HEART };

// ----------------------------------------------------------------------------
//  Data types
// ----------------------------------------------------------------------------
struct Box { float x, y, w, h; };

struct Ghost
{
    Box   b;
    int   type;       // 0 = chaser, 1 = wanderer, 2 = fast
    float speed;
    float tx, ty;     // wander target (center point)
    float wanderT;

    // boss only
    bool  boss     = false;
    int   hp       = 0;
    int   maxHp    = 0;
    float dashCd   = 0;   // time until next dash
    float dashWarn = 0;   // red warning flash before a dash
    float dashTime = 0;   // time left in the current dash
    float dashVX   = 0;
    float dashVY   = 0;
    float flash    = 0;   // white flash when hurt
};

struct PowerUp
{
    Box   b;
    int   type;
    float life;
};

struct Star { float x, y, phase; };

struct Game
{
    State state = State::Title;

    Box   dino   = { 60, 300, 120, 90 };
    Box   candy  = { 300, 200, 60, 60 };
    int   candyType = CANDY_NORMAL;
    float candyLife = 0;   // seconds left for gold / heart candies
    std::vector<Ghost> ghosts;
    std::vector<Box>   obstacles;

    bool    puActive = false;
    PowerUp pu       = { { 0, 0, 44, 44 }, 0, 0 };
    float   puSpawnT = 7.0f;

    int score       = 0;
    int highScore   = 0;
    int level       = 1;
    int lives       = START_LIVES;
    int candyCount  = 0;
    int candyGoal   = 6;

    float invuln  = 0;
    float speedT  = 0;
    float shieldT = 0;
    float freezeT = 0;
    float bannerT = 0;
    float time    = 0;

    // screen shake
    float shakeT = 0, shakeDur = 0.3f, shakeMag = 0;
    float shakeX = 0, shakeY = 0;

    std::vector<Star> stars;
};

// ----------------------------------------------------------------------------
//  Globals
// ----------------------------------------------------------------------------
static ULONG_PTR gdiplusToken = 0;
static Image*    dinoImg      = nullptr;
static Image*    ghostImg     = nullptr;
static Image*    candyImg     = nullptr;
static Bitmap*   backBuffer   = nullptr;
static HWND      g_hwnd       = NULL;
static Game      G;

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static float Rand01() { return rand() / (float)RAND_MAX; }

static float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

static float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Overlap test. A positive pad shrinks both boxes (forgiving hitboxes),
// a negative pad grows them (used for spacing).
static bool Overlap(const Box& a, const Box& b, float pad = 0.0f)
{
    return a.x + pad < b.x + b.w - pad &&
           a.x + a.w - pad > b.x + pad &&
           a.y + pad < b.y + b.h - pad &&
           a.y + a.h - pad > b.y + pad;
}

static bool KeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool HitsObstacle(const Box& b, float pad)
{
    for (const Box& o : G.obstacles)
        if (Overlap(b, o, pad))
            return true;
    return false;
}

// Start (or strengthen) a screen shake. mag = pixels, dur = seconds.
static void StartShake(float mag, float dur)
{
    if (G.shakeT > 0 && mag < G.shakeMag * (G.shakeT / G.shakeDur))
        return; // don't replace a stronger shake that is still running
    G.shakeMag = mag;
    G.shakeDur = dur;
    G.shakeT   = dur;
}

static Image* LoadImg(const wchar_t* path)
{
    Image* img = new Image(path);
    if (img->GetLastStatus() != Ok)
    {
        delete img;
        return nullptr;
    }
    return img;
}

static void LoadHighScore()
{
    FILE* f = fopen("highscore.txt", "r");
    if (f)
    {
        int v = 0;
        if (fscanf(f, "%d", &v) == 1)
            G.highScore = v;
        fclose(f);
    }
}

static void SaveHighScore()
{
    if (G.score > G.highScore)
    {
        G.highScore = G.score;
        FILE* f = fopen("highscore.txt", "w");
        if (f)
        {
            fprintf(f, "%d\n", G.highScore);
            fclose(f);
        }
    }
}

// ----------------------------------------------------------------------------
//  Sound: everything is synthesized in code - no sound files needed.
//  Effects play from memory; music is written to the temp folder and looped.
// ----------------------------------------------------------------------------
enum Sfx
{
    SFX_CANDY, SFX_HIT, SFX_SHIELD, SFX_POWERUP,
    SFX_LEVELUP, SFX_GAMEOVER, SFX_WIN,
    SFX_GOLD, SFX_HEART, SFX_BOSSHIT, SFX_COUNT
};

// Windows beeps used if something goes wrong
static const UINT SFX_FALLBACK[SFX_COUNT] =
{
    MB_ICONASTERISK, MB_ICONHAND, MB_ICONEXCLAMATION, MB_OK,
    MB_OK, MB_ICONHAND, MB_OK,
    MB_OK, MB_OK, MB_ICONHAND
};

static const int SYN_RATE = 22050;
enum Wave { W_SINE, W_SQUARE, W_SAW, W_TRI };

struct Mix
{
    std::vector<float> d;
    bool wrap;
    Mix(bool w = false, float seconds = 0.0f)
        : d((size_t)(seconds * SYN_RATE), 0.0f), wrap(w) {}
};

static std::vector<unsigned char> sfxData[SFX_COUNT];
static std::string musicPath;
static bool soundOn      = true;
static bool musicLoaded  = false;
static bool musicStarted = false;
static bool musicPlaying = false;

static float Hz(float midi)
{
    return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
}

static float WaveAt(int kind, double ph)
{
    const double TWO_PI = 6.283185307179586;
    double x = std::fmod(ph, TWO_PI) / TWO_PI; // 0..1
    switch (kind)
    {
        case W_SQUARE: return x < 0.5 ? 0.6f : -0.6f;
        case W_SAW:    return (float)(2.0 * x - 1.0);
        case W_TRI:    return (float)(4.0 * std::fabs(x - 0.5) - 1.0);
        default:       return (float)std::sin(ph);
    }
}

static void AddSample(Mix& m, int idx, float v)
{
    if (m.wrap)
        idx %= (int)m.d.size();       // tails wrap around = seamless loop
    else if (idx >= (int)m.d.size())
        m.d.resize(idx + 1, 0.0f);
    m.d[idx] += v;
}

static void AddTone(Mix& m, float start, float f0, float f1, float dur,
                    int kind, float vol, float decay)
{
    int n  = (int)(dur * SYN_RATE);
    int s0 = (int)(start * SYN_RATE);
    double ph = 0;
    for (int i = 0; i < n; i++)
    {
        float t = (float)i / SYN_RATE;
        float k = (float)i / n;
        float f = f0 + (f1 - f0) * k;
        ph += 6.283185307179586 * f / SYN_RATE;
        float e = (t < 0.005f ? t / 0.005f : 1.0f) * std::exp(-t * decay);
        AddSample(m, s0 + i, WaveAt(kind, ph) * e * vol);
    }
}

static void AddNoise(Mix& m, float start, float dur, float vol, float decay)
{
    int n  = (int)(dur * SYN_RATE);
    int s0 = (int)(start * SYN_RATE);
    for (int i = 0; i < n; i++)
    {
        float t = (float)i / SYN_RATE;
        float r = Rand01() * 2.0f - 1.0f;
        AddSample(m, s0 + i, r * std::exp(-t * decay) * vol);
    }
}

// Turn mixed float samples into a complete in-memory 16-bit mono WAV file
static std::vector<unsigned char> ToWav(const Mix& m, float peak)
{
    float mx = 0.0001f;
    for (float v : m.d)
        mx = std::fmax(mx, std::fabs(v));

    int n = (int)m.d.size();
    int dataBytes = n * 2;
    std::vector<unsigned char> w(44 + dataBytes);

    auto p32 = [&](int o, unsigned v) {
        w[o] = v & 255; w[o + 1] = (v >> 8) & 255;
        w[o + 2] = (v >> 16) & 255; w[o + 3] = (v >> 24) & 255;
    };
    auto p16 = [&](int o, unsigned v) {
        w[o] = v & 255; w[o + 1] = (v >> 8) & 255;
    };

    std::memcpy(&w[0],  "RIFF", 4);  p32(4, 36 + dataBytes);
    std::memcpy(&w[8],  "WAVE", 4);
    std::memcpy(&w[12], "fmt ", 4);  p32(16, 16);
    p16(20, 1);                      // PCM
    p16(22, 1);                      // mono
    p32(24, SYN_RATE);
    p32(28, SYN_RATE * 2);
    p16(32, 2);
    p16(34, 16);
    std::memcpy(&w[36], "data", 4);  p32(40, dataBytes);

    for (int i = 0; i < n; i++)
    {
        short v = (short)(m.d[i] / mx * peak * 32767.0f);
        p16(44 + i * 2, (unsigned short)v);
    }
    return w;
}

static void BuildSounds()
{
    { // candy: bright two-note blip
        Mix m;
        AddTone(m, 0.00f, Hz(84), Hz(84), 0.12f, W_SQUARE, 0.14f, 36);
        AddTone(m, 0.07f, Hz(91), Hz(91), 0.12f, W_SQUARE, 0.14f, 36);
        sfxData[SFX_CANDY] = ToWav(m, 0.85f);
    }
    { // hit: thud + wail + noise
        Mix m;
        AddTone(m, 0, 180, 50,  0.35f, W_SINE, 1.0f, 9);
        AddTone(m, 0, 700, 250, 0.35f, W_SINE, 0.4f, 7);
        AddNoise(m, 0, 0.35f, 0.5f, 30);
        sfxData[SFX_HIT] = ToWav(m, 0.85f);
    }
    { // shield: metallic ping
        Mix m;
        AddTone(m, 0, 1200, 1200, 0.45f, W_SINE, 1.0f, 9);
        AddTone(m, 0, 1800, 1800, 0.45f, W_SINE, 0.6f, 9);
        AddTone(m, 0, 2750, 2750, 0.45f, W_SINE, 0.4f, 9);
        sfxData[SFX_SHIELD] = ToWav(m, 0.85f);
    }
    { // powerup: rising arpeggio
        Mix m;
        const int notes[5] = { 72, 76, 79, 84, 88 };
        for (int i = 0; i < 5; i++)
            AddTone(m, i * 0.07f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.11f, W_SQUARE, 0.16f, 36);
        sfxData[SFX_POWERUP] = ToWav(m, 0.85f);
    }
    { // levelup: little fanfare
        Mix m;
        const int hi[5] = { 67, 72, 76, 79, 84 };
        for (int i = 0; i < 5; i++)
        {
            AddTone(m, i * 0.11f, Hz((float)hi[i]), Hz((float)hi[i]),
                    0.35f, W_TRI, 0.6f, 5);
            AddTone(m, i * 0.11f, Hz((float)hi[i] - 12), Hz((float)hi[i] - 12),
                    0.35f, W_TRI, 0.25f, 5);
        }
        sfxData[SFX_LEVELUP] = ToWav(m, 0.85f);
    }
    { // gameover: sad descending notes
        Mix m;
        const int notes[5] = { 69, 66, 62, 57, 50 };
        for (int i = 0; i < 5; i++)
            AddTone(m, i * 0.28f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.7f, W_TRI, 0.6f, 4.4f);
        sfxData[SFX_GAMEOVER] = ToWav(m, 0.85f);
    }
    { // win: major fanfare
        Mix m;
        const int a[8] = { 60, 64, 67, 72, 67, 72, 76, 84 };
        for (int i = 0; i < 8; i++)
        {
            AddTone(m, i * 0.13f, Hz((float)a[i]), Hz((float)a[i]),
                    0.21f, W_SQUARE, 0.3f, 13);
            AddTone(m, i * 0.13f, Hz((float)a[i] - 12), Hz((float)a[i] - 12),
                    0.21f, W_TRI, 0.3f, 10);
        }
        sfxData[SFX_WIN] = ToWav(m, 0.85f);
    }

    { // gold candy: sparkly arpeggio
        Mix m;
        const int notes[4] = { 88, 91, 95, 100 };
        for (int i = 0; i < 4; i++)
            AddTone(m, i * 0.05f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.18f, W_TRI, 0.35f, 22);
        sfxData[SFX_GOLD] = ToWav(m, 0.85f);
    }
    { // heart: warm rising chime
        Mix m;
        const int notes[3] = { 72, 79, 84 };
        for (int i = 0; i < 3; i++)
            AddTone(m, i * 0.12f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.45f, W_SINE, 0.6f, 7);
        sfxData[SFX_HEART] = ToWav(m, 0.85f);
    }
    { // boss hit: heavy boom
        Mix m;
        AddTone(m, 0, 120, 35,  0.5f, W_SINE, 1.0f, 6);
        AddTone(m, 0, 400, 100, 0.3f, W_SAW,  0.3f, 10);
        AddNoise(m, 0, 0.3f, 0.7f, 15);
        sfxData[SFX_BOSSHIT] = ToWav(m, 0.9f);
    }

    { // spooky looping music: Am - F - Dm - E, 4 seconds per chord
        struct Chord { int bass; int arp[8]; };
        const Chord prog[4] =
        {
            { 45, { 57, 60, 64, 60, 64, 69, 64, 60 } },
            { 41, { 53, 57, 60, 57, 60, 65, 60, 57 } },
            { 38, { 50, 53, 57, 53, 57, 62, 57, 53 } },
            { 40, { 52, 56, 59, 56, 59, 64, 59, 56 } }
        };
        const float chordLen = 4.0f;
        const double TWO_PI = 6.283185307179586;

        Mix m(true, chordLen * 4);
        int total = (int)m.d.size();

        for (int ci = 0; ci < 4; ci++)
        {
            float t0 = ci * chordLen;

            // bass drone
            AddTone(m, t0, Hz((float)prog[ci].bass), Hz((float)prog[ci].bass),
                    chordLen, W_SINE, 0.5f, 0.33f);

            // eerie pad: detuned triad with slow tremolo
            int n  = (int)(chordLen * SYN_RATE);
            int s0 = (int)(t0 * SYN_RATE);
            const int ks[3] = { prog[ci].arp[0], prog[ci].arp[2], prog[ci].arp[5] };
            for (int i = 0; i < n; i++)
            {
                double t = (double)i / SYN_RATE;
                double v = 0;
                for (int j = 0; j < 3; j++)
                {
                    double f = Hz((float)ks[j] - 12.0f);
                    v += std::sin(TWO_PI * f * t) + std::sin(TWO_PI * f * 1.006 * t);
                }
                v *= (0.5 + 0.5 * std::sin(TWO_PI * 0.5 * t + ci)) * 0.07;
                v *= std::fmin(1.0, t / 0.6) * std::fmin(1.0, (chordLen - t) / 0.6);
                m.d[(s0 + i) % total] += (float)v;
            }

            // music-box arpeggio (eighth notes)
            for (int i = 0; i < 8; i++)
            {
                float f = Hz((float)prog[ci].arp[i] + 12.0f);
                AddTone(m, t0 + i * 0.5f, f, f, 1.2f, W_TRI, 0.22f, 8);
            }
        }

        std::vector<unsigned char> wav = ToWav(m, 0.40f);

        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        musicPath = std::string(tmp) + "SpookyDinus_music.wav";

        FILE* f = fopen(musicPath.c_str(), "wb");
        if (f)
        {
            fwrite(wav.data(), 1, wav.size(), f);
            fclose(f);
        }
    }
}

static void InitSound()
{
    BuildSounds();

    if (!musicPath.empty())
    {
        std::string cmd = "open \"" + musicPath + "\" type waveaudio alias bgm";
        if (mciSendStringA(cmd.c_str(), NULL, 0, NULL) == 0)
            musicLoaded = true;
    }
}

static void ShutdownSound()
{
    PlaySoundA(NULL, NULL, 0);
    if (musicLoaded)
    {
        mciSendStringA("close bgm", NULL, 0, NULL);
        musicLoaded = false;
    }
    if (!musicPath.empty())
        DeleteFileA(musicPath.c_str());
}

static void PlaySfx(Sfx s)
{
    if (!soundOn)
        return;

    if (!sfxData[s].empty())
        PlaySoundA((LPCSTR)sfxData[s].data(), NULL,
                   SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    else
        MessageBeep(SFX_FALLBACK[s]);
}

// Music plays only while actually playing (not paused / not on menus / not muted)
static void SyncMusic()
{
    if (!musicLoaded)
        return;

    bool want = soundOn && G.state == State::Playing;
    if (want != musicPlaying)
    {
        if (want)
        {
            mciSendStringA(musicStarted ? "resume bgm" : "play bgm from 0",
                           NULL, 0, NULL);
            musicStarted = true;
        }
        else
        {
            mciSendStringA("pause bgm", NULL, 0, NULL);
        }
        musicPlaying = want;
    }

    // Loop: when the track ends, start it again
    if (musicPlaying)
    {
        static DWORD lastCheck = 0;
        DWORD now = GetTickCount();
        if (now - lastCheck > 250)
        {
            lastCheck = now;
            char mode[64] = { 0 };
            mciSendStringA("status bgm mode", mode, sizeof(mode), NULL);
            if (std::strcmp(mode, "stopped") == 0)
                mciSendStringA("play bgm from 0", NULL, 0, NULL);
        }
    }
}

// ----------------------------------------------------------------------------
//  Game setup
// ----------------------------------------------------------------------------
static void SpawnCandy(bool forceNormal = false)
{
    for (int i = 0; i < 200; i++)
    {
        Box c = { RandRange(20, WORLD_W - 80),
                  RandRange(PLAY_TOP + 10, WORLD_H - 80),
                  60, 60 };

        if (HitsObstacle(c, 0)) continue;
        if (Overlap(c, G.dino, -20)) continue;

        G.candy     = c;
        G.candyType = CANDY_NORMAL;
        G.candyLife = 0;

        if (!forceNormal)
        {
            float r = Rand01();
            if (r < 0.14f)
            {
                G.candyType = CANDY_GOLD;   // bonus points
                G.candyLife = 8.0f;
            }
            else if (r < 0.28f && G.lives < MAX_LIVES)
            {
                G.candyType = CANDY_HEART;  // extra life
                G.candyLife = 9.0f;
            }
        }
        return;
    }
    G.candy     = { WORLD_W / 2.0f, WORLD_H / 2.0f, 60, 60 };
    G.candyType = CANDY_NORMAL;
    G.candyLife = 0;
}

static Ghost MakeGhost(int type, float x, float y)
{
    Ghost g;
    g.type = type;

    float base = 80.0f + 10.0f * G.level;
    if (G.level == MAX_LEVEL)
        base *= 0.75f;   // the boss level has helpers, so keep them gentler
    g.speed = base * (type == 2 ? 1.3f : (type == 1 ? 0.9f : 1.0f));

    g.b = { x, y, 90, 90 };
    g.tx = x;
    g.ty = y;
    g.wanderT = 0;
    return g;
}

static void StartLevel()
{
    bool bossLevel = (G.level == MAX_LEVEL);

    G.dino.x = 60;
    G.dino.y = (WORLD_H + PLAY_TOP) / 2.0f - G.dino.h / 2.0f;

    G.candyCount = 0;
    G.candyGoal  = bossLevel ? BOSS_HP : 4 + G.level;
    G.invuln     = 1.5f;
    G.freezeT    = 0;
    G.bannerT    = bossLevel ? 2.5f : 2.0f;
    G.puActive   = false;
    G.puSpawnT   = 3.0f;

    // Tombstones
    G.obstacles.clear();
    int wanted = bossLevel ? 4 : 2 + G.level;
    for (int tries = 0; (int)G.obstacles.size() < wanted && tries < 500; tries++)
    {
        Box o = { RandRange(220, WORLD_W - 100),
                  RandRange(PLAY_TOP + 20, WORLD_H - 100),
                  50, 64 };
        bool bad = false;
        for (const Box& e : G.obstacles)
            if (Overlap(o, e, -30)) bad = true;
        if (!bad) G.obstacles.push_back(o);
    }

    // Ghosts
    G.ghosts.clear();
    if (bossLevel)
    {
        Ghost boss = MakeGhost(0, WORLD_W - 290.0f, (WORLD_H + PLAY_TOP) / 2.0f - 85.0f);
        boss.boss    = true;
        boss.b.w     = 170;
        boss.b.h     = 170;
        boss.speed   = 95.0f;
        boss.hp      = BOSS_HP;
        boss.maxHp   = BOSS_HP;
        boss.dashCd  = 4.0f;
        G.ghosts.push_back(boss);

        G.ghosts.push_back(MakeGhost(0, WORLD_W - 160.0f, PLAY_TOP + 50.0f));
        G.ghosts.push_back(MakeGhost(1, WORLD_W - 160.0f, WORLD_H - 150.0f));
    }
    else
    {
        int n = (G.level + 2) / 2;   // 1, 2, 2, 3 ghosts
        for (int i = 0; i < n; i++)
        {
            int type = (i == 1) ? 1 : (i == 2) ? 2 : 0;
            G.ghosts.push_back(MakeGhost(type, WORLD_W - 160.0f,
                Clampf(PLAY_TOP + 40.0f + i * 120.0f, PLAY_TOP, WORLD_H - 100.0f)));
        }
    }

    SpawnCandy();
}

static void ResetGame()
{
    G.score      = 0;
    G.level      = 1;
    G.lives      = START_LIVES;
    G.speedT     = 0;
    G.shieldT    = 0;
    G.freezeT    = 0;
    StartLevel();
    G.state = State::Playing;
}

static void EndGame(State s)
{
    SaveHighScore();
    G.state = s;
    PlaySfx(s == State::Win ? SFX_WIN : SFX_GAMEOVER);
}

// ----------------------------------------------------------------------------
//  Update
// ----------------------------------------------------------------------------
static void UpdateDino(float dt)
{
    if (GetForegroundWindow() != g_hwnd)
        return;

    float dx = 0, dy = 0;
    if (KeyDown(VK_LEFT)  || KeyDown('A')) dx -= 1;
    if (KeyDown(VK_RIGHT) || KeyDown('D')) dx += 1;
    if (KeyDown(VK_UP)    || KeyDown('W')) dy -= 1;
    if (KeyDown(VK_DOWN)  || KeyDown('S')) dy += 1;

    if (dx != 0 && dy != 0)
    {
        dx *= 0.7071f;
        dy *= 0.7071f;
    }

    float sp = DINO_SPEED * (G.speedT > 0 ? SPEED_BOOST : 1.0f);

    // Move one axis at a time so the dino slides along tombstones
    float oldX = G.dino.x;
    G.dino.x += dx * sp * dt;
    G.dino.x = Clampf(G.dino.x, 0, WORLD_W - G.dino.w);
    if (HitsObstacle(G.dino, 12)) G.dino.x = oldX;

    float oldY = G.dino.y;
    G.dino.y += dy * sp * dt;
    G.dino.y = Clampf(G.dino.y, PLAY_TOP, WORLD_H - G.dino.h);
    if (HitsObstacle(G.dino, 12)) G.dino.y = oldY;
}

static void UpdateBoss(Ghost& g, float dt, float dcx, float dcy)
{
    float cx = g.b.x + g.b.w / 2;
    float cy = g.b.y + g.b.h / 2;

    if (g.dashTime > 0)
    {
        // Charging in a straight line
        g.dashTime -= dt;
        g.b.x += g.dashVX * dt;
        g.b.y += g.dashVY * dt;
        if (g.dashTime <= 0)
            g.dashCd = 4.0f;
    }
    else if (g.dashWarn > 0)
    {
        // Flashing red: about to dash. Aim when the warning ends.
        g.dashWarn -= dt;
        if (g.dashWarn <= 0)
        {
            float vx = dcx - cx, vy = dcy - cy;
            float d = std::sqrt(vx * vx + vy * vy);
            if (d < 1.0f) d = 1.0f;
            g.dashVX = vx / d * 420.0f;
            g.dashVY = vy / d * 420.0f;
            g.dashTime = 0.6f;
            StartShake(7.0f, 0.25f);
        }
    }
    else
    {
        g.dashCd -= dt;
        if (g.dashCd <= 0)
        {
            g.dashWarn = 0.8f;
        }
        else
        {
            // Chase - gets faster as it gets hurt
            float speed = g.speed + (g.maxHp - g.hp) * 12.0f;
            float vx = dcx - cx, vy = dcy - cy;
            float d = std::sqrt(vx * vx + vy * vy);
            if (d > 1.0f)
            {
                float step = speed * dt;
                if (step > d) step = d;
                g.b.x += vx / d * step;
                g.b.y += vy / d * step;
            }
        }
    }

    g.b.x = Clampf(g.b.x, 0, WORLD_W - g.b.w);
    g.b.y = Clampf(g.b.y, PLAY_TOP, WORLD_H - g.b.h);
}

static void UpdateGhosts(float dt)
{
    if (G.freezeT > 0 || G.bannerT > 0)
        return;

    float dcx = G.dino.x + G.dino.w / 2;
    float dcy = G.dino.y + G.dino.h / 2;

    for (Ghost& g : G.ghosts)
    {
        if (g.boss)
        {
            UpdateBoss(g, dt, dcx, dcy);
            continue;
        }

        float cx = g.b.x + g.b.w / 2;
        float cy = g.b.y + g.b.h / 2;
        float tx = dcx, ty = dcy;

        if (g.type == 1) // wanderer: roams, sometimes locks onto the dino
        {
            g.wanderT -= dt;
            if (g.wanderT <= 0)
            {
                g.wanderT = 1.5f;
                if (Rand01() < 0.4f)
                {
                    g.tx = dcx;
                    g.ty = dcy;
                }
                else
                {
                    g.tx = RandRange(60, WORLD_W - 60);
                    g.ty = RandRange(PLAY_TOP + 60, WORLD_H - 60);
                }
            }
            tx = g.tx;
            ty = g.ty;
        }

        float vx = tx - cx;
        float vy = ty - cy;
        float d  = std::sqrt(vx * vx + vy * vy);
        if (d > 1.0f)
        {
            float step = g.speed * dt;
            if (step > d) step = d;
            g.b.x += vx / d * step;
            g.b.y += vy / d * step;
        }

        g.b.x = Clampf(g.b.x, 0, WORLD_W - g.b.w);
        g.b.y = Clampf(g.b.y, PLAY_TOP, WORLD_H - g.b.h);
    }
}

static void HitByGhost()
{
    if (G.shieldT > 0)
    {
        G.shieldT = 0;
        G.invuln  = 1.0f;
        StartShake(6.0f, 0.2f);
        PlaySfx(SFX_SHIELD);
        return;
    }

    G.lives--;
    G.invuln = 2.0f;
    StartShake(14.0f, 0.4f);
    PlaySfx(SFX_HIT);

    if (G.lives <= 0)
        EndGame(State::GameOver);
}

static void SpawnPowerUp()
{
    for (int i = 0; i < 200; i++)
    {
        Box b = { RandRange(30, WORLD_W - 80),
                  RandRange(PLAY_TOP + 20, WORLD_H - 80),
                  44, 44 };
        if (HitsObstacle(b, 0)) continue;
        if (Overlap(b, G.candy, -10)) continue;

        G.pu.b    = b;
        G.pu.type = rand() % 3;
        G.pu.life = 9.0f;
        G.puActive = true;
        return;
    }
}

static Ghost* FindBoss()
{
    for (Ghost& g : G.ghosts)
        if (g.boss)
            return &g;
    return nullptr;
}

static void CollectCandy()
{
    int type = G.candyType;
    bool bossLevel = (G.level == MAX_LEVEL);

    G.score += 10;
    G.candyCount++;

    if (type == CANDY_GOLD)
        G.score += 50;                       // gold bonus

    if (type == CANDY_HEART)
    {
        if (G.lives < MAX_LIVES) G.lives++;  // extra life
        else                     G.score += 100;
    }

    // Boss level: candy hurts the boss (gold hits twice)
    if (bossLevel)
    {
        Ghost* boss = FindBoss();
        if (boss)
        {
            boss->hp   -= (type == CANDY_GOLD) ? 2 : 1;
            boss->flash = 0.5f;
            StartShake(10.0f, 0.35f);

            if (boss->hp <= 0)
            {
                boss->hp = 0;
                G.score += 200;              // boss defeated bonus
                StartShake(18.0f, 0.6f);
                EndGame(State::Win);
                return;
            }
            PlaySfx(SFX_BOSSHIT);
        }
        SpawnCandy();
        return;
    }

    if (type == CANDY_GOLD)       PlaySfx(SFX_GOLD);
    else if (type == CANDY_HEART) PlaySfx(SFX_HEART);
    else                          PlaySfx(SFX_CANDY);

    if (G.candyCount >= G.candyGoal)
    {
        G.score += 50; // level bonus
        if (G.lives < MAX_LIVES) G.lives++;   // free life for clearing a level
        PlaySfx(SFX_LEVELUP);
        G.level++;
        StartLevel();
        return;
    }
    SpawnCandy();
}

static void Update(float dt)
{
    G.time += dt;

    // Screen shake fades out over its duration
    if (G.shakeT > 0)
    {
        G.shakeT -= dt;
        float amp = G.shakeT > 0 ? G.shakeMag * (G.shakeT / G.shakeDur) : 0.0f;
        G.shakeX = (Rand01() * 2.0f - 1.0f) * amp;
        G.shakeY = (Rand01() * 2.0f - 1.0f) * amp;
    }
    else
    {
        G.shakeX = G.shakeY = 0.0f;
    }

    if (G.state != State::Playing)
        return;

    // Timers
    if (G.invuln  > 0) G.invuln  -= dt;
    if (G.speedT  > 0) G.speedT  -= dt;
    if (G.shieldT > 0) G.shieldT -= dt;
    if (G.freezeT > 0) G.freezeT -= dt;
    if (G.bannerT > 0) G.bannerT -= dt;

    for (Ghost& g : G.ghosts)
        if (g.flash > 0) g.flash -= dt;

    // Gold / heart candy disappears if you take too long
    if (G.candyType != CANDY_NORMAL)
    {
        G.candyLife -= dt;
        if (G.candyLife <= 0)
            SpawnCandy(true);
    }

    UpdateDino(dt);
    UpdateGhosts(dt);

    // Ghost collisions
    if (G.invuln <= 0)
    {
        for (const Ghost& g : G.ghosts)
        {
            if (Overlap(G.dino, g.b, g.boss ? 40.0f : 30.0f))
            {
                HitByGhost();
                break;
            }
        }
    }
    if (G.state != State::Playing) return;

    // Candy
    if (Overlap(G.dino, G.candy, 10))
    {
        CollectCandy();
        if (G.state != State::Playing) return;
    }

    // Power-ups
    if (G.puActive)
    {
        G.pu.life -= dt;
        if (G.pu.life <= 0)
        {
            G.puActive = false;
            G.puSpawnT = 3.0f;
        }
        else if (Overlap(G.dino, G.pu.b, 6))
        {
            switch (G.pu.type)
            {
                case PU_SPEED:  G.speedT  = 5.0f; break;
                case PU_SHIELD: G.shieldT = 10.0f; break;
                case PU_FREEZE: G.freezeT = 4.0f; break;
            }
            PlaySfx(SFX_POWERUP);
            G.puActive = false;
            G.puSpawnT = 4.0f;
        }
    }
    else
    {
        G.puSpawnT -= dt;
        if (G.puSpawnT <= 0)
            SpawnPowerUp();
    }
}

// ----------------------------------------------------------------------------
//  Rendering helpers
// ----------------------------------------------------------------------------
static void Txt(Graphics& g, const std::wstring& s, float x, float y,
                float size, Color col, bool center = false, bool bold = true)
{
    FontFamily ff(L"Arial");
    Font font(&ff, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);

    StringFormat sf;
    RectF rect;
    if (center)
    {
        sf.SetAlignment(StringAlignmentCenter);
        rect = RectF(0, y, (REAL)WORLD_W, size * 2);
    }
    else
    {
        rect = RectF(x, y, (REAL)WORLD_W, size * 2);
    }

    // Drop shadow
    SolidBrush shadow(Color(160, 0, 0, 0));
    RectF sr = rect;
    sr.X += 2;
    sr.Y += 2;
    g.DrawString(s.c_str(), -1, &font, sr, &sf, &shadow);

    SolidBrush br(col);
    g.DrawString(s.c_str(), -1, &font, rect, &sf, &br);
}

static void DrawSprite(Graphics& g, Image* img, const Box& b, float alpha = 1.0f,
                       float rm = 1.0f, float gm = 1.0f, float bm = 1.0f)
{
    if (alpha >= 0.99f && rm == 1.0f && gm == 1.0f && bm == 1.0f)
    {
        g.DrawImage(img, b.x, b.y, b.w, b.h);
        return;
    }

    ColorMatrix cm = {
        rm, 0,  0,  0,     0,
        0,  gm, 0,  0,     0,
        0,  0,  bm, 0,     0,
        0,  0,  0,  alpha, 0,
        0,  0,  0,  0,     1
    };
    ImageAttributes ia;
    ia.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    g.DrawImage(img, RectF(b.x, b.y, b.w, b.h),
                0, 0, (REAL)img->GetWidth(), (REAL)img->GetHeight(),
                UnitPixel, &ia);
}

static void DrawBackground(Graphics& g)
{
    LinearGradientBrush bg(Point(0, 0), Point(0, WORLD_H),
                           Color(255, 45, 0, 70), Color(255, 10, 0, 25));
    bg.SetWrapMode(WrapModeTileFlipXY);
    g.FillRectangle(&bg, -40, -40, WORLD_W + 80, WORLD_H + 80);

    // Twinkling stars
    for (const Star& s : G.stars)
    {
        int a = (int)(130 + 100 * std::sin(G.time * 2.0f + s.phase));
        SolidBrush sb(Color((BYTE)a, 255, 255, 220));
        g.FillEllipse(&sb, s.x, s.y, 3.0f, 3.0f);
    }

    // Moon
    SolidBrush moon(Color(255, 255, 240, 170));
    g.FillEllipse(&moon, 760, 70, 80, 80);
    SolidBrush crater(Color(60, 180, 160, 80));
    g.FillEllipse(&crater, 780, 90, 20, 20);
    g.FillEllipse(&crater, 805, 115, 14, 14);
}

static void DrawTombstone(Graphics& g, const Box& o)
{
    SolidBrush stone(Color(255, 110, 110, 125));
    SolidBrush dark(Color(255, 60, 60, 75));

    g.FillRectangle(&stone, o.x, o.y + o.w / 2, o.w, o.h - o.w / 2);
    g.FillEllipse(&stone, o.x, o.y, o.w, o.w);
    g.FillRectangle(&dark, o.x - 6, o.y + o.h - 8, o.w + 12, 8.0f);

    FontFamily ff(L"Arial");
    Font f(&ff, 14, FontStyleBold, UnitPixel);
    SolidBrush t(Color(255, 40, 40, 55));
    g.DrawString(L"RIP", -1, &f, PointF(o.x + 10, o.y + 22), &t);
}

static void DrawPowerUp(Graphics& g, const PowerUp& p)
{
    Color c; const wchar_t* letter;
    switch (p.type)
    {
        case PU_SPEED:  c = Color(255, 255, 220, 0);   letter = L"S"; break;
        case PU_SHIELD: c = Color(255, 80, 160, 255);  letter = L"H"; break;
        default:        c = Color(255, 120, 255, 255); letter = L"F"; break;
    }

    // Blink when about to disappear
    if (p.life < 3.0f && std::fmod(p.life, 0.4f) < 0.2f)
        return;

    float pulse = 3.0f * std::sin(G.time * 6.0f);
    SolidBrush glow(Color(90, c.GetR(), c.GetG(), c.GetB()));
    g.FillEllipse(&glow, p.b.x - 6 - pulse, p.b.y - 6 - pulse,
                  p.b.w + 12 + 2 * pulse, p.b.h + 12 + 2 * pulse);
    SolidBrush body(c);
    g.FillEllipse(&body, p.b.x, p.b.y, p.b.w, p.b.h);

    FontFamily ff(L"Arial");
    Font f(&ff, 24, FontStyleBold, UnitPixel);
    SolidBrush t(Color(255, 20, 20, 40));
    g.DrawString(letter, -1, &f, PointF(p.b.x + 11, p.b.y + 8), &t);
}

static void DrawHeartShape(Graphics& g, float x, float y, float size, Color c)
{
    SolidBrush br(c);
    g.FillEllipse(&br, x, y + 0.02f * size, 0.5f * size, 0.5f * size);
    g.FillEllipse(&br, x + 0.5f * size, y + 0.02f * size, 0.5f * size, 0.5f * size);
    PointF pts[3] = {
        PointF(x + 0.03f * size, y + 0.37f * size),
        PointF(x + 0.97f * size, y + 0.37f * size),
        PointF(x + 0.50f * size, y + 0.97f * size)
    };
    g.FillPolygon(&br, pts, 3);
}

static void DrawCandy(Graphics& g)
{
    const Box& c = G.candy;
    float pulse = 3.0f * std::sin(G.time * 6.0f);

    // Special candies blink when they are about to disappear
    if (G.candyType != CANDY_NORMAL && G.candyLife < 3.0f &&
        std::fmod(G.candyLife, 0.4f) < 0.2f)
        return;

    if (G.candyType == CANDY_HEART)
    {
        SolidBrush glow(Color(70, 255, 80, 120));
        g.FillEllipse(&glow, c.x - 8 - pulse, c.y - 8 - pulse,
                      c.w + 16 + 2 * pulse, c.h + 16 + 2 * pulse);
        DrawHeartShape(g, c.x, c.y, c.w, Color(255, 235, 40, 80));
        return;
    }

    if (G.candyType == CANDY_GOLD)
    {
        SolidBrush glow(Color(80, 255, 220, 40));
        g.FillEllipse(&glow, c.x - 8 - pulse, c.y - 8 - pulse,
                      c.w + 16 + 2 * pulse, c.h + 16 + 2 * pulse);
        if (candyImg)
            DrawSprite(g, candyImg, c, 1.0f, 1.0f, 0.85f, 0.15f);   // gold tint
        else
        {
            SolidBrush b(Color(255, 255, 210, 30));
            g.FillEllipse(&b, c.x, c.y, c.w, c.h);
        }
        SolidBrush spark(Color(230, 255, 255, 255));
        g.FillEllipse(&spark, c.x + c.w - 12, c.y + 4, 8.0f, 8.0f);
        return;
    }

    if (candyImg)
        DrawSprite(g, candyImg, c);
    else
    {
        SolidBrush b(Color(255, 255, 140, 0));
        g.FillEllipse(&b, c.x, c.y, c.w, c.h);
    }
}

static void DrawHUD(Graphics& g)
{
    SolidBrush bar(Color(190, 0, 0, 0));
    g.FillRectangle(&bar, 0, 0, WORLD_W, (int)PLAY_TOP);

    Color orange(255, 255, 160, 0);

    Txt(g, L"SCORE: " + std::to_wstring(G.score), 15, 14, 20, orange);
    Txt(g, L"LEVEL " + std::to_wstring(G.level) + L"/" + std::to_wstring(MAX_LEVEL),
        165, 14, 20, Color(255, 200, 120, 255));

    if (G.level == MAX_LEVEL)
        Txt(g, L"BOSS FIGHT", 295, 14, 20, Color(255, 255, 70, 70));
    else
        Txt(g, L"CANDY " + std::to_wstring(G.candyCount) + L"/" + std::to_wstring(G.candyGoal),
            295, 14, 20, Color(255, 255, 120, 40));

    std::wstring fx;
    if (G.speedT  > 0) fx += L"SPEED ";
    if (G.shieldT > 0) fx += L"SHIELD ";
    if (G.freezeT > 0) fx += L"FREEZE ";
    if (!fx.empty())
        Txt(g, fx, 430, 15, 17, Color(255, 120, 255, 255));

    Txt(g, L"BEST: " + std::to_wstring(G.highScore), 640, 14, 18, Color(255, 200, 200, 200));

    // Lives as hearts
    for (int i = 0; i < G.lives; i++)
        DrawHeartShape(g, 755.0f + i * 26.0f, 14.0f, 22.0f, Color(255, 235, 40, 80));

    if (!soundOn)
        Txt(g, L"MUTED (M to unmute)", 15, WORLD_H - 28.0f, 16, Color(255, 200, 200, 200));

    // Boss health bar
    for (const Ghost& gh : G.ghosts)
    {
        if (!gh.boss) continue;

        float w = 320.0f;
        float x = (WORLD_W - w) / 2.0f;
        float y = PLAY_TOP + 12.0f;
        int hp = gh.hp > 0 ? gh.hp : 0;

        SolidBrush back(Color(200, 0, 0, 0));
        g.FillRectangle(&back, x - 3.0f, y - 3.0f, w + 6.0f, 20.0f);
        SolidBrush fill(Color(255, 220, 40, 60));
        g.FillRectangle(&fill, x, y, w * (float)hp / (float)gh.maxHp, 14.0f);
        Txt(g, L"BOSS", x - 62.0f, y - 4.0f, 18, Color(255, 255, 90, 90));
    }
}

static void DrawWorld(Graphics& g)
{
    // Tombstones
    for (const Box& o : G.obstacles)
        DrawTombstone(g, o);

    // Candy
    DrawCandy(g);

    // Power-up
    if (G.puActive)
        DrawPowerUp(g, G.pu);

    // Dino (blinks while invulnerable after a hit)
    bool showDino = !(G.invuln > 0 && std::fmod(G.invuln, 0.2f) < 0.1f);
    if (showDino)
    {
        if (dinoImg) DrawSprite(g, dinoImg, G.dino);
        else
        {
            SolidBrush b(Color(255, 60, 200, 80));
            g.FillEllipse(&b, G.dino.x, G.dino.y, G.dino.w, G.dino.h);
        }
    }

    if (G.shieldT > 0)
    {
        Pen p(Color(220, 80, 180, 255), 3.0f);
        g.DrawEllipse(&p, G.dino.x - 8, G.dino.y - 8, G.dino.w + 16, G.dino.h + 16);
    }

    // Ghosts
    for (const Ghost& gh : G.ghosts)
    {
        float alpha = 1.0f;
        if (gh.type == 1) alpha = 0.7f;
        if (G.freezeT > 0) alpha = 0.45f;

        float rm = 1.0f, gm = 1.0f, bm = 1.0f;
        if (gh.boss)
        {
            gm = 0.45f; bm = 0.55f;                       // angry red-purple
            if (gh.dashWarn > 0 && std::fmod(gh.dashWarn, 0.2f) < 0.1f)
            {
                gm = 0.1f; bm = 0.1f;                     // dash warning flash
            }
            if (gh.flash > 0)
            {
                rm = 2.0f; gm = 2.0f; bm = 2.0f;          // white flash when hurt
            }
        }

        if (ghostImg) DrawSprite(g, ghostImg, gh.b, alpha, rm, gm, bm);
        else
        {
            SolidBrush b(Color((BYTE)(alpha * 255),
                               gh.type == 2 ? 255 : 240, 240, 255));
            g.FillEllipse(&b, gh.b.x, gh.b.y, gh.b.w, gh.b.h);
        }

        if (G.freezeT > 0)
        {
            Pen ice(Color(200, 120, 255, 255), 2.0f);
            g.DrawEllipse(&ice, gh.b.x - 3, gh.b.y - 3, gh.b.w + 6, gh.b.h + 6);
        }

        if (gh.boss && gh.dashWarn > 0)
            Txt(g, L"!", gh.b.x + gh.b.w / 2 - 10, gh.b.y - 48, 48, Color(255, 255, 60, 60));
    }
}

static void Dim(Graphics& g)
{
    SolidBrush d(Color(165, 0, 0, 0));
    g.FillRectangle(&d, 0, 0, WORLD_W, WORLD_H);
}

static void DrawOverlays(Graphics& g)
{
    bool blink = std::fmod(G.time, 1.0f) < 0.6f;

    switch (G.state)
    {
        case State::Title:
        {
            Dim(g);
            Txt(g, L"SPOOKY DINUS", 0, 110, 72, Color(255, 255, 140, 0), true);
            Txt(g, L"HALLOWEEN EDITION", 0, 200, 30, Color(255, 190, 110, 255), true);

            Txt(g, L"Move:  Arrow keys or WASD", 0, 270, 20, Color(255, 230, 230, 230), true, false);
            Txt(g, L"Collect candy and dodge the ghosts. Defeat the boss on level 5!",
                0, 300, 20, Color(255, 230, 230, 230), true, false);
            Txt(g, L"Power-ups:  S = speed   H = shield   F = freeze ghosts",
                0, 330, 20, Color(255, 230, 230, 230), true, false);
            Txt(g, L"P / Esc = pause     R = restart     M = mute     F11 = fullscreen", 0, 360, 20,
                Color(255, 230, 230, 230), true, false);
            Txt(g, L"Gold candy = bonus points     Heart candy = extra life", 0, 390, 20,
                Color(255, 255, 210, 80), true, false);

            if (blink)
                Txt(g, L"Press ENTER to start", 0, 430, 32, Color(255, 255, 255, 255), true);

            Txt(g, L"BEST SCORE: " + std::to_wstring(G.highScore), 0, 500, 22,
                Color(255, 255, 200, 60), true);
            break;
        }

        case State::Playing:
        {
            if (G.bannerT > 0)
            {
                if (G.level == MAX_LEVEL)
                {
                    Txt(g, L"BOSS FIGHT!", 0, 235, 64, Color(255, 255, 70, 70), true);
                    Txt(g, L"Candy hurts the boss - gold hits twice! Watch for the red flash.",
                        0, 315, 22, Color(255, 255, 255, 255), true);
                }
                else
                {
                    Txt(g, L"LEVEL " + std::to_wstring(G.level), 0, 240, 64,
                        Color(255, 255, 160, 0), true);
                    Txt(g, L"Collect " + std::to_wstring(G.candyGoal) + L" candies",
                        0, 315, 24, Color(255, 255, 255, 255), true);
                }
            }
            break;
        }

        case State::Paused:
        {
            Dim(g);
            Txt(g, L"PAUSED", 0, 230, 64, Color(255, 255, 160, 0), true);
            Txt(g, L"P = resume     R = restart     Esc = title", 0, 320, 22,
                Color(255, 230, 230, 230), true, false);
            break;
        }

        case State::GameOver:
        {
            Dim(g);
            Txt(g, L"GAME OVER", 0, 190, 72, Color(255, 255, 40, 40), true);
            Txt(g, L"Score: " + std::to_wstring(G.score), 0, 290, 30,
                Color(255, 255, 160, 0), true);
            Txt(g, L"Best: " + std::to_wstring(G.highScore), 0, 330, 24,
                Color(255, 220, 220, 220), true);
            if (blink)
                Txt(g, L"Press R or ENTER to try again", 0, 410, 26,
                    Color(255, 255, 255, 255), true);
            break;
        }

        case State::Win:
        {
            Dim(g);
            Txt(g, L"YOU SURVIVED HALLOWEEN!", 0, 170, 58, Color(255, 255, 160, 0), true);
            Txt(g, L"Final score: " + std::to_wstring(G.score), 0, 270, 32,
                Color(255, 120, 255, 120), true);
            Txt(g, L"Best: " + std::to_wstring(G.highScore), 0, 315, 24,
                Color(255, 220, 220, 220), true);
            if (blink)
                Txt(g, L"Press R or ENTER to play again", 0, 400, 26,
                    Color(255, 255, 255, 255), true);
            break;
        }
    }
}

static void Render(HDC hdc)
{
    {
        Graphics g(backBuffer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        // World shakes; HUD and overlays stay steady
        g.TranslateTransform(G.shakeX, G.shakeY);
        DrawBackground(g);
        DrawWorld(g);
        g.ResetTransform();

        DrawHUD(g);
        DrawOverlays(g);
    }

    // One single blit to the window = no flicker
    Graphics screen(hdc);

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    if (cw == WORLD_W && ch == WORLD_H)
    {
        screen.DrawImage(backBuffer, 0, 0, WORLD_W, WORLD_H);
    }
    else if (cw > 0 && ch > 0)
    {
        // Fullscreen: scale up, keep the aspect ratio, black bars at the sides
        float sc = std::fmin((float)cw / WORLD_W, (float)ch / WORLD_H);
        float dw = WORLD_W * sc;
        float dh = WORLD_H * sc;
        float dx = (cw - dw) / 2.0f;
        float dy = (ch - dh) / 2.0f;

        screen.Clear(Color(255, 0, 0, 0));
        screen.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        screen.SetPixelOffsetMode(PixelOffsetModeHalf);
        screen.DrawImage(backBuffer, dx, dy, dw, dh);
    }
}

// ----------------------------------------------------------------------------
//  Window procedure
// ----------------------------------------------------------------------------
static bool            g_fullscreen = false;
static WINDOWPLACEMENT g_prevPlace  = {};
static LONG_PTR        g_prevStyle  = 0;

static void ToggleFullscreen()
{
    if (!g_fullscreen)
    {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        g_prevPlace.length = sizeof(g_prevPlace);

        if (!GetWindowPlacement(g_hwnd, &g_prevPlace) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;

        g_prevStyle = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_prevStyle & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    }
    else
    {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_prevStyle);
        SetWindowPlacement(g_hwnd, &g_prevPlace);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

static void OnKeyPress(WPARAM key)
{
    if (key == VK_F11)
    {
        ToggleFullscreen();
        return;
    }

    if (key == 'M')
    {
        soundOn = !soundOn;
        if (!soundOn)
            PlaySoundA(NULL, NULL, 0); // cut any playing effect
        return;
    }

    switch (G.state)
    {
        case State::Title:
            if (key == VK_RETURN || key == VK_SPACE)
                ResetGame();
            break;

        case State::Playing:
            if (key == 'P' || key == VK_ESCAPE)
                G.state = State::Paused;
            else if (key == 'R')
                ResetGame();
            else if (key == 'N' && G.level < MAX_LEVEL)   // skip level (handy for testing)
            {
                G.level++;
                StartLevel();
            }
            break;

        case State::Paused:
            if (key == 'P')
                G.state = State::Playing;
            else if (key == 'R')
                ResetGame();
            else if (key == VK_ESCAPE)
                G.state = State::Title;
            break;

        case State::GameOver:
        case State::Win:
            if (key == 'R' || key == VK_RETURN)
                ResetGame();
            else if (key == VK_ESCAPE)
                G.state = State::Title;
            break;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            srand((unsigned)time(NULL));

            GdiplusStartupInput startup;
            GdiplusStartup(&gdiplusToken, &startup, NULL);

            dinoImg  = LoadImg(L"assets\\dino.png");
            ghostImg = LoadImg(L"assets\\ghost.png");
            candyImg = LoadImg(L"assets\\candy.png");

            backBuffer = new Bitmap(WORLD_W, WORLD_H, PixelFormat32bppPARGB);

            for (int i = 0; i < 70; i++)
            {
                Star s = { RandRange(0, WORLD_W), RandRange(0, WORLD_H),
                           RandRange(0, 6.28f) };
                G.stars.push_back(s);
            }

            LoadHighScore();
            InitSound();
            return 0;
        }

        case WM_KEYDOWN:
        {
            if (lParam & 0x40000000) // ignore auto-repeat
                return 0;
            OnKeyPress(wParam);
            return 0;
        }

        case WM_KILLFOCUS:
        {
            if (G.state == State::Playing)
                G.state = State::Paused;
            return 0;
        }

        case WM_SETCURSOR:
        {
            if (g_fullscreen && LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(NULL);   // hide the mouse in fullscreen
                return TRUE;
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1; // we paint everything ourselves

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (backBuffer)
                Render(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
        {
            delete backBuffer; backBuffer = nullptr;
            delete dinoImg;    dinoImg    = nullptr;
            delete ghostImg;   ghostImg   = nullptr;
            delete candyImg;   candyImg   = nullptr;

            ShutdownSound();
            GdiplusShutdown(gdiplusToken);
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
//  Entry point with a real game loop (delta-time based)
// ----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    const wchar_t CLASS_NAME[] = L"SpookyDinus";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    // Icon comes from the resource compiled from SpookyDinus.rc (resource id 1).
    // If it isn't linked in, Windows just shows its default icon.
    wc.hIcon   = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON),
                                   GetSystemMetrics(SM_CYICON), LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wc);

    // Fixed-size window whose CLIENT area is exactly WORLD_W x WORLD_H
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT rc = { 0, 0, WORLD_W, WORLD_H };
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Spooky Dinus - Halloween Edition", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
        return 0;

    g_hwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    bool running = true;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        last = now;
        if (dt > 0.05f) dt = 0.05f; // avoid huge jumps after a stall

        Update(dt);
        SyncMusic();

        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);

        Sleep(1);
    }

    return 0;
}
