// ============================================================================
//  REINDEER RUSH - Christmas Edition
//  Win32 + GDI+  |  single file  |  no image or sound files needed
//
//  Build (MinGW):  windres ReindeerRush.rc -O coff -o ReindeerRush_res.o
//                  g++ ReindeerRush.cpp ReindeerRush_res.o -o ReindeerRush.exe -mwindows -lgdiplus -lgdi32 -luser32 -lwinmm
//  (icon.ico must sit next to ReindeerRush.rc when running windres)
//  Build (MSVC):   rc ReindeerRush.rc
//                  cl /EHsc /O2 ReindeerRush.cpp ReindeerRush.res /link /SUBSYSTEM:WINDOWS
//
//  Controls:  SPACE / UP / W = fly up (release to glide down)   S / DOWN = dive
//             P / Esc = pause   R = restart   M = mute   F11 = fullscreen
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
static const int   WORLD_W     = 900;
static const int   WORLD_H     = 600;
static const float PLAY_TOP    = 50.0f;    // HUD height
static const float GROUND_Y    = 520.0f;   // top of the snow
static const float PI_F        = 3.14159265f;
static const float RX          = 210.0f;   // reindeer's fixed x position
static const int   START_LIVES = 3;
static const int   MAX_LIVES   = 5;

enum class State { Title, Playing, Paused, GameOver };

// ----------------------------------------------------------------------------
//  Data
// ----------------------------------------------------------------------------
struct Box      { float x, y, w, h; };
struct Cane     { float x, y; bool gold; float phase; };
struct Icicle   { float x, y, w, len, vy, fallT; bool falling; };
struct Spirit   { float x, y, baseY, w, h, phase, amp, freq, extra; int type; };
struct Pickup   { float x, y; int type; };            // 0 = star, 1 = heart
struct Particle { float x, y, vx, vy, life, maxLife, size; BYTE r, g, b; bool gravity; };
struct Flake    { float x, y, size, depth, drift; };
struct Star     { float x, y, phase; };

struct Game
{
    State state = State::Title;

    float time  = 0;
    float scroll = 0;
    float speed = 60;
    float dist  = 0;           // meters

    float ry = 400, rvy = 0;   // reindeer center y and vertical speed
    bool  grounded = false;

    int   lives     = START_LIVES;
    int   caneScore = 0;
    int   combo     = 0;
    int   highScore = 0;

    float invuln  = 0;
    float starT   = 0;
    float spawnT  = 1.0f;
    float pickupT = 14.0f;
    float trailT  = 0;
    float msgT    = 0;
    std::wstring msg;
    int   nextMilestone = 500;

    float shakeT = 0, shakeDur = 0.3f, shakeMag = 0;
    float shakeX = 0, shakeY = 0;

    std::vector<Cane>     canes;
    std::vector<Icicle>   icicles;
    std::vector<Spirit>   spirits;
    std::vector<Pickup>   pickups;
    std::vector<Particle> parts;
    std::vector<Flake>    flakes;
    std::vector<Star>     stars;
};

