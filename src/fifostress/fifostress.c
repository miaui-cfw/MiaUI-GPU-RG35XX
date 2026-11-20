/**
 * @file fifostress.c
 * @brief Display FIFO underflow stress test for the RG35XX (Actions ATM7059 DE + PowerVR SGX544).
 *
 * Draws a fixed, repeatable GPU/CPU load and counts the kernel's "fifo underflow" lines (de_atm7059.c,
 * de_path_fcr_set: a sticky status read at every flip) that appear while it runs. One line per report window and a
 * summary with fps, so a "fix" that only lowers the frame rate shows.
 *
 * No SDL, no GL headers: libEGL / libGLESv2 are opened at run time, so the same binary runs on any MiaUI build. EGL
 * on the PVR fbdev window (native window 0); the swap waits for the OWL vsync first, as the device's SDL does.
 * Writes nothing to disk. MinUI must be parked by the caller (one GL client owns the display).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>

#include "chdload.h"
#include "replay.h"

/* ---------------------------------------------------------------- EGL / GLES2 subset (no headers needed) */
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef int32_t EGLint;
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;

#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_TRIANGLE_STRIP 0x0005
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_DEPTH_TEST 0x0B71

/** @brief The EGL and GLES2 entry points used, resolved with dlsym. */
static struct
{
    EGLDisplay (*GetDisplay)(void*);
    unsigned (*Initialize)(EGLDisplay, EGLint*, EGLint*);
    unsigned (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
    unsigned (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    unsigned (*SwapBuffers)(EGLDisplay, EGLSurface);
    unsigned (*SwapInterval)(EGLDisplay, EGLint);
    unsigned (*Terminate)(EGLDisplay);
    unsigned (*DestroySurface)(EGLDisplay, EGLSurface);
    unsigned (*DestroyContext)(EGLDisplay, EGLContext);
    EGLint (*GetError)(void);
    void* (*GetProcAddress)(const char*);
} egl;

/** @brief The GLES2 entry points used. */
static struct
{
    void (*Clear)(GLbitfield);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint*);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar*);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint*);
    void (*UseProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar*);
    void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Uniform1i)(GLint, GLint);
    void (*EnableVertexAttribArray)(GLuint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*GenTextures)(GLsizei, GLuint*);
    void (*BindTexture)(GLenum, GLuint);
    void (*ActiveTexture)(GLenum);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*PixelStorei)(GLenum, GLint);
    void (*GenFramebuffers)(GLsizei, GLuint*);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*GenRenderbuffers)(GLsizei, GLuint*);
    void (*BindRenderbuffer)(GLenum, GLuint);
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (*Finish)(void);
    void (*DiscardFramebufferEXT)(GLenum, GLsizei, const GLenum*);
} gl;

/* ---------------------------------------------------------------- options */
#define SCREEN_W 640
#define SCREEN_H 480
#define FBIO_OWL_WAITFORVSYNC _IOW('O', 57, long long)   /* include/video/owlfb.h OWLFB_WAITFORVSYNC */
/** @brief include/video/owlfb.h struct owlfb_sync_info (OWLFB_VSYNC_EVENT_EN's argument). */
struct OwlSyncInfo
{
    uint8_t enabled;
    uint8_t dispId;
    uint16_t reserved;
};
#define FBIO_OWL_VSYNC_EVENT_EN _IOW('O', 67, struct OwlSyncInfo)

/** @brief Run settings (command line). */
static struct
{
    int seconds;          ///< run length
    int report;           ///< seconds per report line
    int fill;             ///< full-screen blended solid quads per frame
    int tex;              ///< full-screen quads sampling a 1024x1024 RGBA8 texture, minified 4x (texture bandwidth)
    int fbo;              ///< draw the frame into a 640x400 RGB565 + D24S8 render target, then blit it
    int fboPasses;        ///< render target <-> window switches per frame (each one stores and reloads tiles)
    int discard;          ///< discard the render target's depth/stencil after drawing
    int uploadW, uploadH; ///< per-frame texture upload size (0: none)
    int uploads;          ///< uploads per frame (3 rotating textures each)
    char uploadFmt[8];    ///< upload format: "565" (RGB565) or "5551" (RGBA5551, AITD's sprite layers)
    int cpuMB;            ///< MB each CPU thread copies per frame
    int cpuThreads;       ///< CPU copy threads
    int delayMs;          ///< sleep before drawing (pushes the GPU work later in the scan)
    int flipAtUs;         ///< -1: off; else finish the frame, wait for vsync, sleep this long, then flip
    int vsyncWait;        ///< wait for the OWL vsync before the swap (as the device's SDL)
    int finish;           ///< glFinish before the swap
    char rampParam[16];   ///< parameter raised until the first underflow ("" = none)
    int rampStep;         ///< its step
    int rampSeconds;      ///< seconds per step
    int json;             ///< JSON summary line
    int audio;            ///< play silence through ALSA "default" (44100 Hz stereo S16), as the games' audio does
} opt = { 60, 5, 0, 0, 0, 1, 0, 0, 0, 1, "565", 0, 0, 0, -1, 1, 0, "", 1, 10, 0, 0 };

static int s_noSwap = 0;                  ///< --no-swap: glFinish each frame instead of eglSwapBuffers (no GPU flips)
static const char* s_replayPath = NULL;   ///< --replay: a gltrace capture played instead of the synthetic load
static int s_skipOps[16], s_skipCount = 0; ///< --skip: record types left out of the replay
static const char* s_chdPath = NULL;      ///< --chd: CHD image decompressed alongside
static int s_chdRate = 0;                 ///< its pace in hunks a second (0: flat out)

/* ---------------------------------------------------------------- kernel log */
/**
 * @brief Reads the whole kernel log ring (as dmesg does: a snapshot, no live reader).
 * @param len Out: bytes read.
 * @return A malloc'd buffer (caller frees), or NULL.
 */
static char* readKernelLog(int* len)
{
    int size = klogctl(10, NULL, 0);   /* SYSLOG_ACTION_SIZE_BUFFER */
    if (size <= 0)
        size = 1 << 20;
    char* buf = malloc((size_t)size + 1);
    if (!buf)
        return NULL;
    *len = klogctl(3, buf, size);      /* SYSLOG_ACTION_READ_ALL */
    if (*len < 0)
    {
        free(buf);
        return NULL;
    }
    buf[*len] = 0;
    return buf;
}

/**
 * @brief Parses the "[ seconds.micro]" timestamp at the start of a kernel log line (after an optional "<N>").
 * @param line The line.
 * @param ts Out: the timestamp in seconds.
 * @return 1 if found.
 */
static int lineTime(const char* line, double* ts)
{
    if (*line == '<')
    {
        const char* close = strchr(line, '>');
        if (!close)
            return 0;
        line = close + 1;
    }
    if (*line != '[')
        return 0;
    return sscanf(line + 1, "%lf", ts) == 1;
}

/**
 * @brief Counts underflow lines newer than a timestamp and returns the newest timestamp seen in the log.
 * @param after Only lines with a timestamp above this count.
 * @param newest Out: the newest timestamp of any line (the next call's `after`).
 * @param times Out (optional): the underflow timestamps (up to maxTimes).
 * @param maxTimes Size of times.
 * @return Underflows found, or -1 if the log could not be read.
 */
static int countUnderflows(double after, double* newest, double* times, int maxTimes)
{
    int len = 0, count = 0;
    char* log = readKernelLog(&len);
    if (!log)
        return -1;
    *newest = after;
    for (char* line = log; line && *line;)
    {
        char* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        double ts;
        if (lineTime(line, &ts))
        {
            if (ts > *newest)
                *newest = ts;
            if (ts > after && strstr(line, "fifo underflow"))
            {
                if (times && count < maxTimes)
                    times[count] = ts;
                count++;
            }
        }
        line = next;
    }
    free(log);
    return count;
}

/* ---------------------------------------------------------------- CPU load */
static pthread_mutex_t cpuLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cpuGo = PTHREAD_COND_INITIALIZER;
static unsigned long cpuFrame = 0;
static int cpuStop = 0;

/**
 * @brief A CPU copy thread: on every frame, copies opt.cpuMB megabytes between two 8 MB buffers (far beyond the
 *        512 KB L2, so every byte goes to DDR).
 * @param arg Unused.
 * @return NULL.
 */
static void* cpuThread(void* arg)
{
    (void)arg;
    const size_t bufSize = 8u << 20;
    char* a = malloc(bufSize);
    char* b = malloc(bufSize);
    if (!a || !b)
        return NULL;
    memset(a, 1, bufSize);
    memset(b, 2, bufSize);
    unsigned long seen = 0;
    for (;;)
    {
        pthread_mutex_lock(&cpuLock);
        while (cpuFrame == seen && !cpuStop)
            pthread_cond_wait(&cpuGo, &cpuLock);
        seen = cpuFrame;
        const int stop = cpuStop;
        const size_t total = (size_t)opt.cpuMB << 20;
        pthread_mutex_unlock(&cpuLock);
        if (stop)
            break;
        for (size_t done = 0; done < total; done += bufSize)
        {
            const size_t n = total - done < bufSize ? total - done : bufSize;
            memcpy(b, a, n);
            char* t = a;
            a = b;
            b = t;
        }
    }
    free(a);
    free(b);
    return NULL;
}