// ----------------------------------------------------------------------------
//  Globals
// ----------------------------------------------------------------------------
static ULONG_PTR gdiplusToken = 0;
static Bitmap*   backBuffer   = nullptr;
static HWND      g_hwnd       = NULL;
static Game      G;

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------
static float Rand01() { return rand() / (float)RAND_MAX; }
static float RandRange(float a, float b) { return a + (b - a) * Rand01(); }
static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bool Overlap(const Box& a, const Box& b, float pad = 0.0f)
{
    return a.x + pad < b.x + b.w - pad &&
           a.x + a.w - pad > b.x + pad &&
           a.y + pad < b.y + b.h - pad &&
           a.y + a.h - pad > b.y + pad;
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static unsigned Hash(unsigned x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static int Score() { return G.caneScore + (int)G.dist; }

static int Multiplier()
{
    int m = 1 + G.combo / 8;
    return m > 5 ? 5 : m;
}

static void StartShake(float mag, float dur)
{
    if (G.shakeT > 0 && mag < G.shakeMag * (G.shakeT / G.shakeDur))
        return;
    G.shakeMag = mag;
    G.shakeDur = dur;
    G.shakeT   = dur;
}

static void LoadHighScore()
{
    FILE* f = fopen("reindeer_highscore.txt", "r");
    if (f)
    {
        int v = 0;
        if (fscanf(f, "%d", &v) == 1) G.highScore = v;
        fclose(f);
    }
}

static void SaveHighScore()
{
    if (Score() > G.highScore)
    {
        G.highScore = Score();
        FILE* f = fopen("reindeer_highscore.txt", "w");
        if (f)
        {
            fprintf(f, "%d\n", G.highScore);
            fclose(f);
        }
    }
}

// ----------------------------------------------------------------------------
//  Sound: everything is synthesized in code - no sound files needed.
// ----------------------------------------------------------------------------
enum Sfx
{
    SFX_CANE, SFX_GOLD, SFX_HIT, SFX_SHATTER, SFX_POOF,
    SFX_STAR, SFX_HEART, SFX_MILESTONE, SFX_GAMEOVER, SFX_COUNT
};

static const UINT SFX_FALLBACK[SFX_COUNT] =
{
    MB_ICONASTERISK, MB_OK, MB_ICONHAND, MB_ICONEXCLAMATION, MB_OK,
    MB_OK, MB_OK, MB_OK, MB_ICONHAND
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



// A soft bell: fundamental + two inharmonic overtones
static void Bell(Mix& m, float start, float midi, float dur, float vol)
{
    float f = Hz(midi);
    AddTone(m, start, f, f, dur, W_SINE, vol, 6.0f);
    AddTone(m, start, f * 2.0f, f * 2.0f, dur * 0.6f, W_SINE, vol * 0.25f, 10.0f);
    AddTone(m, start, f * 2.76f, f * 2.76f, dur * 0.4f, W_SINE, vol * 0.12f, 18.0f);
}

static void BuildSounds()
{
    { // candy cane: single bright bell
        Mix m;
        Bell(m, 0, 84, 0.45f, 0.8f);
        sfxData[SFX_CANE] = ToWav(m, 0.85f);
    }
    { // golden cane: sparkling bell arpeggio
        Mix m;
        const int off[4] = { 0, 4, 7, 12 };
        for (int i = 0; i < 4; i++)
            Bell(m, i * 0.06f, 84.0f + off[i], 0.5f, 0.6f);
        sfxData[SFX_GOLD] = ToWav(m, 0.85f);
    }
    { // hit: thud + wail + noise
        Mix m;
        AddTone(m, 0, 180, 50,  0.35f, W_SINE, 1.0f, 9);
        AddTone(m, 0, 700, 250, 0.35f, W_SINE, 0.4f, 7);
        AddNoise(m, 0, 0.35f, 0.5f, 30);
        sfxData[SFX_HIT] = ToWav(m, 0.85f);
    }
    { // icicle shatter: crunch + glass pings
        Mix m;
        AddNoise(m, 0, 0.25f, 0.8f, 18);
        AddTone(m, 0.00f, 2200, 2200, 0.3f, W_SINE, 0.3f, 25);
        AddTone(m, 0.03f, 3100, 3100, 0.3f, W_SINE, 0.25f, 25);
        AddTone(m, 0.06f, 3900, 3900, 0.3f, W_SINE, 0.2f, 25);
        sfxData[SFX_SHATTER] = ToWav(m, 0.8f);
    }
    { // ghost poof
        Mix m;
        AddNoise(m, 0, 0.3f, 0.6f, 10);
        AddTone(m, 0, 200, 700, 0.3f, W_SINE, 0.5f, 9);
        sfxData[SFX_POOF] = ToWav(m, 0.8f);
    }
    { // star power: rising chime run
        Mix m;
        const int notes[6] = { 72, 76, 79, 84, 88, 91 };
        for (int i = 0; i < 6; i++)
            AddTone(m, i * 0.06f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.25f, W_TRI, 0.35f, 8);
        sfxData[SFX_STAR] = ToWav(m, 0.85f);
    }
    { // heart: warm rising chime
        Mix m;
        const int notes[3] = { 72, 79, 84 };
        for (int i = 0; i < 3; i++)
            AddTone(m, i * 0.12f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.45f, W_SINE, 0.6f, 7);
        sfxData[SFX_HEART] = ToWav(m, 0.85f);
    }
    { // milestone: little fanfare
        Mix m;
        const int hi[5] = { 67, 72, 76, 79, 84 };
        for (int i = 0; i < 5; i++)
        {
            AddTone(m, i * 0.11f, Hz((float)hi[i]), Hz((float)hi[i]), 0.35f, W_TRI, 0.6f, 5);
            AddTone(m, i * 0.11f, Hz((float)hi[i] - 12), Hz((float)hi[i] - 12), 0.35f, W_TRI, 0.25f, 5);
        }
        sfxData[SFX_MILESTONE] = ToWav(m, 0.85f);
    }
    { // game over: sad descending notes
        Mix m;
        const int notes[5] = { 69, 66, 62, 57, 50 };
        for (int i = 0; i < 5; i++)
            AddTone(m, i * 0.28f, Hz((float)notes[i]), Hz((float)notes[i]),
                    0.7f, W_TRI, 0.6f, 4.4f);
        sfxData[SFX_GAMEOVER] = ToWav(m, 0.85f);
    }

    { // background music: "Jingle Bells" (public domain) with bells and sleigh-bell ticks
        const float beat = 0.4f;
        struct Note { int midi; float beats; };
        const Note mel[] =
        {
            { 64, 1 }, { 64, 1 }, { 64, 2 },
            { 64, 1 }, { 64, 1 }, { 64, 2 },
            { 64, 1 }, { 67, 1 }, { 60, 1.5f }, { 62, 0.5f },
            { 64, 4 },
            { 65, 1 }, { 65, 1 }, { 65, 1.5f }, { 65, 0.5f },
            { 65, 1 }, { 64, 1 }, { 64, 1 }, { 64, 0.5f }, { 64, 0.5f },
            { 64, 1 }, { 62, 1 }, { 62, 1 }, { 64, 1 },
            { 62, 2 }, { 67, 2 }
        };

        Mix m(true, 32 * beat);

        float t = 0;
        for (const Note& n : mel)
        {
            float d = n.beats * beat;
            Bell(m, t, (float)n.midi + 12.0f, d + 0.4f, 0.5f);
            t += d;
        }

        // bass: root on beat 1, fifth on beat 3
        const int roots[8]  = { 48, 48, 48, 48, 41, 48, 43, 43 };
        const int fifths[8] = { 55, 55, 55, 55, 48, 55, 50, 50 };
        for (int b = 0; b < 8; b++)
        {
            float t0 = b * 4 * beat;
            AddTone(m, t0,            Hz((float)roots[b]),  Hz((float)roots[b]),  1.4f, W_TRI, 0.35f, 3.0f);
            AddTone(m, t0 + 2 * beat, Hz((float)fifths[b]), Hz((float)fifths[b]), 1.4f, W_TRI, 0.30f, 3.0f);
        }

        // sleigh-bell ticks on every half beat
        for (int i = 0; i < 64; i++)
            AddNoise(m, i * 0.5f * beat, 0.05f, (i % 2 == 0) ? 0.05f : 0.03f, 50);

        std::vector<unsigned char> wav = ToWav(m, 0.40f);

        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        musicPath = std::string(tmp) + "ReindeerRush_music.wav";

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
//  Particles
// ----------------------------------------------------------------------------
static void Burst(float x, float y, int count, BYTE r, BYTE g, BYTE b,
                  float speed, float life, bool gravity, float size = 5.0f)
{
    for (int i = 0; i < count; i++)
    {
        float ang = Rand01() * 2.0f * PI_F;
        float sp  = speed * (0.3f + 0.7f * Rand01());
        Particle p;
        p.x = x; p.y = y;
        p.vx = std::cos(ang) * sp;
        p.vy = std::sin(ang) * sp;
        p.maxLife = p.life = life * (0.6f + 0.4f * Rand01());
        p.size = size * (0.6f + 0.8f * Rand01());
        p.r = r; p.g = g; p.b = b;
        p.gravity = gravity;
        G.parts.push_back(p);
    }
}

static void UpdateParticles(float dt)
{
    for (size_t i = 0; i < G.parts.size(); )
    {
        Particle& p = G.parts[i];
        p.life -= dt;
        if (p.life <= 0)
        {
            G.parts.erase(G.parts.begin() + i);
            continue;
        }
        if (p.gravity) p.vy += 700.0f * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        i++;
    }
}

// ----------------------------------------------------------------------------
//  Game logic
// ----------------------------------------------------------------------------
static void ResetGame()
{
    G.scroll = 0;
    G.speed  = 250;
    G.dist   = 0;
    G.ry     = 400;
    G.rvy    = 0;
    G.lives  = START_LIVES;
    G.caneScore = 0;
    G.combo  = 0;
    G.invuln = 1.0f;
    G.starT  = 0;
    G.spawnT = 1.0f;
    G.pickupT = 14.0f;
    G.msgT   = 0;
    G.nextMilestone = 500;
    G.canes.clear();
    G.icicles.clear();
    G.spirits.clear();
    G.pickups.clear();
    G.parts.clear();
    G.state = State::Playing;
}

static void EndGame()
{
    SaveHighScore();
    G.state = State::GameOver;
    PlaySfx(SFX_GAMEOVER);
    StartShake(18.0f, 0.6f);
}

static void HurtReindeer()
{
    G.lives--;
    G.combo  = 0;
    G.invuln = 2.0f;
    G.rvy    = -250.0f;
    StartShake(14.0f, 0.4f);
    PlaySfx(SFX_HIT);
    Burst(RX, G.ry, 16, 255, 255, 255, 240.0f, 0.6f, false);
    if (G.lives <= 0)
        EndGame();
}

static void SpawnCanes()
{
    int n = 5 + rand() % 3;
    float cy  = RandRange(PLAY_TOP + 110.0f, GROUND_Y - 90.0f);
    float amp = RandRange(30.0f, 80.0f) * ((rand() % 2) ? 1.0f : -1.0f);
    int goldIdx = (Rand01() < 0.15f) ? n / 2 : -1;

    for (int i = 0; i < n; i++)
    {
        Cane c;
        float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        c.x = WORLD_W + 40.0f + i * 46.0f;
        c.y = Clampf(cy + std::sin(t * PI_F) * amp, PLAY_TOP + 40.0f, GROUND_Y - 40.0f);
        c.gold  = (i == goldIdx);
        c.phase = i * 0.5f;
        G.canes.push_back(c);
    }
}

static void SpawnIcicles(float d)
{
    int n = 1;
    if (Rand01() < 0.35f + 0.4f * d) n++;
    if (Rand01() < 0.2f * d)         n++;

    float x = WORLD_W + 80.0f;
    for (int i = 0; i < n; i++)
    {
        Icicle ic;
        ic.x = x;
        ic.y = PLAY_TOP;
        ic.w = RandRange(26.0f, 38.0f);
        ic.len = RandRange(70.0f, 140.0f);
        ic.vy = 0;
        ic.fallT = RandRange(0.55f, 0.8f);
        ic.falling = false;
        G.icicles.push_back(ic);
        x += RandRange(60.0f, 100.0f);
    }
}

static void SpawnSpirit(float d)
{
    int type = rand() % 3;
    if (type == 2 && d < 0.15f) type = rand() % 2;

    Spirit s;
    s.type  = type;
    s.x     = WORLD_W + 80.0f;
    s.baseY = RandRange(PLAY_TOP + 90.0f, GROUND_Y - 80.0f);
    s.y     = s.baseY;
    s.phase = RandRange(0.0f, 6.0f);
    s.freq  = RandRange(1.5f, 3.0f);

    if (type == 0)      { s.w = 64;  s.h = 80;  s.amp = RandRange(40, 100); s.extra = 30.0f; }
    else if (type == 1) { s.w = 86;  s.h = 96;  s.amp = RandRange(30, 80);  s.extra = 15.0f; }
    else                { s.w = 90;  s.h = 110; s.amp = RandRange(10, 40);  s.extra = 110.0f + 80.0f * d; }

    G.spirits.push_back(s);
}

static void SpawnPickup()
{
    Pickup p;
    p.x = WORLD_W + 40.0f;
    p.y = RandRange(PLAY_TOP + 90.0f, GROUND_Y - 80.0f);
    p.type = (G.lives < MAX_LIVES && Rand01() < 0.45f) ? 1 : 0;
    G.pickups.push_back(p);
}

static void ShowMessage(const std::wstring& s, float secs)
{
    G.msg  = s;
    G.msgT = secs;
}

static void UpdatePlaying(float dt)
{
    // Difficulty ramps up with distance
    float d = Clampf(G.dist / 3500.0f, 0.0f, 1.0f);
    G.speed = 250.0f + 230.0f * d;
    G.scroll += G.speed * dt;
    G.dist = G.scroll / 20.0f;

    if (G.invuln > 0) G.invuln -= dt;
    if (G.starT  > 0) G.starT  -= dt;
    if (G.msgT   > 0) G.msgT   -= dt;

    // ---- reindeer physics ----
    bool up   = false, down = false;
    if (GetForegroundWindow() == g_hwnd)
    {
        up   = KeyDown(VK_SPACE) || KeyDown(VK_UP)   || KeyDown('W');
        down = KeyDown(VK_DOWN)  || KeyDown('S');
    }
    float ay = up ? -1500.0f : 1100.0f;
    if (down) ay += 900.0f;

    G.rvy += ay * dt;
    G.rvy = Clampf(G.rvy, -420.0f, 520.0f);
    G.ry += G.rvy * dt;

    float minY = PLAY_TOP + 34.0f;
    float maxY = GROUND_Y - 36.0f;
    G.grounded = false;
    if (G.ry < minY) { G.ry = minY; if (G.rvy < 0) G.rvy = 0; }
    if (G.ry > maxY) { G.ry = maxY; if (G.rvy > 0) G.rvy = 0; G.grounded = true; }

    // glowing nose trail
    G.trailT -= dt;
    if (G.trailT <= 0)
    {
        G.trailT = 0.04f;
        Particle p;
        p.x = RX + 66.0f; p.y = G.ry - 42.0f;
        p.vx = -G.speed * 0.6f; p.vy = RandRange(-20.0f, 20.0f);
        p.maxLife = p.life = 0.45f;
        p.size = RandRange(4.0f, 8.0f);
        if (G.starT > 0) { p.r = 255; p.g = 220; p.b = 60; }
        else             { p.r = 255; p.g = 70;  p.b = 70; }
        p.gravity = false;
        G.parts.push_back(p);
    }

    // ---- spawning ----
    G.spawnT -= dt;
    if (G.spawnT <= 0)
    {
        G.spawnT = Clampf(RandRange(1.0f, 1.5f) - 0.5f * d, 0.6f, 1.5f);
        float r = Rand01();
        if      (r < 0.38f) SpawnCanes();
        else if (r < 0.68f) SpawnIcicles(d);
        else                SpawnSpirit(d);
    }

    G.pickupT -= dt;
    if (G.pickupT <= 0)
    {
        G.pickupT = RandRange(14.0f, 22.0f);
        SpawnPickup();
    }

    // ---- collisions ----
    Box rb = { RX - 38.0f, G.ry - 24.0f, 76.0f, 46.0f };
    bool vulnerable = (G.invuln <= 0 && G.starT <= 0);

    // candy canes
    for (size_t i = 0; i < G.canes.size(); )
    {
        Cane& c = G.canes[i];
        c.x -= G.speed * dt;

        Box cb = { c.x - 14.0f, c.y - 30.0f, 34.0f, 58.0f };
        if (Overlap(rb, cb, 0.0f))
        {
            int mult = Multiplier();
            G.combo++;
            G.caneScore += (c.gold ? 50 : 10) * mult;
            PlaySfx(c.gold ? SFX_GOLD : SFX_CANE);
            if (c.gold) Burst(c.x, c.y, 16, 255, 215, 60, 260.0f, 0.6f, false);
            else        Burst(c.x, c.y, 8, 255, 255, 255, 180.0f, 0.4f, false, 4.0f);
            G.canes.erase(G.canes.begin() + i);
            continue;
        }
        if (c.x < -60.0f)
        {
            G.canes.erase(G.canes.begin() + i);
            continue;
        }
        i++;
    }

    // icicles
    for (size_t i = 0; i < G.icicles.size(); )
    {
        Icicle& ic = G.icicles[i];
        ic.x -= G.speed * dt;

        float dx = ic.x - RX;
        if (!ic.falling && dx < G.speed * ic.fallT)
            ic.falling = true;

        if (ic.falling)
        {
            ic.vy += 1300.0f * dt;
            ic.y  += ic.vy * dt;
        }

        bool shatter = false;
        if (ic.y + ic.len >= GROUND_Y)
            shatter = true;

        Box ib = { ic.x - ic.w * 0.28f, ic.y, ic.w * 0.56f, ic.len * 0.92f };
        if (!shatter && Overlap(rb, ib, 4.0f))
        {
            if (G.starT > 0)
            {
                G.caneScore += 10 * Multiplier();
                shatter = true;
            }
            else if (vulnerable)
            {
                HurtReindeer();
                vulnerable = false;
                shatter = true;
            }
        }

        if (shatter)
        {
            Burst(ic.x, ic.y + ic.len * 0.8f, 14, 200, 235, 255, 300.0f, 0.7f, true);
            if (G.state == State::Playing) PlaySfx(SFX_SHATTER);
            if (ic.y + ic.len >= GROUND_Y) StartShake(3.0f, 0.15f);
            G.icicles.erase(G.icicles.begin() + i);
            if (G.state != State::Playing) return;
            continue;
        }
        if (ic.x < -80.0f)
        {
            G.icicles.erase(G.icicles.begin() + i);
            continue;
        }
        i++;
    }

    // ghosts of Christmas
    for (size_t i = 0; i < G.spirits.size(); )
    {
        Spirit& s = G.spirits[i];
        s.x -= (G.speed + s.extra) * dt;
        s.phase += s.freq * dt;

        if (s.type == 2 && s.x - RX > 160.0f)       // Future drifts toward you
            s.baseY += (G.ry - s.baseY) * 0.9f * dt;

        s.y = Clampf(s.baseY + std::sin(s.phase) * s.amp, PLAY_TOP + s.h * 0.5f, GROUND_Y - s.h * 0.3f);

        Box sb = { s.x - s.w * 0.35f, s.y - s.h * 0.4f, s.w * 0.7f, s.h * 0.8f };
        if (Overlap(rb, sb, 6.0f))
        {
            if (G.starT > 0)
            {
                G.caneScore += 25 * Multiplier();
                Burst(s.x, s.y, 18, 255, 255, 255, 280.0f, 0.7f, false);
                PlaySfx(SFX_POOF);
                G.spirits.erase(G.spirits.begin() + i);
                continue;
            }
            if (vulnerable)
            {
                HurtReindeer();
                vulnerable = false;
                Burst(s.x, s.y, 14, 255, 255, 255, 240.0f, 0.6f, false);
                G.spirits.erase(G.spirits.begin() + i);
                if (G.state != State::Playing) return;
                continue;
            }
        }
        if (s.x < -120.0f)
        {
            G.spirits.erase(G.spirits.begin() + i);
            continue;
        }
        i++;
    }

    // pickups
    for (size_t i = 0; i < G.pickups.size(); )
    {
        Pickup& p = G.pickups[i];
        p.x -= G.speed * dt;

        Box pb = { p.x - 22.0f, p.y - 22.0f, 44.0f, 44.0f };
        if (Overlap(rb, pb, 0.0f))
        {
            if (p.type == 0)
            {
                G.starT = 6.0f;
                ShowMessage(L"UNSTOPPABLE!", 1.5f);
                PlaySfx(SFX_STAR);
                Burst(p.x, p.y, 22, 255, 220, 60, 300.0f, 0.8f, false);
            }
            else
            {
                if (G.lives < MAX_LIVES) G.lives++;
                else                     G.caneScore += 100;
                ShowMessage(L"+1 LIFE", 1.2f);
                PlaySfx(SFX_HEART);
                Burst(p.x, p.y, 16, 255, 80, 120, 240.0f, 0.7f, false);
            }
            G.pickups.erase(G.pickups.begin() + i);
            continue;
        }
        if (p.x < -60.0f)
        {
            G.pickups.erase(G.pickups.begin() + i);
            continue;
        }
        i++;
    }

    // distance milestones
    if (G.dist >= G.nextMilestone)
    {
        ShowMessage(std::to_wstring(G.nextMilestone) + L" m!", 1.5f);
        G.caneScore += 50;
        PlaySfx(SFX_MILESTONE);
        G.nextMilestone += 500;
    }
}

static void UpdateFlakes(float dt)
{
    float wind = G.speed > 40.0f ? G.speed : 40.0f;
    for (Flake& f : G.flakes)
    {
        f.y += (35.0f + 60.0f * f.depth) * dt;
        f.x -= (wind * 0.5f * f.depth + f.drift) * dt;
        if (f.y > WORLD_H + 5)
        {
            f.y = -5.0f;
            f.x = RandRange(0.0f, (float)WORLD_W);
        }
        if (f.x < -10.0f)
            f.x = WORLD_W + 10.0f;
    }
}

static void Update(float dt)
{
    G.time += dt;

    // screen shake
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

    if (G.state == State::Paused)
        return;

    UpdateFlakes(dt);
    UpdateParticles(dt);

    switch (G.state)
    {
        case State::Title:
            G.speed   = 60.0f;
            G.scroll += 60.0f * dt;
            G.ry      = 330.0f + std::sin(G.time * 2.0f) * 18.0f;
            G.rvy     = std::cos(G.time * 2.0f) * 36.0f;
            G.grounded = false;
            break;

        case State::Playing:
            UpdatePlaying(dt);
            break;

        case State::GameOver:
            G.speed = 0;
            break;

        default:
            break;
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

    SolidBrush shadow(Color((BYTE)(col.GetA() * 160 / 255), 0, 0, 0));
    RectF sr = rect;
    sr.X += 2;
    sr.Y += 2;
    g.DrawString(s.c_str(), -1, &font, sr, &sf, &shadow);

    SolidBrush br(col);
    g.DrawString(s.c_str(), -1, &font, rect, &sf, &br);
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

static void DrawStarShape(Graphics& g, float cx, float cy, float r, Color c)
{
    PointF pts[10];
    for (int i = 0; i < 10; i++)
    {
        float ang = -PI_F / 2.0f + i * PI_F / 5.0f;
        float rr  = (i % 2 == 0) ? r : r * 0.45f;
        pts[i] = PointF(cx + std::cos(ang) * rr, cy + std::sin(ang) * rr);
    }
    SolidBrush br(c);
    g.FillPolygon(&br, pts, 10);
}

// ----------------------------------------------------------------------------
//  Background
// ----------------------------------------------------------------------------
static void DrawMountains(Graphics& g, float par, float baseY, float amp, Color col, float seed)
{
    std::vector<PointF> pts;
    float off = G.scroll * par;
    pts.push_back(PointF(-40.0f, GROUND_Y));
    for (int x = -40; x <= WORLD_W + 40; x += 20)
    {
        float X = x + off;
        float h = amp * (0.55f + 0.30f * std::sin(X * 0.006f + seed)
                               + 0.15f * std::sin(X * 0.017f + seed * 2.0f));
        pts.push_back(PointF((float)x, baseY - h));
    }
    pts.push_back(PointF((float)WORLD_W + 40.0f, GROUND_Y));
    SolidBrush br(col);
    g.FillPolygon(&br, pts.data(), (INT)pts.size());
}

static void DrawTree(Graphics& g, float cx, float base, float h, bool lights, unsigned seed)
{
    SolidBrush trunk(Color(255, 50, 35, 30));
    g.FillRectangle(&trunk, cx - 4.0f, base - h * 0.14f, 8.0f, h * 0.14f);

    SolidBrush leaf(Color(255, 18, 64, 52));
    SolidBrush snow(Color(255, 225, 236, 250));
    for (int k = 0; k < 3; k++)
    {
        float tw = h * (0.55f - k * 0.14f);
        float ty = base - h * 0.12f - k * h * 0.27f;
        float th = h * 0.38f;
        PointF p[3] = { PointF(cx - tw / 2, ty), PointF(cx + tw / 2, ty), PointF(cx, ty - th) };
        g.FillPolygon(&leaf, p, 3);
        PointF s[3] = { PointF(cx - tw * 0.18f, ty - th * 0.62f),
                        PointF(cx + tw * 0.18f, ty - th * 0.62f),
                        PointF(cx, ty - th) };
        g.FillPolygon(&snow, s, 3);
    }

    if (lights)
    {
        for (int j = 0; j < 7; j++)
        {
            float lx = cx + std::sin(j * 2.3f + (float)(seed & 255)) * h * 0.16f;
            float ly = base - h * (0.2f + j * 0.12f);
            int a = (int)(170 + 85 * std::sin(G.time * 3.0f + j + (float)(seed & 15)));
            Color cols[4] = { Color((BYTE)a, 255, 70, 70), Color((BYTE)a, 255, 220, 80),
                              Color((BYTE)a, 90, 220, 255), Color((BYTE)a, 120, 255, 120) };
            SolidBrush b(cols[j % 4]);
            g.FillEllipse(&b, lx - 2.5f, ly - 2.5f, 5.0f, 5.0f);
        }
    }
}

static void DrawHouse(Graphics& g, float px, float base, float w, float h, unsigned seed)
{
    SolidBrush wall(Color(255, 28, 32, 62));
    g.FillRectangle(&wall, px, base - h, w, h);

    SolidBrush chim(Color(255, 40, 40, 72));
    g.FillRectangle(&chim, px + w * 0.7f, base - h - 38.0f, 14.0f, 36.0f);
    SolidBrush chimSnow(Color(255, 230, 238, 250));
    g.FillRectangle(&chimSnow, px + w * 0.7f - 2.0f, base - h - 42.0f, 18.0f, 6.0f);

    SolidBrush roof(Color(255, 215, 228, 246));
    PointF rp[3] = { PointF(px - 8.0f, base - h), PointF(px + w + 8.0f, base - h),
                     PointF(px + w / 2, base - h - 40.0f) };
    g.FillPolygon(&roof, rp, 3);

    bool lit1 = (seed & 1) != 0;
    SolidBrush winOn(Color(255, 255, 214, 110));
    SolidBrush winOff(Color(255, 50, 56, 90));
    g.FillRectangle(lit1 ? &winOn : &winOff, px + w * 0.15f, base - h * 0.7f, 16.0f, 18.0f);
    g.FillRectangle(&winOn, px + w * 0.5f, base - h * 0.7f, 16.0f, 18.0f);
    g.FillRectangle(&winOff, px + w * 0.38f, base - h * 0.35f, 14.0f, h * 0.35f);
}

static void DrawBackground(Graphics& g)
{
    // sky
    LinearGradientBrush sky(Point(0, 0), Point(0, (int)GROUND_Y),
                            Color(255, 8, 10, 40), Color(255, 38, 86, 128));
    sky.SetWrapMode(WrapModeTileFlipXY);
    g.FillRectangle(&sky, -40, -40, WORLD_W + 80, (int)GROUND_Y + 40);

    // twinkling stars
    for (const Star& s : G.stars)
    {
        int a = (int)(140 + 100 * std::sin(G.time * 2.0f + s.phase));
        SolidBrush sb(Color((BYTE)a, 255, 255, 225));
        g.FillEllipse(&sb, s.x, s.y, 3.0f, 3.0f);
    }

    // moon with glow
    for (int k = 3; k >= 1; k--)
    {
        SolidBrush glow(Color(22, 230, 240, 255));
        float r = 52.0f + k * 14.0f;
        g.FillEllipse(&glow, 740.0f - r, 95.0f - r, r * 2.0f, r * 2.0f);
    }
    SolidBrush moon(Color(255, 240, 244, 255));
    g.FillEllipse(&moon, 688.0f, 43.0f, 104.0f, 104.0f);
    SolidBrush crater(Color(60, 170, 185, 215));
    g.FillEllipse(&crater, 715.0f, 70.0f, 22.0f, 22.0f);
    g.FillEllipse(&crater, 750.0f, 100.0f, 15.0f, 15.0f);

    // aurora ribbons
    for (int band = 0; band < 2; band++)
    {
        std::vector<PointF> pts;
        std::vector<PointF> low;
        for (int x = -40; x <= WORLD_W + 40; x += 30)
        {
            float top = 130.0f + band * 40.0f + 26.0f * std::sin(x * 0.009f + G.time * 0.4f + band * 2.0f);
            float bot = top + 55.0f + 15.0f * std::sin(x * 0.013f + G.time * 0.3f);
            pts.push_back(PointF((float)x, top));
            low.push_back(PointF((float)x, bot));
        }
        for (int i = (int)low.size() - 1; i >= 0; i--)
            pts.push_back(low[i]);
        SolidBrush br(band == 0 ? Color(34, 60, 255, 160) : Color(30, 120, 160, 255));
        g.FillPolygon(&br, pts.data(), (INT)pts.size());
    }

    // far and near mountains
    DrawMountains(g, 0.08f, GROUND_Y - 30.0f, 200.0f, Color(255, 42, 66, 112), 0.5f);
    DrawMountains(g, 0.18f, GROUND_Y - 10.0f, 130.0f, Color(255, 30, 52, 92), 2.3f);

    // town: houses and trees
    float off = G.scroll * 0.4f;
    int first = (int)std::floor(off / 150.0f) - 1;
    for (int i = first; i < first + 8; i++)
    {
        float px = i * 150.0f - off;
        unsigned hs = Hash((unsigned)(i + 1000));
        int kind = (int)(hs % 4);
        float jitter = (float)((hs >> 8) % 40);
        if (kind < 2)
            DrawHouse(g, px + jitter * 0.5f, GROUND_Y + 2.0f,
                      80.0f + (float)((hs >> 12) % 40), 60.0f + (float)((hs >> 16) % 30), hs);
        else
            DrawTree(g, px + 60.0f + jitter * 0.5f, GROUND_Y + 2.0f,
                     90.0f + (float)((hs >> 12) % 50), kind == 3, hs);
    }

    // snowy ground
    LinearGradientBrush ground(Point(0, (int)GROUND_Y), Point(0, WORLD_H),
                               Color(255, 238, 245, 255), Color(255, 165, 190, 228));
    ground.SetWrapMode(WrapModeTileFlipXY);
    g.FillRectangle(&ground, -40, (int)GROUND_Y, WORLD_W + 80, WORLD_H - (int)GROUND_Y + 40);

    SolidBrush edge(Color(255, 245, 250, 255));
    for (int k = -1; k < 14; k++)
    {
        float x = k * 70.0f - std::fmod(G.scroll, 70.0f);
        g.FillEllipse(&edge, x - 10.0f, GROUND_Y - 9.0f, 90.0f, 22.0f);
    }
    SolidBrush shade(Color(70, 140, 165, 215));
    int base = (int)std::floor(G.scroll / 110.0f);
    for (int k = -1; k < 10; k++)
    {
        int idx = base + k;
        unsigned hs = Hash((unsigned)(idx + 5000));
        float x = idx * 110.0f - G.scroll;
        float w = 50.0f + (float)(hs % 50);
        g.FillEllipse(&shade, x, GROUND_Y + 28.0f + (float)((hs >> 8) % 30), w, 12.0f);
    }

    // string of Christmas lights along the snow
    Pen wire(Color(255, 30, 40, 30), 2.0f);
    int lb = (int)std::floor(G.scroll / 40.0f);
    PointF prev(-50.0f, GROUND_Y + 14.0f);
    for (int k = -1; k < 25; k++)
    {
        int idx = lb + k;
        float x = idx * 40.0f - G.scroll;
        float y = GROUND_Y + 18.0f + 6.0f * std::sin(x * 0.08f);
        g.DrawLine(&wire, prev.X, prev.Y, x, y);
        prev = PointF(x, y);

        int a = (int)(190 + 65 * std::sin(G.time * 4.0f + idx * 1.7f));
        Color cols[4] = { Color((BYTE)a, 255, 60, 60), Color((BYTE)a, 255, 215, 70),
                          Color((BYTE)a, 80, 210, 255), Color((BYTE)a, 110, 245, 110) };
        SolidBrush bulbGlow(Color(50, cols[((idx % 4) + 4) % 4].GetR(),
                                      cols[((idx % 4) + 4) % 4].GetG(),
                                      cols[((idx % 4) + 4) % 4].GetB()));
        g.FillEllipse(&bulbGlow, x - 9.0f, y - 4.0f, 18.0f, 18.0f);
        SolidBrush bulb(cols[((idx % 4) + 4) % 4]);
        g.FillEllipse(&bulb, x - 4.0f, y, 8.0f, 11.0f);
    }
}

// ----------------------------------------------------------------------------
//  Entities
// ----------------------------------------------------------------------------
static void DrawCane(Graphics& g, const Cane& c)
{
    float bob = std::sin(G.time * 4.0f + c.phase) * 3.0f;
    float cx = c.x;
    float cy = c.y + bob;

    Color body  = c.gold ? Color(255, 255, 240, 170) : Color(255, 255, 255, 255);
    Color stripe = c.gold ? Color(255, 255, 185, 20)  : Color(255, 215, 25, 45);

    if (c.gold)
    {
        SolidBrush glow(Color(70, 255, 220, 60));
        float pulse = 4.0f * std::sin(G.time * 6.0f + c.phase);
        g.FillEllipse(&glow, cx - 24.0f - pulse, cy - 36.0f - pulse, 58.0f + 2.0f * pulse, 72.0f + 2.0f * pulse);
    }

    // stem
    Pen white(body, 12.0f);
    white.SetStartCap(LineCapRound);
    white.SetEndCap(LineCapRound);
    g.DrawLine(&white, cx, cy + 26.0f, cx, cy - 6.0f);

    SolidBrush red(stripe);
    for (int k = 0; k < 5; k++)
    {
        float y0 = cy + 24.0f - k * 9.0f;
        PointF p[4] = { PointF(cx - 6.0f, y0), PointF(cx + 6.0f, y0 - 5.0f),
                        PointF(cx + 6.0f, y0 - 10.0f), PointF(cx - 6.0f, y0 - 5.0f) };
        g.FillPolygon(&red, p, 4);
    }

    // hook
    g.DrawArc(&white, cx, cy - 15.0f, 18.0f, 18.0f, 180.0f, 180.0f);
    Pen redPen(stripe, 12.0f);
    REAL dp[2] = { 0.5f, 0.5f };
    redPen.SetDashPattern(dp, 2);
    g.DrawArc(&redPen, cx, cy - 15.0f, 18.0f, 18.0f, 180.0f, 180.0f);

    if (c.gold)
    {
        SolidBrush sp(Color(230, 255, 255, 255));
        g.FillEllipse(&sp, cx + 12.0f, cy - 20.0f, 6.0f, 6.0f);
    }
}

static void DrawIcicle(Graphics& g, const Icicle& ic)
{
    float sx = 0;
    float dx = ic.x - RX;
    if (!ic.falling && dx < G.speed * ic.fallT + 350.0f && G.state == State::Playing)
        sx = std::sin(G.time * 60.0f) * 2.5f;

    float x = ic.x + sx;

    // frozen cap on the ceiling
    if (!ic.falling)
    {
        SolidBrush cap(Color(255, 225, 238, 255));
        g.FillEllipse(&cap, x - ic.w * 0.7f, ic.y - 10.0f, ic.w * 1.4f, 16.0f);
    }

    PointF p[3] = { PointF(x - ic.w / 2, ic.y), PointF(x + ic.w / 2, ic.y), PointF(x, ic.y + ic.len) };
    SolidBrush body(Color(235, 175, 225, 255));
    g.FillPolygon(&body, p, 3);

    Pen edge(Color(255, 235, 248, 255), 2.0f);
    g.DrawPolygon(&edge, p, 3);

    Pen shine(Color(190, 255, 255, 255), 2.0f);
    g.DrawLine(&shine, x - ic.w * 0.18f, ic.y + 6.0f, x - 1.0f, ic.y + ic.len * 0.65f);
}

static void DrawSpirit(Graphics& g, const Spirit& s)
{
    float x = s.x, y = s.y, w = s.w, h = s.h;

    Color fill, glowC;
    int alpha;
    switch (s.type)
    {
        case 0:  fill = Color(200, 205, 228, 255); glowC = Color(45, 170, 210, 255); break;
        case 1:  fill = Color(205, 175, 238, 170); glowC = Color(45, 150, 255, 140); break;
        default: fill = Color(240, 42, 30, 64);    glowC = Color(40, 150, 60, 220);  break;
    }
    (void)alpha;

    // aura
    SolidBrush glow(glowC);
    g.FillEllipse(&glow, x - w * 0.8f, y - h * 0.7f, w * 1.6f, h * 1.4f);

    // body: round hood on top, wavy hem at the bottom
    std::vector<PointF> pts;
    float cy = y - h / 2 + w / 2;
    for (int a = 180; a <= 360; a += 20)
    {
        float rad = a * PI_F / 180.0f;
        pts.push_back(PointF(x + std::cos(rad) * w / 2, cy + std::sin(rad) * w / 2));
    }
    for (int i = 0; i <= 6; i++)
    {
        float px = x + w / 2 - i * (w / 6.0f);
        float py = y + h / 2 - ((i % 2) ? 0.0f : 11.0f) + std::sin(G.time * 5.0f + i + s.phase) * 3.0f;
        pts.push_back(PointF(px, py));
    }
    SolidBrush body(fill);
    g.FillPolygon(&body, pts.data(), (INT)pts.size());

    SolidBrush dark(Color(255, 15, 15, 30));
    if (s.type == 2)
    {
        // hooded Ghost of Christmas Future: black face, glowing eyes
        g.FillEllipse(&dark, x - w * 0.28f, y - h * 0.38f, w * 0.56f, h * 0.38f);
        SolidBrush eye(Color(255, 255, 90, 40));
        float pulse = 1.0f + 0.3f * std::sin(G.time * 8.0f);
        g.FillEllipse(&eye, x - w * 0.16f, y - h * 0.24f, 7.0f * pulse, 7.0f * pulse);
        g.FillEllipse(&eye, x + w * 0.06f, y - h * 0.24f, 7.0f * pulse, 7.0f * pulse);
    }
    else
    {
        g.FillEllipse(&dark, x - w * 0.24f, y - h * 0.22f, 9.0f, 13.0f);
        g.FillEllipse(&dark, x + w * 0.10f, y - h * 0.22f, 9.0f, 13.0f);
        g.FillEllipse(&dark, x - 5.0f, y - h * 0.02f, 10.0f, 14.0f);
    }

    if (s.type == 1)
    {
        // holly crown of the Ghost of Christmas Present
        SolidBrush leaf(Color(255, 30, 130, 60));
        SolidBrush berry(Color(255, 220, 30, 40));
        float top = y - h / 2 + 2.0f;
        for (int k = -2; k <= 2; k++)
            g.FillEllipse(&leaf, x + k * 12.0f - 9.0f, top - 6.0f + std::fabs((float)k) * 3.0f, 18.0f, 10.0f);
        g.FillEllipse(&berry, x - 8.0f, top - 3.0f, 6.0f, 6.0f);
        g.FillEllipse(&berry, x + 3.0f, top - 3.0f, 6.0f, 6.0f);
    }
    else if (s.type == 0)
    {
        // little candle flame of the Ghost of Christmas Past
        SolidBrush flame(Color(230, 255, 220, 90));
        float fl = 1.0f + 0.2f * std::sin(G.time * 12.0f + s.phase);
        g.FillEllipse(&flame, x - 5.0f, y - h / 2 - 16.0f, 10.0f * fl, 16.0f * fl);
    }
}

static void DrawReindeer(Graphics& g)
{
    float x = RX, y = G.ry;

    bool blink = (G.invuln > 0 && G.starT <= 0 && std::fmod(G.invuln, 0.2f) < 0.1f);
    if (blink)
        return;

    // shadow on the snow
    float hgt = Clampf((GROUND_Y - y) / 400.0f, 0.0f, 1.0f);
    SolidBrush shadow(Color((BYTE)(90 - 50 * hgt), 40, 60, 110));
    float sw = 80.0f * (1.0f - 0.4f * hgt);
    g.FillEllipse(&shadow, x - sw / 2, GROUND_Y + 8.0f, sw, 10.0f);

    // star-power rainbow aura
    if (G.starT > 0 && (G.starT > 1.5f || std::fmod(G.starT, 0.2f) < 0.1f))
    {
        int r = (int)(140 + 115 * std::sin(G.time * 6.0f));
        int gg = (int)(140 + 115 * std::sin(G.time * 6.0f + 2.1f));
        int b = (int)(140 + 115 * std::sin(G.time * 6.0f + 4.2f));
        SolidBrush aura(Color(80, (BYTE)r, (BYTE)gg, (BYTE)b));
        g.FillEllipse(&aura, x - 70.0f, y - 70.0f, 140.0f, 130.0f);
    }

    // legs
    Pen leg(Color(255, 105, 66, 36), 6.0f);
    leg.SetStartCap(LineCapRound);
    leg.SetEndCap(LineCapRound);
    float legX[4] = { -26.0f, -14.0f, 14.0f, 26.0f };
    for (int i = 0; i < 4; i++)
    {
        float bx = x + legX[i], by = y + 14.0f;
        float ex, ey;
        if (G.grounded)
        {
            float sw2 = std::sin(G.time * 16.0f + i * 1.6f) * 14.0f;
            ex = bx + sw2;
            ey = by + 22.0f - ((sw2 > 0) ? 0.0f : 5.0f);
        }
        else
        {
            float dir = Clampf(G.rvy / 300.0f, -1.0f, 1.0f);
            ex = bx + ((i < 2) ? -12.0f : 12.0f) + dir * 4.0f;
            ey = by + 14.0f;
        }
        g.DrawLine(&leg, bx, by, ex, ey);
    }

    // tail
    SolidBrush tail(Color(255, 250, 250, 255));
    g.FillEllipse(&tail, x - 44.0f, y - 14.0f, 14.0f, 12.0f);

    // body
    SolidBrush bodyB(Color(255, 150, 98, 52));
    g.FillEllipse(&bodyB, x - 36.0f, y - 20.0f, 72.0f, 40.0f);
    SolidBrush belly(Color(200, 230, 195, 150));
    g.FillEllipse(&belly, x - 24.0f, y + 2.0f, 48.0f, 14.0f);

    // neck + head
    float hx = x + 42.0f, hy = y - 40.0f;
    PointF neck[4] = { PointF(x + 16.0f, y - 14.0f), PointF(x + 36.0f, y - 22.0f),
                       PointF(hx + 6.0f, hy + 8.0f), PointF(hx - 14.0f, hy + 4.0f) };
    g.FillPolygon(&bodyB, neck, 4);
    g.FillEllipse(&bodyB, hx - 17.0f, hy - 13.0f, 34.0f, 26.0f);
    SolidBrush snout(Color(255, 190, 135, 85));
    g.FillEllipse(&snout, hx + 6.0f, hy - 6.0f, 20.0f, 16.0f);

    // scarf with a flapping tail
    SolidBrush scarf(Color(255, 205, 30, 45));
    g.FillRectangle(&scarf, x + 22.0f, y - 26.0f, 18.0f, 9.0f);
    float flap = std::sin(G.time * 10.0f) * 3.0f;
    PointF st[4] = { PointF(x + 24.0f, y - 20.0f), PointF(x + 30.0f, y - 18.0f),
                     PointF(x + 8.0f, y - 8.0f + flap), PointF(x + 2.0f, y - 12.0f + flap) };
    g.FillPolygon(&scarf, st, 4);

    // glowing red nose
    float pulse = 3.0f * std::sin(G.time * 5.0f);
    SolidBrush noseGlow(Color(70, 255, 60, 60));
    g.FillEllipse(&noseGlow, hx + 25.0f - 14.0f - pulse, hy - 1.0f - 14.0f - pulse,
                  28.0f + 2.0f * pulse, 28.0f + 2.0f * pulse);
    SolidBrush nose(Color(255, 240, 30, 40));
    g.FillEllipse(&nose, hx + 19.0f, hy - 7.0f, 13.0f, 13.0f);

    // eye and ear
    SolidBrush dark(Color(255, 20, 15, 15));
    g.FillEllipse(&dark, hx + 1.0f, hy - 8.0f, 6.0f, 7.0f);
    SolidBrush white(Color(255, 255, 255, 255));
    g.FillEllipse(&white, hx + 3.0f, hy - 7.0f, 2.5f, 2.5f);
    g.FillEllipse(&bodyB, hx - 14.0f, hy - 24.0f, 10.0f, 15.0f);

    // antlers
    Pen antler(Color(255, 215, 180, 120), 4.0f);
    antler.SetStartCap(LineCapRound);
    antler.SetEndCap(LineCapRound);
    Pen antlerFar(Color(255, 165, 135, 90), 3.0f);
    antlerFar.SetStartCap(LineCapRound);
    antlerFar.SetEndCap(LineCapRound);

    g.DrawLine(&antlerFar, hx + 4.0f, hy - 12.0f, hx - 2.0f, hy - 40.0f);
    g.DrawLine(&antlerFar, hx + 1.0f, hy - 26.0f, hx + 10.0f, hy - 34.0f);
    g.DrawLine(&antler, hx - 6.0f, hy - 12.0f, hx - 18.0f, hy - 42.0f);
    g.DrawLine(&antler, hx - 12.0f, hy - 28.0f, hx - 1.0f, hy - 38.0f);
    g.DrawLine(&antler, hx - 15.0f, hy - 36.0f, hx - 26.0f, hy - 38.0f);
}

static void DrawParticles(Graphics& g)
{
    for (const Particle& p : G.parts)
    {
        float a = Clampf(p.life / p.maxLife, 0.0f, 1.0f);
        SolidBrush b(Color((BYTE)(a * 255.0f), p.r, p.g, p.b));
        g.FillEllipse(&b, p.x - p.size / 2, p.y - p.size / 2, p.size, p.size);
    }
}

static void DrawFlakes(Graphics& g)
{
    for (const Flake& f : G.flakes)
    {
        SolidBrush b(Color((BYTE)(120 + 120 * f.depth), 255, 255, 255));
        g.FillEllipse(&b, f.x, f.y, f.size, f.size);
    }
}

static void DrawWorld(Graphics& g)
{
    for (const Icicle& ic : G.icicles) DrawIcicle(g, ic);
    for (const Cane& c : G.canes)      DrawCane(g, c);

    for (const Pickup& p : G.pickups)
    {
        float pulse = 3.0f * std::sin(G.time * 6.0f);
        if (p.type == 0)
        {
            SolidBrush glow(Color(80, 255, 220, 60));
            g.FillEllipse(&glow, p.x - 30.0f - pulse, p.y - 30.0f - pulse, 60.0f + 2.0f * pulse, 60.0f + 2.0f * pulse);
            DrawStarShape(g, p.x, p.y, 24.0f, Color(255, 255, 215, 50));
        }
        else
        {
            SolidBrush glow(Color(70, 255, 80, 120));
            g.FillEllipse(&glow, p.x - 30.0f - pulse, p.y - 30.0f - pulse, 60.0f + 2.0f * pulse, 60.0f + 2.0f * pulse);
            DrawHeartShape(g, p.x - 20.0f, p.y - 20.0f, 40.0f, Color(255, 235, 40, 80));
        }
    }

    for (const Spirit& s : G.spirits) DrawSpirit(g, s);

    DrawReindeer(g);
    DrawParticles(g);
    DrawFlakes(g);
}

// ----------------------------------------------------------------------------
//  HUD and overlays
// ----------------------------------------------------------------------------
static void DrawHUD(Graphics& g)
{
    SolidBrush bar(Color(170, 0, 0, 20));
    g.FillRectangle(&bar, 0, 0, WORLD_W, (int)PLAY_TOP);

    Txt(g, L"SCORE: " + std::to_wstring(Score()), 15, 14, 20, Color(255, 255, 215, 90));
    Txt(g, std::to_wstring((int)G.dist) + L" m", 190, 14, 20, Color(255, 160, 220, 255));

    if (G.combo >= 2)
        Txt(g, L"COMBO x" + std::to_wstring(Multiplier()) + L"  (" + std::to_wstring(G.combo) + L")",
            300, 14, 20, Color(255, 255, 120, 120));

    if (G.starT > 0)
        Txt(g, L"STAR " + std::to_wstring((int)std::ceil(G.starT)) + L"s", 500, 15, 18, Color(255, 255, 230, 90));

    Txt(g, L"BEST: " + std::to_wstring(G.highScore), 640, 14, 18, Color(255, 205, 215, 235));

    for (int i = 0; i < G.lives; i++)
        DrawHeartShape(g, 755.0f + i * 26.0f, 14.0f, 22.0f, Color(255, 235, 40, 80));

    if (!soundOn)
        Txt(g, L"MUTED (M to unmute)", 15, WORLD_H - 28.0f, 16, Color(255, 220, 230, 245));

    if (G.msgT > 0 && G.state == State::Playing)
    {
        int a = (int)(255.0f * Clampf(G.msgT / 0.5f, 0.0f, 1.0f));
        Txt(g, G.msg, 0, 130, 44, Color((BYTE)a, 255, 240, 140), true);
    }
}

static void Dim(Graphics& g, int alpha)
{
    SolidBrush d(Color((BYTE)alpha, 0, 0, 20));
    g.FillRectangle(&d, 0, 0, WORLD_W, WORLD_H);
}

static void DrawOverlays(Graphics& g)
{
    bool blink = std::fmod(G.time, 1.0f) < 0.6f;
    Color soft(255, 235, 240, 250);

    switch (G.state)
    {
        case State::Title:
        {
            Dim(g, 90);
            Txt(g, L"REINDEER RUSH", 0, 90, 76, Color(255, 255, 80, 80), true);
            Txt(g, L"CHRISTMAS EDITION", 0, 180, 30, Color(255, 100, 230, 130), true);

            Txt(g, L"Hold SPACE / UP / W to fly up. Let go to glide down.", 0, 260, 21, soft, true, false);
            Txt(g, L"Collect candy canes. Dodge icicles and the Ghosts of Christmas!", 0, 292, 21, soft, true, false);
            Txt(g, L"Gold cane = bonus     Heart = extra life     Star = unstoppable", 0, 324, 21,
                Color(255, 255, 225, 110), true, false);
            Txt(g, L"P = pause    R = restart    M = mute    F11 = fullscreen", 0, 356, 21, soft, true, false);

            if (blink)
                Txt(g, L"Press ENTER to start", 0, 430, 34, Color(255, 255, 255, 255), true);

            Txt(g, L"BEST SCORE: " + std::to_wstring(G.highScore), 0, 500, 22, Color(255, 255, 215, 90), true);
            break;
        }

        case State::Paused:
        {
            Dim(g, 160);
            Txt(g, L"PAUSED", 0, 220, 66, Color(255, 255, 215, 90), true);
            Txt(g, L"P = resume     R = restart     Esc = title", 0, 315, 22, soft, true, false);
            break;
        }

        case State::GameOver:
        {
            Dim(g, 165);
            Txt(g, L"THE GHOSTS GOT YOU!", 0, 150, 60, Color(255, 255, 90, 90), true);
            Txt(g, L"Score: " + std::to_wstring(Score()), 0, 250, 34, Color(255, 255, 215, 90), true);
            Txt(g, L"Distance: " + std::to_wstring((int)G.dist) + L" m", 0, 295, 24, Color(255, 160, 220, 255), true);
            Txt(g, L"Best: " + std::to_wstring(G.highScore), 0, 332, 24, soft, true);
            if (blink)
                Txt(g, L"Press R or ENTER to fly again", 0, 410, 28, Color(255, 255, 255, 255), true);
            break;
        }

        default:
            break;
    }
}

static void Render(HDC hdc)
{
    {
        Graphics g(backBuffer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        // the world shakes; HUD and overlays stay steady
        g.TranslateTransform(G.shakeX, G.shakeY);
        DrawBackground(g);
        DrawWorld(g);
        g.ResetTransform();

        DrawHUD(g);
        DrawOverlays(g);
    }

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
//  Input and window
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
            PlaySoundA(NULL, NULL, 0);
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

            backBuffer = new Bitmap(WORLD_W, WORLD_H, PixelFormat32bppPARGB);

            for (int i = 0; i < 70; i++)
            {
                Star s = { RandRange(0, WORLD_W), RandRange(0, GROUND_Y - 120.0f), RandRange(0, 6.28f) };
                G.stars.push_back(s);
            }
            for (int i = 0; i < 140; i++)
            {
                Flake f;
                f.x = RandRange(0, (float)WORLD_W);
                f.y = RandRange(0, (float)WORLD_H);
                f.depth = RandRange(0.3f, 1.0f);
                f.size = 1.5f + 2.5f * f.depth;
                f.drift = RandRange(-10.0f, 10.0f) * f.depth;
                G.flakes.push_back(f);
            }

            LoadHighScore();
            InitSound();
            return 0;
        }

        case WM_KEYDOWN:
        {
            if (lParam & 0x40000000)   // ignore auto-repeat
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
                SetCursor(NULL);
                return TRUE;
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1;

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
            delete backBuffer;
            backBuffer = nullptr;

            ShutdownSound();
            GdiplusShutdown(gdiplusToken);
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    const wchar_t CLASS_NAME[] = L"ReindeerRush";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    // Icon comes from the resource compiled from ReindeerRush.rc (resource id 1).
    wc.hIcon   = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON),
                                   GetSystemMetrics(SM_CYICON), LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT rc = { 0, 0, WORLD_W, WORLD_H };
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Reindeer Rush - Christmas Edition", style,
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
        if (dt > 0.05f) dt = 0.05f;

        Update(dt);
        SyncMusic();

        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);

        Sleep(1);
    }

    return 0;
}