/* ---------------------------------------------------------------- audio load */
static volatile int audioStop = 0;

/**
 * @brief Plays silence through ALSA's "default" device (the device's softvol plug, as SDL opens it) until audioStop:
 *        the I2S DMA runs exactly as during a game, with nothing audible. libasound is opened at run time.
 * @param arg Unused.
 * @return NULL.
 */
static void* audioThread(void* arg)
{
    (void)arg;
    void* lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!lib)
    {
        fprintf(stderr, "fifostress: no libasound: audio off\n");
        return NULL;
    }
    int (*pcmOpen)(void**, const char*, int, int) = (int (*)(void**, const char*, int, int))dlsym(lib, "snd_pcm_open");
    int (*setParams)(void*, int, int, unsigned, unsigned, int, unsigned) =
        (int (*)(void*, int, int, unsigned, unsigned, int, unsigned))dlsym(lib, "snd_pcm_set_params");
    long (*writei)(void*, const void*, unsigned long) = (long (*)(void*, const void*, unsigned long))dlsym(lib, "snd_pcm_writei");
    int (*recover)(void*, int, int) = (int (*)(void*, int, int))dlsym(lib, "snd_pcm_recover");
    int (*pcmClose)(void*) = (int (*)(void*))dlsym(lib, "snd_pcm_close");
    void* pcm = NULL;
    /* SND_PCM_STREAM_PLAYBACK 0, SND_PCM_FORMAT_S16_LE 2, SND_PCM_ACCESS_RW_INTERLEAVED 3; 2 ch, 44100 Hz, 50 ms */
    if (!pcmOpen || !setParams || !writei || pcmOpen(&pcm, "default", 0, 0) < 0 || setParams(pcm, 2, 3, 2, 44100, 1, 50000) < 0)
    {
        fprintf(stderr, "fifostress: cannot open ALSA default: audio off\n");
        return NULL;
    }
    static int16_t silence[1024 * 2];
    while (!audioStop)
    {
        long r = writei(pcm, silence, 1024);
        if (r < 0)
            recover(pcm, (int)r, 1);
    }
    pcmClose(pcm);
    return NULL;
}

/* ---------------------------------------------------------------- GL helpers */
/**
 * @brief Loads one symbol or exits.
 * @param lib dlopen handle.
 * @param name Symbol name.
 * @return The address.
 */
static void* need(void* lib, const char* name)
{
    void* p = dlsym(lib, name);
    if (!p)
    {
        fprintf(stderr, "fifostress: %s missing\n", name);
        exit(2);
    }
    return p;
}

/**
 * @brief Opens libEGL and libGLESv2 and resolves every entry point.
 */
static void loadGL(void)
{
    void* e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!e)
        e = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    void* g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g)
        g = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!e || !g)
    {
        fprintf(stderr, "fifostress: cannot open libEGL/libGLESv2: %s\n", dlerror());
        exit(2);
    }
#define E(n) *(void**)&egl.n = need(e, "egl" #n)
#define G(n) *(void**)&gl.n = need(g, "gl" #n)
    E(GetDisplay); E(Initialize); E(ChooseConfig); E(CreateWindowSurface); E(CreateContext); E(MakeCurrent);
    E(SwapBuffers); E(SwapInterval); E(Terminate); E(DestroySurface); E(DestroyContext); E(GetError);
    E(GetProcAddress);
    G(Clear); G(ClearColor); G(Viewport); G(Enable); G(Disable); G(BlendFunc); G(CreateShader); G(ShaderSource);
    G(CompileShader); G(GetShaderiv); G(GetShaderInfoLog); G(CreateProgram); G(AttachShader); G(BindAttribLocation);
    G(LinkProgram); G(GetProgramiv); G(UseProgram); G(GetUniformLocation); G(Uniform4f); G(Uniform1i);
    G(EnableVertexAttribArray); G(VertexAttribPointer); G(DrawArrays); G(GenTextures); G(BindTexture);
    G(ActiveTexture); G(TexImage2D); G(TexSubImage2D); G(TexParameteri); G(PixelStorei); G(GenFramebuffers);
    G(BindFramebuffer); G(FramebufferTexture2D); G(GenRenderbuffers); G(BindRenderbuffer); G(RenderbufferStorage);
    G(FramebufferRenderbuffer); G(Finish);
#undef E
#undef G
    *(void**)&gl.DiscardFramebufferEXT = egl.GetProcAddress("glDiscardFramebufferEXT");
}

/**
 * @brief Compiles and links a program with attributes 0 = aPos (vec2), 1 = aUv (vec2).
 * @param fs Fragment shader source.
 * @return The program.
 */
static GLuint makeProgram(const char* fs)
{
    static const char* vs =
        "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv; uniform vec4 uXform;\n"
        "void main() { vUv = aUv * uXform.xy; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const char* src[2] = { vs, fs };
    const GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint p = gl.CreateProgram();
    for (int i = 0; i < 2; i++)
    {
        GLuint s = gl.CreateShader(type[i]);
        gl.ShaderSource(s, 1, &src[i], NULL);
        gl.CompileShader(s);
        GLint ok = 0;
        gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[512];
            gl.GetShaderInfoLog(s, sizeof log, NULL, log);
            fprintf(stderr, "fifostress: shader: %s\n", log);
            exit(2);
        }
        gl.AttachShader(p, s);
    }
    gl.BindAttribLocation(p, 0, "aPos");
    gl.BindAttribLocation(p, 1, "aUv");
    gl.LinkProgram(p);
    return p;
}

/**
 * @brief Creates a texture with nearest filtering.
 * @param w Width. @param h Height. @param format GL format. @param type GL type. @param data Pixels or NULL.
 * @param wrap Wrap mode.
 * @return The texture.
 */
static GLuint makeTexture(int w, int h, GLenum format, GLenum type, const void* data, GLenum wrap)
{
    GLuint t;
    gl.GenTextures(1, &t);
    gl.BindTexture(GL_TEXTURE_2D, t);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)format, w, h, 0, format, type, data);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wrap);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wrap);
    return t;
}

static GLuint progSolid, progTex;
static GLint solidColor, solidXform, texXform, texSampler;

/**
 * @brief Draws a full-viewport quad with the given program.
 * @param prog Program. @param xform Its uXform location. @param uvScale Texture coordinate scale.
 */
static void quad(GLuint prog, GLint xform, float uvScale)
{
    static const float v[16] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };
    gl.UseProgram(prog);
    gl.Uniform4f(xform, uvScale, uvScale, 0, 0);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 16, v);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, 16, v + 2);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/**
 * @brief Monotonic time in seconds.
 * @return Seconds.
 */
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/**
 * @brief Prints the usage text.
 */
static void usage(void)
{
    puts("fifostress: counts 'fifo underflow' (display FIFO ran dry) under a fixed GPU/CPU load.\n"
         "MinUI must be parked first. Writes nothing to disk.\n\n"
         "  -t SECS          run length (60)\n"
         "  -r SECS          seconds per report line (5)\n"
         "  --fill N         N full-screen blended solid quads per frame\n"
         "  --tex N          N full-screen quads sampling a 1024x1024 RGBA texture minified 4x (GPU reads)\n"
         "  --fbo [PASSES]   draw into a 640x400 565+D24S8 render target, blit to the window; PASSES target\n"
         "                   switches per frame (1)\n"
         "  --discard        discard the render target's depth/stencil\n"
         "  --upload WxH[:N[:FMT]] N texture uploads of WxH per frame, each then drawn (1); FMT 565 (default) or\n"
         "                   5551 (RGBA5551, blended: AITD's sprite layers)\n"
         "  --cpu MB[:T]     T threads (1) each copy MB megabytes per frame\n"
         "  --delay MS       sleep MS before drawing each frame\n"
         "  --no-vsync       swap without waiting for the OWL vsync first\n"
         "  --flip-at MS     finish the frame, wait for the vsync, sleep MS, then flip: the flip lands MS into the scan\n"
         "                   (ramp parameter flipat, in 0.1 ms steps)\n"
         "  --finish         glFinish before the swap\n"
         "  --no-swap        render each frame and glFinish, but never swap: the GPU works, its driver never flips\n"
         "                   (flips then come only from another tool, e.g. panflip)\n"
         "  --ramp P:STEP:S  raise P (fill|tex|uploads|cpu|fbo) by STEP every S seconds until an underflow\n"
         "  --replay FILE    loop the window of a gltrace capture (gltrace.so) instead of the synthetic load\n"
         "  --chd FILE[:HPS] read + decompress a CHD image (libchdr) alongside, HPS hunks a second (0: flat out;\n"
         "                   a 2x CD-ROM is ~16)\n"
         "  --no-timing      replay: issue each frame's calls at once (default: at their recorded time)\n"
         "  --skip OP        leave records of type OP (gltrace_format.h) out of the replay; repeatable\n"
         "  --audio          play silence through ALSA (the I2S DMA runs as in a game)\n"
         "  --json           JSON summary line\n");
}

/**
 * @brief Parses the command line into opt.
 * @param argc Count. @param argv Arguments.
 */
static void parseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        const char* n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-t") && n) opt.seconds = atoi(argv[++i]);
        else if (!strcmp(a, "-r") && n) opt.report = atoi(argv[++i]);
        else if (!strcmp(a, "--fill") && n) opt.fill = atoi(argv[++i]);
        else if (!strcmp(a, "--tex") && n) opt.tex = atoi(argv[++i]);
        else if (!strcmp(a, "--fbo"))
        {
            opt.fbo = 1;
            if (n && n[0] >= '0' && n[0] <= '9')
                opt.fboPasses = atoi(argv[++i]);
        }
        else if (!strcmp(a, "--discard")) opt.discard = 1;
        else if (!strcmp(a, "--upload") && n)
        {
            sscanf(argv[++i], "%dx%d:%d:%7s", &opt.uploadW, &opt.uploadH, &opt.uploads, opt.uploadFmt);
        }
        else if (!strcmp(a, "--cpu") && n)
        {
            opt.cpuThreads = 1;
            sscanf(argv[++i], "%d:%d", &opt.cpuMB, &opt.cpuThreads);
        }
        else if (!strcmp(a, "--delay") && n) opt.delayMs = atoi(argv[++i]);
        else if (!strcmp(a, "--no-vsync")) opt.vsyncWait = 0;
        else if (!strcmp(a, "--flip-at") && n) opt.flipAtUs = (int)(atof(argv[++i]) * 1000);
        else if (!strcmp(a, "--finish")) opt.finish = 1;
        else if (!strcmp(a, "--no-swap")) s_noSwap = 1;
        else if (!strcmp(a, "--ramp") && n)
            sscanf(argv[++i], "%15[^:]:%d:%d", opt.rampParam, &opt.rampStep, &opt.rampSeconds);
        else if (!strcmp(a, "--json")) opt.json = 1;
        else if (!strcmp(a, "--audio")) opt.audio = 1;
        else if (!strcmp(a, "--replay") && n) s_replayPath = argv[++i];
        else if (!strcmp(a, "--skip") && n && s_skipCount < 16) s_skipOps[s_skipCount++] = atoi(argv[++i]);
        else if (!strcmp(a, "--no-timing")) replaySetTiming(0);
        else if (!strcmp(a, "--chd") && n)
        {
            static char path[512];
            snprintf(path, sizeof path, "%s", argv[++i]);
            char* colon = strrchr(path, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9')
            {
                *colon = 0;
                s_chdRate = atoi(colon + 1);
            }
            s_chdPath = path;
        }
        else
        {
            usage();
            exit(!strcmp(a, "-h") || !strcmp(a, "--help") ? 0 : 1);
        }
    }
}

/**
 * @brief The ramped parameter's storage, or NULL.
 * @return Pointer into opt.
 */
static int* rampTarget(void)
{
    if (!strcmp(opt.rampParam, "fill")) return &opt.fill;
    if (!strcmp(opt.rampParam, "tex")) return &opt.tex;
    if (!strcmp(opt.rampParam, "uploads")) return &opt.uploads;
    if (!strcmp(opt.rampParam, "cpu")) return &opt.cpuMB;
    if (!strcmp(opt.rampParam, "fbo")) return &opt.fboPasses;
    if (!strcmp(opt.rampParam, "flipat")) return &opt.flipAtUs;
    return NULL;
}

/**
 * @brief Entry point: sets up EGL, runs the load, reports underflows per window and a summary.
 * @param argc Count. @param argv Arguments.
 * @return 0 on success.
 */
int main(int argc, char** argv)
{
    parseArgs(argc, argv);
    int* ramp = rampTarget();
    if (opt.rampParam[0] && !ramp)
    {
        fprintf(stderr, "fifostress: unknown ramp parameter '%s'\n", opt.rampParam);
        return 1;
    }
    if (!strcmp(opt.rampParam, "cpu") && opt.cpuThreads == 0)
        opt.cpuThreads = 1;
    if (!strcmp(opt.rampParam, "uploads") && opt.uploadW == 0)
        opt.uploadW = 320, opt.uploadH = 240;

    loadGL();
    EGLDisplay dpy = egl.GetDisplay(NULL);
    if (!dpy || !egl.Initialize(dpy, NULL, NULL))
    {
        fprintf(stderr, "fifostress: EGL init failed\n");
        return 2;
    }
    const EGLint cfgAttr[] = { EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_DEPTH_SIZE, 0,
                               EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!egl.ChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1)
    {
        fprintf(stderr, "fifostress: no EGL config\n");
        return 2;
    }
    EGLSurface surf = egl.CreateWindowSurface(dpy, cfg, NULL, NULL);
    if (!surf)
    {
        fprintf(stderr, "fifostress: cannot create the window (error 0x%x): is MinUI or a game still running?\n",
                egl.GetError());
        return 2;
    }
    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = egl.CreateContext(dpy, cfg, NULL, ctxAttr);
    if (!ctx || !egl.MakeCurrent(dpy, surf, surf, ctx))
    {
        fprintf(stderr, "fifostress: cannot create the GL context\n");
        return 2;
    }
    egl.SwapInterval(dpy, 1);
    int fb = opt.vsyncWait ? open("/dev/fb0", O_RDWR) : -1;
    if (fb >= 0)
    {
        /* As the device's SDL does at start: without it the driver's vsync wait returns at once (hw_sync_enable is
           off) and the DE's pre-line interrupt (which spins until vertical blanking, IRQs off) never runs */
        struct OwlSyncInfo on = { 1, 0, 0 };
        if (ioctl(fb, FBIO_OWL_VSYNC_EVENT_EN, &on) < 0)
            fprintf(stderr, "fifostress: OWLFB_VSYNC_EVENT_EN failed: the vsync wait will not block\n");
    }

    /* resources */
    progSolid = makeProgram("precision mediump float; uniform vec4 uColor; varying vec2 vUv;\n"
                            "void main() { gl_FragColor = uColor; }\n");
    solidColor = gl.GetUniformLocation(progSolid, "uColor");
    solidXform = gl.GetUniformLocation(progSolid, "uXform");
    progTex = makeProgram("precision mediump float; uniform sampler2D uTex; varying vec2 vUv;\n"
                          "void main() { gl_FragColor = texture2D(uTex, vUv); }\n");
    texXform = gl.GetUniformLocation(progTex, "uXform");
    texSampler = gl.GetUniformLocation(progTex, "uTex");
    gl.UseProgram(progTex);
    gl.Uniform1i(texSampler, 0);

    uint32_t* big = malloc(1024 * 1024 * 4);
    for (int i = 0; i < 1024 * 1024; i++)
        big[i] = ((uint32_t)(i * 2654435761u) & 0x00FFFFFFu) | 0x80000000u;   /* noise, alpha 0.5 */
    const GLuint bigTex = makeTexture(1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, big, GL_REPEAT);
    free(big);

    GLuint fboTex = 0, fbo = 0, rb = 0;
    fboTex = makeTexture(640, 400, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL, GL_CLAMP_TO_EDGE);
    gl.GenFramebuffers(1, &fbo);
    gl.GenRenderbuffers(1, &rb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, rb);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, 640, 400);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    const int maxUploads = 64;
    const int is5551 = !strcmp(opt.uploadFmt, "5551");
    const GLenum upFormat = is5551 ? GL_RGBA : GL_RGB;
    const GLenum upType = is5551 ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
    GLuint upTex[3 * 64] = { 0 };
    uint16_t* upData = NULL;
    if (opt.uploadW > 0)
    {
        upData = malloc((size_t)opt.uploadW * opt.uploadH * 2);
        for (int i = 0; i < opt.uploadW * opt.uploadH; i++)
            upData[i] = (uint16_t)(i * 40503u);
        for (int i = 0; i < 3 * maxUploads; i++)
            upTex[i] = makeTexture(opt.uploadW, opt.uploadH, upFormat, upType, NULL, GL_CLAMP_TO_EDGE);
    }

    pthread_t threads[8];
    const int nThreads = opt.cpuThreads > 8 ? 8 : opt.cpuThreads;
    for (int i = 0; i < nThreads; i++)
        pthread_create(&threads[i], NULL, cpuThread, NULL);
    pthread_t audio;
    if (opt.audio)
        pthread_create(&audio, NULL, audioThread, NULL);
    if (s_chdPath)
        chdLoadStart(s_chdPath, s_chdRate);

    if (s_replayPath)
    {
        if (replayLoad(s_replayPath) != 0)
            return 2;
        for (int i = 0; i < s_skipCount; i++)
        {
            replaySetSkip(s_skipOps[i]);
            printf("replay: skipping op %d\n", s_skipOps[i]);
        }
    }

    /* baseline: only kernel log lines after this point count */
    double logMark = 0;
    if (countUnderflows(0, &logMark, NULL, 0) < 0)
    {
        fprintf(stderr, "fifostress: cannot read the kernel log (run as root)\n");
        return 2;
    }

    printf("fifostress: %d s; fill %d tex %d fbo %d(x%d)%s upload %dx%d:%d:%s cpu %dMB x%d delay %d vsync %d finish %d"
           "%s%s%s\n", opt.seconds, opt.fill, opt.tex, opt.fbo, opt.fboPasses, opt.discard ? " discard" : "",
           opt.uploadW, opt.uploadH, opt.uploadW ? opt.uploads : 0, opt.uploadFmt, opt.cpuMB, nThreads, opt.delayMs, opt.vsyncWait,
           opt.finish, opt.rampParam[0] ? " ramp " : "", opt.rampParam, opt.audio ? " audio" : "");
    fflush(stdout);

    const double start = now();
    double windowStart = start, nextReport = start + opt.report, nextRamp = start + opt.rampSeconds;
    long frames = 0, windowFrames = 0, totalUnder = 0;
    double worstFrame = 0, windowWorst = 0, last = start;
    int rampHit = -1;
    unsigned long frameNo = 0;

    while (now() - start < opt.seconds)
    {
        if (opt.delayMs)
            usleep((useconds_t)opt.delayMs * 1000);
        if (nThreads)
        {
            pthread_mutex_lock(&cpuLock);
            cpuFrame++;
            pthread_cond_broadcast(&cpuGo);
            pthread_mutex_unlock(&cpuLock);
        }
        frameNo++;

        if (s_replayPath)
            replayFrame();
        for (int pass = 0; !s_replayPath && pass < (opt.fbo ? opt.fboPasses : 1); pass++)
        {
            if (opt.fbo)
            {
                gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
                gl.Viewport(0, 0, 640, 400);
                if (pass == 0)
                {
                    gl.ClearColor(0.1f, 0.1f, 0.2f, 1);
                    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                }
                gl.Enable(GL_DEPTH_TEST);   /* the depth buffer is in use, as in a 3D scene */
            }
            else
            {
                gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
                gl.Viewport(0, 0, SCREEN_W, SCREEN_H);
                gl.ClearColor(0.1f, 0.1f, 0.2f, 1);
                gl.Clear(GL_COLOR_BUFFER_BIT);
            }
            /* the frame's load is spread over the passes */
            const int passes = opt.fbo ? opt.fboPasses : 1;
            gl.ActiveTexture(GL_TEXTURE0);
            gl.BindTexture(GL_TEXTURE_2D, bigTex);
            /* blended, so the SGX's hidden-surface removal cannot drop the layers underneath: every layer reads */
            gl.Enable(GL_BLEND);
            gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (int i = pass; i < opt.tex; i += passes)
                quad(progTex, texXform, 4.0f + (float)(i & 3) * 0.37f);
            for (int i = pass; i < opt.fill; i += passes)
            {
                gl.UseProgram(progSolid);
                gl.Uniform4f(solidColor, (float)(i % 7) / 7.f, 0.5f, (float)(frameNo % 60) / 60.f, 0.05f);
                quad(progSolid, solidXform, 1.0f);
            }
            gl.Disable(GL_BLEND);
            if (opt.uploadW && pass == 0)
            {
                const int ups = opt.uploads > maxUploads ? maxUploads : opt.uploads;
                for (int u = 0; u < ups; u++)
                {
                    const GLuint t = upTex[(frameNo % 3) * maxUploads + u];
                    gl.BindTexture(GL_TEXTURE_2D, t);
                    upData[(frameNo * 131 + u) % ((unsigned)opt.uploadW * opt.uploadH)] ^= 0xFFFF;
                    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, opt.uploadW, opt.uploadH, upFormat, upType,
                                     upData);
                    if (is5551)
                        gl.Enable(GL_BLEND);   /* a sprite layer over the scene */
                    quad(progTex, texXform, 1.0f);
                    gl.Disable(GL_BLEND);
                }
            }
            if (opt.fbo)
            {
                gl.Disable(GL_DEPTH_TEST);
                if (opt.discard && gl.DiscardFramebufferEXT && pass == passes - 1)
                {
                    const GLenum att[2] = { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
                    gl.DiscardFramebufferEXT(GL_FRAMEBUFFER, 2, att);
                }
                /* to the window: the target is sampled (a resolve), then drawn again next pass */
                gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
                gl.Viewport(0, 40, 640, 400);
                if (pass == 0)
                {
                    gl.ClearColor(0, 0, 0, 1);
                    gl.Clear(GL_COLOR_BUFFER_BIT);
                }
                gl.BindTexture(GL_TEXTURE_2D, fboTex);
                quad(progTex, texXform, 1.0f);
            }
        }

        if (opt.finish || opt.flipAtUs >= 0)
            gl.Finish();
        if (fb >= 0)
        {
            long long ts = 0;
            ioctl(fb, FBIO_OWL_WAITFORVSYNC, &ts);
        }
        if (opt.flipAtUs > 0)
        {
            /* busy-wait: a sleep's wake-up jitter (~0.1 ms+) would blur the flip position */
            const double until = now() + opt.flipAtUs / 1e6;
            while (now() < until)
                ;
        }
        if (s_noSwap)
            gl.Finish();
        else
            egl.SwapBuffers(dpy, surf);

        const double t = now();
        const double ft = t - last;
        last = t;
        if (ft > windowWorst)
            windowWorst = ft;
        if (ft > worstFrame)
            worstFrame = ft;
        frames++;
        windowFrames++;

        if (t >= nextReport)
        {
            double newest = logMark;
            const int u = countUnderflows(logMark, &newest, NULL, 0);
            logMark = newest;
            totalUnder += u > 0 ? u : 0;
            printf("t=%5.1f  underflows %d  fps %.1f  worst frame %.1f ms%s", t - start, u,
                   windowFrames / (t - windowStart), windowWorst * 1000, ramp ? "" : "\n");
            if (ramp)
                printf("  %s=%d\n", opt.rampParam, *ramp);
            fflush(stdout);
            windowStart = t;
            windowFrames = 0;
            windowWorst = 0;
            nextReport = t + opt.report;
            if (ramp && u > 0)
            {
                rampHit = *ramp;
                break;
            }
        }
        if (ramp && t >= nextRamp)
        {
            *ramp += opt.rampStep;
            nextRamp = t + opt.rampSeconds;
        }
    }

    const double elapsed = now() - start;
    {
        double newest = logMark;
        const int u = countUnderflows(logMark, &newest, NULL, 0);
        totalUnder += u > 0 ? u : 0;
    }
    if (nThreads)
    {
        pthread_mutex_lock(&cpuLock);
        cpuStop = 1;
        pthread_cond_broadcast(&cpuGo);
        pthread_mutex_unlock(&cpuLock);
        for (int i = 0; i < nThreads; i++)
            pthread_join(threads[i], NULL);
    }
    chdLoadStop();
    if (opt.audio)
    {
        audioStop = 1;
        pthread_join(audio, NULL);
    }
    printf("RESULT underflows %ld in %.0f s = %.2f/min  fps %.1f  worst frame %.1f ms", totalUnder, elapsed,
           totalUnder * 60.0 / elapsed, frames / elapsed, worstFrame * 1000);
    if (ramp)
        printf("  ramp %s: %s %d", opt.rampParam, rampHit >= 0 ? "first underflow at" : "none up to", rampHit >= 0 ? rampHit : *ramp);
    printf("\n");
    if (opt.json)
        printf("{\"underflows\":%ld,\"seconds\":%.1f,\"per_min\":%.2f,\"fps\":%.1f,\"worst_ms\":%.1f,\"ramp\":\"%s\","
               "\"ramp_hit\":%d}\n", totalUnder, elapsed, totalUnder * 60.0 / elapsed, frames / elapsed,
               worstFrame * 1000, opt.rampParam, rampHit);
    if (fb >= 0)
        close(fb);
    egl.MakeCurrent(dpy, NULL, NULL, NULL);
    egl.DestroyContext(dpy, ctx);
    egl.DestroySurface(dpy, surf);
    egl.Terminate(dpy);
    return 0;
}
