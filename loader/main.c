/* main.c -- YoYo Loader based on .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2025 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */
#define _POSIX_TIMERS
#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <AL/efx.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"
#include "unzip.h"

#include "openal_patch.h"

#define STBI_MALLOC vglMalloc
#define STBI_REALLOC vglRealloc
#define STBI_FREE vglFree
#define STB_IMAGE_IMPLEMENTATION
#define STB_ONLY_PNG
#include "stb_image.h"

extern int trophies_init();
extern void patch_trophies();
extern void audio_player_play(char *path, int loop);
extern void audio_player_stop();
extern void audio_player_pause();
extern void audio_player_resume();
extern int audio_player_is_playing();
extern int is_gamepad_connected(int id);
extern void send_post_request(const char *url, const char *data);
extern void mem_profiler(void *framebuf);
extern SceUID post_thid;
extern SceUID get_thid;
extern volatile int post_response_code;
extern volatile int get_response_code;
extern volatile uint64_t downloaded_bytes;
extern uint8_t *downloader_mem_buffer;
extern uint8_t *downloader_hdr_buffer;
extern char *post_url;
extern char *get_url;
extern unsigned _newlib_heap_size;

int disableObjectsArray = 0;
int uncached_mem = 0;
int double_buffering = 0;
int forceGL1 = 0;
int forceSplashSkip = 0;
int platTarget = 0;
int forceBilinear = 0;
int has_net = 0;
extern int maximizeMem;
int debugShaders = 0;
int squeeze_mem = 0;
int debugMode = 0;
int disableAudio = 0;
int ime_active = 0;
int msg_active = 0;
int msg_index = 0;
int ime_index = 0;
int post_active = 0;
int post_index = 0;
int get_active = 0;
int get_index = 0;
int setup_ended = 0;

int deltarune_hack = 0;
int voidstranger_hack = 0;

extern int (*YYGetInt32) (void *args, int idx);
void (*Function_Add)(const char *name, intptr_t func, int argc, char ret);
int (*Java_com_yoyogames_runner_RunnerJNILib_CreateVersionDSMap) (void *env, int a2, int sdk_ver, char *release_version, char *model, char *device, char *manufacturer, char *cpu_abi, char *cpu_abi2, char *bootloader, char *board, char *version, char *region, char *version_name, int has_keyboard);
int (*Java_com_yoyogames_runner_RunnerJNILib_TouchEvent) (void *env, int a2, int type, int id, float x, float y);
float (*Audio_GetTrackPos) (int id);
uint8_t *g_fNoAudio;
int64_t *g_GML_DeltaTime;
uint32_t *g_IOFrameCount;
char **g_pWorkingDirectory;
int *g_TextureScale;

double jni_double = 0.0f;
GLuint main_fb, main_tex = 0xDEADBEEF;
int is_portrait = 0;

char data_path[256];
static uint32_t tex_lru_frame = 0; // frame counter for the texture page LRU
static int tex_lru_num = 0;
static uint32_t tex_lru_bytes = 0;
char data_path_root[256];
char apk_path[256];
char gxp_path[256];

void patch_gamepad();
void GamePadUpdate();

char *translate_frag_shader(const char *string, int size);
char *translate_vert_shader(const char *string, int size);

void recursive_mkdir(char *dir) {
	char *p = dir;
	while (p) {
		char *p2 = strstr(p, "/");
		if (p2) {
			p2[0] = 0;
			sceIoMkdir(dir, 0777);
			p = p2 + 1;
			p2[0] = '/';
		} else break;
	}
}

void loadConfig(const char *game) {
	char configFile[512];
	char buffer[30];
	int value;
#ifdef STANDALONE_MODE
	sprintf(configFile, "app0:yyl.cfg");
#else
	sprintf(configFile, "%s/%s/yyl.cfg", DATA_PATH, game);
#endif
	FILE *config = fopen(configFile, "r");

	if (config) {
		while (EOF != fscanf(config, "%[^=]=%d\n", buffer, &value)) {
			if (strcmp("forceGLES1", buffer) == 0) forceGL1 = value;
			else if (strcmp("forceBilinear", buffer) == 0) forceBilinear = value;
			else if (strcmp("winMode", buffer) == 0) platTarget = value ? 1 : 0; // Retrocompatibility
			else if (strcmp("platTarget", buffer) == 0) platTarget = value;
			else if (strcmp("debugShaders", buffer) == 0) debugShaders = value;
			else if (strcmp("debugMode", buffer) == 0) debugMode = value;
			else if (strcmp("noSplash", buffer) == 0) forceSplashSkip = value;
			else if (strcmp("maximizeMem", buffer) == 0) maximizeMem = value;
			else if (strcmp("netSupport", buffer) == 0) has_net = value;
			else if (strcmp("squeezeMem", buffer) == 0) squeeze_mem = value;
			else if (strcmp("disableAudio", buffer) == 0) disableAudio = value;
			else if (strcmp("uncachedMem", buffer) == 0) uncached_mem = value;
			else if (strcmp("doubleBuffering", buffer) == 0) double_buffering = value;
		}
		fclose(config);
	}
}

extern void *GetPlatformInstance;

static int __stack_chk_guard_fake = 0x42424242;
static char fake_vm[0x1000];
char fake_env[0x1000];

so_module yoyoloader_mod, cpp_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
	return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

#if 1
int debugPrintf(char *text, ...) {
	if (!debugMode)
		return 0;

	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/gms/shared/yyl.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}

	return 0;
}
#endif

/*
 * Performance tuning and measurement, set per game in ux0:data/gms/<game>/tune.txt
 * (one "key=value" per line; a missing file or key keeps the default):
 *   pool=<KB>   vitaGL circular vertex pool (default 3: almost every draw allocates GPU memory)
 *   vsync=0|1   wait for the vertical blank on each swap (default 1)
 *   perf=0|1    write a timing line every 120 frames to ux0:data/gms/shared/perf.log (default 0)
 * perf works with Debug Mode off, so the numbers aren't skewed by the debug overlay.
 */
int tune_pool_kb = 3;
int tune_vsync = 1;
int tune_perf = 0;
int tune_frameskip = 0; // frameskip=N: run the draw step on 1 frame out of N+1 (game logic still runs every frame)
int frame_skipped = 0;   // set by the draw-step wrapper when it skipped drawing this frame (then don't swap)

void read_tune(const char *game_root) {
	char path[512], buf[512];
	snprintf(path, sizeof(path), "%stune.txt", game_root);
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
	if (fd < 0)
		return;
	int n = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	char *s;
	if ((s = strstr(buf, "pool=")))
		tune_pool_kb = atoi(s + 5);
	if ((s = strstr(buf, "vsync=")))
		tune_vsync = atoi(s + 6);
	if ((s = strstr(buf, "perf=")))
		tune_perf = atoi(s + 5);
	if ((s = strstr(buf, "frameskip=")))
		tune_frameskip = atoi(s + 10);
	if (tune_frameskip < 0)
		tune_frameskip = 0;
	if (tune_frameskip > 3)
		tune_frameskip = 3;
	if (tune_pool_kb < 3)
		tune_pool_kb = 3;
	if (tune_pool_kb > 16 * 1024)
		tune_pool_kb = 16 * 1024;
}

// Counters for the perf log (only touched when tune_perf is on)
static uint64_t perf_gl_us;
static uint32_t perf_drawn; // frames that actually ran the draw step
static uint32_t perf_draws, perf_buffers, perf_buffer_bytes, perf_tex_uploads;

static inline uint64_t perf_now(void) {
	return sceKernelGetProcessTimeWide();
}

void perf_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
	if (!tune_perf) {
		glDrawArrays(mode, first, count);
		return;
	}
	uint64_t t = perf_now();
	glDrawArrays(mode, first, count);
	perf_gl_us += perf_now() - t;
	perf_draws++;
}

void perf_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
	if (!tune_perf) {
		glBufferData(target, size, data, usage);
		return;
	}
	uint64_t t = perf_now();
	glBufferData(target, size, data, usage);
	perf_gl_us += perf_now() - t;
	perf_buffers++;
	perf_buffer_bytes += size;
}

void perf_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data) {
	if (!tune_perf) {
		glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
		return;
	}
	uint64_t t = perf_now();
	glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
	perf_gl_us += perf_now() - t;
	perf_tex_uploads++;
}

void perf_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
	uint64_t t = perf_now();
	glDrawElements(mode, count, type, indices);
	perf_gl_us += perf_now() - t;
	perf_draws++;
}

void perf_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
	uint64_t t = perf_now();
	glBufferSubData(target, offset, size, data);
	perf_gl_us += perf_now() - t;
	perf_buffers++;
	perf_buffer_bytes += size;
}

// Graphics functions the runner fetches through dlsym, counted when perf=1
void *perf_gl_lookup(const char *symbol) {
	if (!tune_perf)
		return NULL;
	if (!strcmp(symbol, "glDrawArrays")) return (void *)perf_glDrawArrays;
	if (!strcmp(symbol, "glDrawElements")) return (void *)perf_glDrawElements;
	if (!strcmp(symbol, "glBufferData")) return (void *)perf_glBufferData;
	if (!strcmp(symbol, "glBufferSubData")) return (void *)perf_glBufferSubData;
	if (!strcmp(symbol, "glTexImage2D")) return (void *)perf_glTexImage2D;
	return NULL;
}

/*
 * Timers around the runner's per-frame steps (only installed when perf=1, all on the main thread).
 * Each wrapper restores the original code, calls it and re-patches (SO_CONTINUE).
 */
enum { PS_BEGIN, PS_IO, PS_UPDATE, PS_COLLISION, PS_DRAW, PS_LAYERS, PS_FINISH, PS_END, PS_AUDIO, PS_COUNT };
static const char *perf_step_name[PS_COUNT] = { "begin", "input", "update", "collision", "draw", "room layers", "finish frame", "end", "audio" };
static const char *perf_step_sym[PS_COUNT] = {
	"_Z13DoAStep_Beginv", "_Z10DoAStep_IOv", "_Z14DoAStep_Updatev", "_Z15HandleCollisionv", "_Z12DoAStep_Drawv",
	"_Z14DrawRoomLayersP9tagYYRECTi", "_Z19GR_D3D_Finish_Frameb", "_Z11DoAstep_Endv", "_Z10Audio_Tickv" };
static so_hook perf_hook[PS_COUNT];
static uint64_t perf_step_us[PS_COUNT];

#define PERF_WRAP0(idx, fname) \
	void fname(void) { uint64_t t = perf_now(); SO_CONTINUE(int, perf_hook[idx]); perf_step_us[idx] += perf_now() - t; }
PERF_WRAP0(PS_BEGIN, perf_step_begin)
PERF_WRAP0(PS_IO, perf_step_io)
PERF_WRAP0(PS_UPDATE, perf_step_update)
PERF_WRAP0(PS_COLLISION, perf_step_collision)
// The draw step also implements frameskip: on skipped frames it returns without drawing.
void perf_step_draw(void) {
	static uint32_t fs_counter;
	if (tune_frameskip && (fs_counter++ % (tune_frameskip + 1)) != 0) {
		frame_skipped = 1;
		return;
	}
	frame_skipped = 0;
	perf_drawn++;
	uint64_t t = perf_now();
	SO_CONTINUE(int, perf_hook[PS_DRAW]);
	perf_step_us[PS_DRAW] += perf_now() - t;
}
PERF_WRAP0(PS_END, perf_step_end)
PERF_WRAP0(PS_AUDIO, perf_step_audio)
void perf_step_layers(void *rect, int a) { uint64_t t = perf_now(); SO_CONTINUE(int, perf_hook[PS_LAYERS], rect, a); perf_step_us[PS_LAYERS] += perf_now() - t; }
void perf_step_finish(int a) { uint64_t t = perf_now(); SO_CONTINUE(int, perf_hook[PS_FINISH], a); perf_step_us[PS_FINISH] += perf_now() - t; }

/*
 * perf=2 adds a deeper probe of the draw step: GML code execution and the GML drawing built-ins.
 * These run thousands of times a frame, so only the original call is timed (the hook's own
 * restore/re-patch cost is left out), but the game itself runs slower in this mode.
 */
enum { PB_CODE, PB_SPRITE, PB_TEXT, PB_SURFACE, PB_SURFTARGET, PB_RECT, PB_PAGELOAD, PB_COUNT };
static const char *perf_probe_name[PB_COUNT] = { "GML code", "draw_sprite*", "draw_text*", "draw_surface*", "surface_set_target", "draw_rectangle", "page loads" };
static uint64_t perf_probe_us[PB_COUNT];
static uint32_t perf_probe_calls[PB_COUNT];

typedef struct { const char *sym; int cat; so_hook h; } perf_probe_t;
static perf_probe_t perf_probe[] = {
	{ "_Z12Code_ExecuteP9CInstanceS0_P5CCodeP6RValuei", PB_CODE },
	{ "_Z12F_DrawSpriteR6RValueP9CInstanceS2_iPS_", PB_SPRITE },
	{ "_Z15F_DrawSpriteExtR6RValueP9CInstanceS2_iPS_", PB_SPRITE },
	{ "_Z16F_DrawSpritePartR6RValueP9CInstanceS2_iPS_", PB_SPRITE },
	{ "_Z19F_DrawSpriteGeneralR6RValueP9CInstanceS2_iPS_", PB_SPRITE },
	{ "_Z10F_DrawTextR6RValueP9CInstanceS2_iPS_", PB_TEXT },
	{ "_Z13F_DrawTextExtR6RValueP9CInstanceS2_iPS_", PB_TEXT },
	{ "_Z15F_DrawTextColorR6RValueP9CInstanceS2_iPS_", PB_TEXT },
	{ "_Z18F_DrawTextExtColorR6RValueP9CInstanceS2_iPS_", PB_TEXT },
	{ "_Z21F_DrawTextTransformedR6RValueP9CInstanceS2_iPS_", PB_TEXT },
	{ "_Z13F_DrawSurfaceR6RValueP9CInstanceS2_iPS_", PB_SURFACE },
	{ "_Z16F_DrawSurfaceExtR6RValueP9CInstanceS2_iPS_", PB_SURFACE },
	{ "_Z17F_DrawSurfacePartR6RValueP9CInstanceS2_iPS_", PB_SURFACE },
	{ "_Z22F_DrawSurfaceStretchedR6RValueP9CInstanceS2_iPS_", PB_SURFACE },
	{ "_Z18F_SurfaceSetTargetR6RValueP9CInstanceS2_iPS_", PB_SURFTARGET },
	{ "_Z15F_DrawRectangleR6RValueP9CInstanceS2_iPS_", PB_RECT },
};
#define PERF_NPROBE (sizeof(perf_probe) / sizeof(perf_probe[0]))

// Restore the original code, time just the call, then re-patch.
#define PERF_PROBE_CALL(i, ...) ({ \
	so_hook *h = &perf_probe[i].h; \
	kuKernelCpuUnrestrictedMemcpy((void *)h->addr, h->orig_instr, sizeof(h->orig_instr)); \
	kuKernelFlushCaches((void *)h->addr, sizeof(h->orig_instr)); \
	uint64_t t0 = perf_now(); \
	int r = h->thumb_addr ? ((int(*)())h->thumb_addr)(__VA_ARGS__) : ((int(*)())h->addr)(__VA_ARGS__); \
	perf_probe_us[perf_probe[i].cat] += perf_now() - t0; \
	perf_probe_calls[perf_probe[i].cat]++; \
	kuKernelCpuUnrestrictedMemcpy((void *)h->addr, h->patch_instr, sizeof(h->patch_instr)); \
	kuKernelFlushCaches((void *)h->addr, sizeof(h->patch_instr)); \
	r; })

#define PERF_PROBE_FN(i) \
	int perf_probe_fn##i(void *a, void *b, void *c, int d, void *e) { return PERF_PROBE_CALL(i, a, b, c, d, e); }
PERF_PROBE_FN(0) PERF_PROBE_FN(1) PERF_PROBE_FN(2) PERF_PROBE_FN(3) PERF_PROBE_FN(4) PERF_PROBE_FN(5)
PERF_PROBE_FN(6) PERF_PROBE_FN(7) PERF_PROBE_FN(8) PERF_PROBE_FN(9) PERF_PROBE_FN(10) PERF_PROBE_FN(11)
PERF_PROBE_FN(12) PERF_PROBE_FN(13) PERF_PROBE_FN(14) PERF_PROBE_FN(15)
static void *perf_probe_fns[] = { perf_probe_fn0, perf_probe_fn1, perf_probe_fn2, perf_probe_fn3, perf_probe_fn4,
	perf_probe_fn5, perf_probe_fn6, perf_probe_fn7, perf_probe_fn8, perf_probe_fn9, perf_probe_fn10, perf_probe_fn11,
	perf_probe_fn12, perf_probe_fn13, perf_probe_fn14, perf_probe_fn15 };

void perf_install_hooks(void) {
	if (tune_perf >= 2) {
		for (unsigned i = 0; i < PERF_NPROBE && i < sizeof(perf_probe_fns) / sizeof(perf_probe_fns[0]); i++) {
			uintptr_t addr = so_symbol(&yoyoloader_mod, perf_probe[i].sym);
			if (addr)
				perf_probe[i].h = hook_addr(addr, (uintptr_t)perf_probe_fns[i]);
		}
	}
	void *wrap[PS_COUNT] = { perf_step_begin, perf_step_io, perf_step_update, perf_step_collision, perf_step_draw,
		perf_step_layers, perf_step_finish, perf_step_end, perf_step_audio };
	for (int i = 0; i < PS_COUNT; i++) {
		if (!tune_perf && i != PS_DRAW)
			continue; // frameskip alone only needs the draw step
		uintptr_t addr = so_symbol(&yoyoloader_mod, perf_step_sym[i]);
		if (addr)
			perf_hook[i] = hook_addr(addr, (uintptr_t)wrap[i]);
	}
}

/*
 * Library calls the runner makes (counted with perf=1, cheap: one increment), plus a hardware
 * square root: vsqrt.f64 instead of the C library's sqrt, which may be done in software.
 */
enum { LC_MALLOC, LC_FREE, LC_REALLOC, LC_SQRT, LC_FLOOR, LC_POW, LC_FMOD, LC_TRIG, LC_STRCMP, LC_STRLEN, LC_COUNT };
static const char *perf_lc_name[LC_COUNT] = { "malloc", "free", "realloc", "sqrt", "floor", "pow", "fmod", "sin/cos/atan2", "strcmp", "strlen" };
static uint32_t perf_lc[LC_COUNT];

// libvorbis (linked after libm) still needs the C library's sqrt/sqrtf: keep them referenced here
void *keep_libm_sqrt[2] = { (void *)&sqrt, (void *)&sqrtf };

static inline double fast_sqrt(double x) {
	double r;
	__asm__("vsqrt.f64 %P0, %P1" : "=w"(r) : "w"(x));
	return r;
}
static inline float fast_sqrtf(float x) {
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
	return r;
}
void *lc_malloc(size_t n) { if (tune_perf) perf_lc[LC_MALLOC]++; return vglMalloc(n); }
void lc_free(void *p) { if (tune_perf) perf_lc[LC_FREE]++; vglFree(p); }
void *lc_realloc(void *p, size_t n) { if (tune_perf) perf_lc[LC_REALLOC]++; return vglRealloc(p, n); }
void *lc_calloc(size_t a, size_t b) { if (tune_perf) perf_lc[LC_MALLOC]++; return vglCalloc(a, b); }
double lc_sqrt(double x) { if (tune_perf) perf_lc[LC_SQRT]++; return fast_sqrt(x); }
float lc_sqrtf(float x) { if (tune_perf) perf_lc[LC_SQRT]++; return fast_sqrtf(x); }
double lc_floor(double x) { if (tune_perf) perf_lc[LC_FLOOR]++; return floor(x); }
double lc_pow(double a, double b) { if (tune_perf) perf_lc[LC_POW]++; return pow(a, b); }
double lc_fmod(double a, double b) { if (tune_perf) perf_lc[LC_FMOD]++; return fmod(a, b); }
double lc_sin(double x) { if (tune_perf) perf_lc[LC_TRIG]++; return sin(x); }
double lc_cos(double x) { if (tune_perf) perf_lc[LC_TRIG]++; return cos(x); }
double lc_atan2(double a, double b) { if (tune_perf) perf_lc[LC_TRIG]++; return atan2(a, b); }
int lc_strcmp(const char *a, const char *b) { if (tune_perf) perf_lc[LC_STRCMP]++; return strcmp(a, b); }
size_t lc_strlen(const char *s) { if (tune_perf) perf_lc[LC_STRLEN]++; return strlen(s); }

// Called once per frame with the time spent in the runner's Process and in the buffer swap.
void perf_frame(uint64_t process_us, uint64_t swap_us) {
	static uint64_t start, sum_process, sum_swap, sum_gl, max_frame, last, sum_step[PS_COUNT];
	static uint32_t frames, draws, buffers, buffer_bytes, uploads;
	for (int i = 0; i < PS_COUNT; i++) {
		sum_step[i] += perf_step_us[i];
		perf_step_us[i] = 0;
	}
	uint64_t now = perf_now();
	if (!start)
		start = last = now;
	uint64_t frame = now - last;
	last = now;
	if (frame > max_frame)
		max_frame = frame;
	sum_process += process_us;
	sum_swap += swap_us;
	sum_gl += perf_gl_us;
	draws += perf_draws;
	buffers += perf_buffers;
	buffer_bytes += perf_buffer_bytes;
	uploads += perf_tex_uploads;
	perf_gl_us = 0;
	perf_draws = perf_buffers = perf_buffer_bytes = perf_tex_uploads = 0;
	if (++frames < 120)
		return;
	uint64_t elapsed = now - start;
	char line[2048];
	int len = snprintf(line, sizeof(line),
		"[PERF] pool=%dKB vsync=%d | %.1f fps | per frame: total %.1f ms, runner Process %.1f ms "
		"(of which GL calls %.1f ms), swap %.1f ms, worst frame %.1f ms | %u draws, %u buffer uploads "
		"(%u KB), %u texture uploads\n",
		tune_pool_kb, tune_vsync, frames * 1000000.0 / (double)elapsed, elapsed / 1000.0 / frames,
		sum_process / 1000.0 / frames, sum_gl / 1000.0 / frames, sum_swap / 1000.0 / frames, max_frame / 1000.0,
		draws / frames, buffers / frames, buffer_bytes / frames / 1024, uploads);
	len += snprintf(line + len, sizeof(line) - len, "        steps (ms per frame):");
	for (int i = 0; i < PS_COUNT; i++) {
		len += snprintf(line + len, sizeof(line) - len, " %s %.1f%s", perf_step_name[i], sum_step[i] / 1000.0 / frames,
			i == PS_COUNT - 1 ? "\n" : ",");
		sum_step[i] = 0;
	}
	len += snprintf(line + len, sizeof(line) - len, "        frameskip=%d, drawn %.1f fps | library calls per frame:", tune_frameskip,
		perf_drawn * 1000000.0 / (double)elapsed);
	perf_drawn = 0;
	for (int i = 0; i < LC_COUNT; i++) {
		len += snprintf(line + len, sizeof(line) - len, " %s %u%s", perf_lc_name[i], perf_lc[i] / frames, i == LC_COUNT - 1 ? "\n" : ",");
		perf_lc[i] = 0;
	}
	if (tune_perf >= 2) {
		len += snprintf(line + len, sizeof(line) - len, "        probe (ms and calls per frame):");
		for (int i = 0; i < PB_COUNT; i++) {
			len += snprintf(line + len, sizeof(line) - len, " %s %.1f ms / %u%s", perf_probe_name[i],
				perf_probe_us[i] / 1000.0 / frames, perf_probe_calls[i] / frames, i == PB_COUNT - 1 ? "\n" : ",");
			perf_probe_us[i] = 0;
			perf_probe_calls[i] = 0;
		}
	}
	SceUID fd = sceIoOpen("ux0:data/gms/shared/perf.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, line, len);
		sceIoClose(fd);
	}
	start = now;
	sum_process = sum_swap = sum_gl = max_frame = 0;
	frames = draws = buffers = buffer_bytes = uploads = 0;
}

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

// From https://github.com/kraj/uClibc/blob/master/libc/misc/dirent/scandir.c
int scandir_hook(const char *dir, struct android_dirent ***namelist,
	int (*selector) (const struct dirent *),
	int (*compar) (const struct dirent **, const struct dirent **))
{
	DIR *dp = opendir (dir);
	struct dirent *current;
	struct android_dirent d;
	struct android_dirent *android_current = &d;
	struct android_dirent **names = NULL;
	size_t names_size = 0, pos;
	//int save;

	if (dp == NULL)
		return -1;

	//save = errno;
	//__set_errno (0);

	pos = 0;
	while ((current = readdir (dp)) != NULL) {
		int use_it = selector == NULL;
		
		sceClibMemcpy(android_current->d_name, current->d_name, 256);
		android_current->d_type = SCE_S_ISDIR(current->d_stat.st_mode) ? 4 : 8;

		if (! use_it) {	
			use_it = (*selector)(android_current);
			/* The selector function might have changed errno.
			* It was zero before and it need to be again to make
			* the latter tests work.  */
			//if (! use_it)
			//__set_errno (0);
		}
		if (use_it) {
			struct android_dirent *vnew;
			size_t dsize;

			/* Ignore errors from selector or readdir */
			//__set_errno (0);

			if (pos == names_size)
			{
				struct android_dirent **new;
				if (names_size == 0)
					names_size = 10;
				else
					names_size *= 2;
				new = (struct android_dirent **)vglRealloc(names, names_size * sizeof(struct android_dirent*));
				if (new == NULL)
					break;
				names = new;
			}

			dsize = &android_current->d_name[256+1] - (char*)android_current;
			vnew = (struct android_dirent*)vglMalloc(dsize);
			if (vnew == NULL)
				break;

			names[pos++] = (struct android_dirent*)sceClibMemcpy(vnew, android_current, dsize);
		}
	}

	if (errno != 0) {
		//save = errno;
		closedir(dp);
		while (pos > 0)
			vglFree(names[--pos]);
		vglFree(names);
		//__set_errno (save);
		return -1;
	}

	closedir (dp);
	//__set_errno (save);

	/* Sort the list if we have a comparison function to sort with.  */
	if (compar != NULL)
		qsort (names, pos, sizeof(struct android_dirent*), (__compar_fn_t) compar);
	*namelist = names;
	return pos;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
	if (!debugMode)
		return 0;

	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	debugPrintf("[LOG] %s: %s\n", tag, string);
	
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
	if (!debugMode)
		return 0;
	
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	debugPrintf("[LOGV] %s: %s\n", tag, string);

	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

#define  MUTEX_TYPE_NORMAL	 0x0000
#define  MUTEX_TYPE_RECURSIVE  0x4000
#define  MUTEX_TYPE_ERRORCHECK 0x8000

static void init_static_mutex(pthread_mutex_t **mutex)
{
	pthread_mutex_t *mtxMem = NULL;

	switch ((int)*mutex) {
	case MUTEX_TYPE_NORMAL: {
		pthread_mutex_t initTmpNormal = PTHREAD_MUTEX_INITIALIZER;
		mtxMem = vglCalloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpNormal, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_RECURSIVE: {
		pthread_mutex_t initTmpRec = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
		mtxMem = vglCalloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpRec, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_ERRORCHECK: {
		pthread_mutex_t initTmpErr = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
		mtxMem = vglCalloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpErr, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	default:
		break;
	}
}

static void init_static_cond(pthread_cond_t **cond)
{
	if (*cond == NULL) {
		pthread_cond_t initTmp = PTHREAD_COND_INITIALIZER;
		pthread_cond_t *condMem = vglCalloc(1, sizeof(pthread_cond_t));
		sceClibMemcpy(condMem, &initTmp, sizeof(pthread_cond_t));
		*cond = condMem;
	}
}

int pthread_attr_destroy_soloader(pthread_attr_t **attr)
{
	int ret = pthread_attr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_attr_getstack_soloader(const pthread_attr_t **attr,
				   void **stackaddr,
				   size_t *stacksize)
{
	return pthread_attr_getstack(*attr, stackaddr, stacksize);
}

__attribute__((unused)) int pthread_condattr_init_soloader(pthread_condattr_t **attr)
{
	*attr = vglCalloc(1, sizeof(pthread_condattr_t));

	return pthread_condattr_init(*attr);
}

__attribute__((unused)) int pthread_condattr_destroy_soloader(pthread_condattr_t **attr)
{
	int ret = pthread_condattr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_cond_init_soloader(pthread_cond_t **cond,
				   const pthread_condattr_t **attr)
{
	*cond = vglCalloc(1, sizeof(pthread_cond_t));

	if (attr != NULL)
		return pthread_cond_init(*cond, *attr);
	else
		return pthread_cond_init(*cond, NULL);
}

int pthread_cond_destroy_soloader(pthread_cond_t **cond)
{
	int ret = pthread_cond_destroy(*cond);
	free(*cond);
	return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t **cond)
{
	init_static_cond(cond);
	return pthread_cond_signal(*cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t **cond,
					pthread_mutex_t **mutex,
					struct timespec *abstime)
{
	init_static_cond(cond);
	init_static_mutex(mutex);
	return pthread_cond_timedwait(*cond, *mutex, abstime);
}

int pthread_create_soloader(pthread_t **thread,
				const pthread_attr_t **attr,
				void *(*start)(void *),
				void *param)
{
	*thread = vglCalloc(1, sizeof(pthread_t));

	int ret;
	if (attr != NULL) {
		pthread_attr_setstacksize(*attr, 512 * 1024);
		ret = pthread_create(*thread, *attr, start, param);
	} else {
		pthread_attr_t attrr;
		pthread_attr_init(&attrr);
		pthread_attr_setstacksize(&attrr, 512 * 1024);
		ret = pthread_create(*thread, &attrr, start, param);
	}
	if (ret) // e.g. no RAM left for the stack; runners often ignore this and wait forever
		debugPrintf("pthread_create failed (%d) for %p\n", ret, start);
	return ret;

}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t **attr)
{
	*attr = vglCalloc(1, sizeof(pthread_mutexattr_t));

	return pthread_mutexattr_init(*attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t **attr, int type)
{
	return pthread_mutexattr_settype(*attr, type);
}

int pthread_mutexattr_setpshared_soloader(pthread_mutexattr_t **attr, int pshared)
{
	return pthread_mutexattr_setpshared(*attr, pshared);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t **attr)
{
	int ret = pthread_mutexattr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t **mutex)
{
	int ret = pthread_mutex_destroy(*mutex);
	free(*mutex);
	return ret;
}

int pthread_mutex_init_soloader(pthread_mutex_t **mutex,
				const pthread_mutexattr_t **attr)
{
	*mutex = vglCalloc(1, sizeof(pthread_mutex_t));

	if (attr != NULL)
		return pthread_mutex_init(*mutex, *attr);
	else
		return pthread_mutex_init(*mutex, NULL);
}

int pthread_mutex_lock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_lock(*mutex);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_trylock(*mutex);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t **mutex)
{
	return pthread_mutex_unlock(*mutex);
}

/* pthread_rwlock_* for GameMaker 2024 runtimes (issue #160: "Unknown symbol
 * pthread_rwlock_init"). Bionic's pthread_rwlock_t is an opaque block that
 * starts zeroed (PTHREAD_RWLOCK_INITIALIZER); we keep a pointer to our own
 * lock in its first word and create it lazily, like the mutex shims above.
 * Reader-preferring. Waiters poll with a short sleep instead of
 * pthread_cond_wait: the runner takes these locks from native threads (e.g.
 * the OpenSLES playback thread) where pthread-embedded's cancellable wait
 * crashes, since those threads have no pthread data. */
typedef struct {
	pthread_mutex_t m;
	int readers;
	int writer;
} so_rwlock_t;

static pthread_mutex_t so_rwlock_init_lock = PTHREAD_MUTEX_INITIALIZER;

static so_rwlock_t *so_rwlock_get(void **rw)
{
	if (!*rw) {
		pthread_mutex_lock(&so_rwlock_init_lock);
		if (!*rw) {
			so_rwlock_t *l = vglCalloc(1, sizeof(so_rwlock_t));
			pthread_mutex_init(&l->m, NULL);
			*rw = l;
		}
		pthread_mutex_unlock(&so_rwlock_init_lock);
	}
	return (so_rwlock_t *)*rw;
}

int pthread_rwlock_init_soloader(void **rw, const void *attr)
{
	*rw = NULL;
	so_rwlock_get(rw);
	return 0;
}

int pthread_rwlock_destroy_soloader(void **rw)
{
	so_rwlock_t *l = (so_rwlock_t *)*rw;
	if (l) {
		pthread_mutex_destroy(&l->m);
		vglFree(l);
		*rw = NULL;
	}
	return 0;
}

int pthread_rwlock_tryrdlock_soloader(void **rw)
{
	so_rwlock_t *l = so_rwlock_get(rw);
	int ret = 0;
	pthread_mutex_lock(&l->m);
	if (l->writer)
		ret = EBUSY;
	else
		l->readers++;
	pthread_mutex_unlock(&l->m);
	return ret;
}

int pthread_rwlock_rdlock_soloader(void **rw)
{
	while (pthread_rwlock_tryrdlock_soloader(rw))
		sceKernelDelayThread(100);
	return 0;
}

int pthread_rwlock_trywrlock_soloader(void **rw)
{
	so_rwlock_t *l = so_rwlock_get(rw);
	int ret = 0;
	pthread_mutex_lock(&l->m);
	if (l->writer || l->readers)
		ret = EBUSY;
	else
		l->writer = 1;
	pthread_mutex_unlock(&l->m);
	return ret;
}

int pthread_rwlock_wrlock_soloader(void **rw)
{
	while (pthread_rwlock_trywrlock_soloader(rw))
		sceKernelDelayThread(100);
	return 0;
}

int pthread_rwlock_timedrdlock_soloader(void **rw, const void *abstime)
{
	return pthread_rwlock_rdlock_soloader(rw);
}

int pthread_rwlock_timedwrlock_soloader(void **rw, const void *abstime)
{
	return pthread_rwlock_wrlock_soloader(rw);
}

int pthread_rwlock_unlock_soloader(void **rw)
{
	so_rwlock_t *l = so_rwlock_get(rw);
	pthread_mutex_lock(&l->m);
	if (l->writer)
		l->writer = 0;
	else if (l->readers > 0)
		l->readers--;
	pthread_mutex_unlock(&l->m);
	return 0;
}

int pthread_join_soloader(const pthread_t *thread, void **value_ptr)
{
	return pthread_join(*thread, value_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t **cond, pthread_mutex_t **mutex)
{
	return pthread_cond_wait(*cond, *mutex);
}

int pthread_cond_broadcast_soloader(pthread_cond_t **cond)
{
	return pthread_cond_broadcast(*cond);
}

int pthread_attr_init_soloader(pthread_attr_t **attr)
{
	*attr = vglCalloc(1, sizeof(pthread_attr_t));

	return pthread_attr_init(*attr);
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t **attr, int state)
{
	// pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
	return pthread_attr_setdetachstate(*attr, !state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t **attr, size_t stacksize)
{
	return pthread_attr_setstacksize(*attr, stacksize);
}

int pthread_attr_getstacksize_soloader(pthread_attr_t **attr, size_t *stacksize)
{
	return pthread_attr_getstacksize(*attr, stacksize);
}

int pthread_attr_setschedparam_soloader(pthread_attr_t **attr,
					const struct sched_param *param)
{
	return pthread_attr_setschedparam(*attr, param);
}

int pthread_attr_getschedparam_soloader(pthread_attr_t **attr,
					const struct sched_param *param)
{
	return pthread_attr_getschedparam(*attr, param);
}

int pthread_attr_setstack_soloader(pthread_attr_t **attr,
				   void *stackaddr,
				   size_t stacksize)
{
	return pthread_attr_setstack(*attr, stackaddr, stacksize);
}

int pthread_setschedparam_soloader(const pthread_t *thread, int policy,
				   const struct sched_param *param)
{
	return pthread_setschedparam(*thread, policy, param);
}

int pthread_getschedparam_soloader(const pthread_t *thread, int *policy,
				   struct sched_param *param)
{
	return pthread_getschedparam(*thread, policy, param);
}

int pthread_detach_soloader(const pthread_t *thread)
{
	return pthread_detach(*thread);
}

int pthread_getattr_np_soloader(pthread_t* thread, pthread_attr_t *attr) {
	fprintf(stderr, "[WARNING!] Not implemented: pthread_getattr_np\n");
	return 0;
}

int pthread_equal_soloader(const pthread_t *t1, const pthread_t *t2)
{
	if (t1 == t2)
		return 1;
	if (!t1 || !t2)
		return 0;
	return pthread_equal(*t1, *t2);
}


int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

int DebugPrintf(int *target, const char *fmt, ...) {
	if (!debugMode)
		return 0;
	
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	debugPrintf("[DBG] %s\n", string);
	return 0;
}

void main_loop() {
	int (*Java_com_yoyogames_runner_RunnerJNILib_Process) (void *env, int a2, int w, int h, float accel_x, float accel_y, float accel_z, int keypad_open, int orientation, float refresh_rate) = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_Process");
	int (*Java_com_yoyogames_runner_RunnerJNILib_InputResult) (void *env, int a2, char *string, int state, int id) = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_InputResult");
	int (*Java_com_yoyogames_runner_RunnerJNILib_HttpResult) (void *env, int a2, void *result, int responde_code, int id, char *url, void *header) = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_HttpResult");
	int (*Java_com_yoyogames_runner_RunnerJNILib_canFlip) (void) = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_canFlip");
	g_IOFrameCount = (uint32_t *)so_symbol(&yoyoloader_mod, "g_IOFrameCount");
	g_GML_DeltaTime = (int64_t *)so_symbol(&yoyoloader_mod, "g_GML_DeltaTime");
	Audio_GetTrackPos = (void *)so_symbol(&yoyoloader_mod, "_Z17Audio_GetTrackPosi");
	
	int lastX[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
	int lastY[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
	
	setup_ended = 1;
	glReleaseShaderCompiler();
	if (tune_perf || tune_frameskip)
		perf_install_hooks();
	for (;;) {
		if (post_active) {
			SceKernelThreadInfo info;
			info.size = sizeof(SceKernelThreadInfo);
			int res = sceKernelGetThreadInfo(post_thid, &info);
			if (info.status > SCE_THREAD_DORMANT || res < 0) {
				Java_com_yoyogames_runner_RunnerJNILib_HttpResult(fake_env, 0, downloader_mem_buffer, post_response_code, post_index, post_url, downloader_hdr_buffer);
				free(post_url);
				vglFree(downloader_mem_buffer);
				vglFree(downloader_hdr_buffer);
				post_active = 0;
			}
		}
		if (get_active) {
			SceKernelThreadInfo info;
			info.size = sizeof(SceKernelThreadInfo);
			int res = sceKernelGetThreadInfo(get_thid, &info);
			if (info.status > SCE_THREAD_DORMANT || res < 0) {
				Java_com_yoyogames_runner_RunnerJNILib_HttpResult(fake_env, 0, downloader_mem_buffer, get_response_code, get_index, get_url, downloader_hdr_buffer);
				free(get_url);
				vglFree(downloader_mem_buffer);
				vglFree(downloader_hdr_buffer);
				get_active = 0;
			}
		}
		
		SceMotionSensorState sensor;
		sceMotionGetSensorState(&sensor, 1);
		SceMotionState state;
		sceMotionGetState(&state);
		float orientation[3];
		sceMotionGetBasicOrientation(orientation);
		is_portrait = (int)orientation[0];
		if (is_portrait) {
			if (main_tex == 0xDEADBEEF) {
				glGenTextures(1, &main_tex);
				glBindTexture(GL_TEXTURE_2D, main_tex);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_H, SCREEN_W, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				glGenFramebuffers(1, &main_fb);
				glBindFramebuffer(GL_FRAMEBUFFER, main_fb);
				glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, main_tex, 0);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, main_fb);
			glViewport(0, 0, SCREEN_H, SCREEN_W);
			glScissor(0, 0, SCREEN_H, SCREEN_W);
		} else {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, SCREEN_W, SCREEN_H);
			glScissor(0, 0, SCREEN_W, SCREEN_H);
		}
		
		if (*g_IOFrameCount >= 1) {
			GamePadUpdate();
		}
		
		SceTouchData touch;
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
		for (int i = 0; i < SCE_TOUCH_MAX_REPORT; i++) {
			if (i < touch.reportNum) {
				int x, y;
				if (is_portrait) {
					y = (int)((float)touch.report[i].x * (float)SCREEN_W / 1920.0f);
					x = (int)((float)touch.report[i].y * (float)SCREEN_H / 1088.0f);
					if (is_portrait > 0) {
						y = SCREEN_W - y;
					} else {
						x = SCREEN_H - x;
					}
				} else {
					x = (int)((float)touch.report[i].x * (float)SCREEN_W / 1920.0f);
					y = (int)((float)touch.report[i].y * (float)SCREEN_H / 1088.0f);
				}

				if (lastX[i] == -1 || lastY[i] == -1)
					Java_com_yoyogames_runner_RunnerJNILib_TouchEvent(fake_env, 0, TOUCH_DOWN, i, x, y);
				else if (lastX[i] != x || lastY[i] != y)
					Java_com_yoyogames_runner_RunnerJNILib_TouchEvent(fake_env, 0, TOUCH_MOVE, i, x, y);
				lastX[i] = x;
				lastY[i] = y;
			} else {
				if (lastX[i] != -1 || lastY[i] != -1) {
					Java_com_yoyogames_runner_RunnerJNILib_TouchEvent(fake_env, 0, TOUCH_UP, i, lastX[i], lastY[i]);
					lastX[i] = -1;
					lastY[i] = -1;
				}
			}
		}

		tex_lru_frame++;
		if (debugMode) {
			// L + R + START: crash on purpose so the Vita writes a core dump with every thread's state
			SceCtrlData dbg_pad;
			sceCtrlPeekBufferPositive(0, &dbg_pad, 1);
			if ((dbg_pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) {
				debugPrintf("Debug dump requested\n");
				*(volatile int *)0 = 0;
			}
		}
		if (debugMode && tex_lru_frame % 300 == 0) {
			extern unsigned newlib_heap_used(void);
			struct mallinfo mi = mallinfo();
			debugPrintf("[MEM] newlib in use %u KB, peak %u KB / %u KB | vitaGL free: RAM %u KB, VRAM %u KB, PHYCONT %u KB, total %u KB | pages %u KB in %d\n",
				(unsigned)mi.uordblks / 1024, newlib_heap_used() / 1024, _newlib_heap_size / 1024, vglMemFree(VGL_MEM_RAM) / 1024, vglMemFree(VGL_MEM_VRAM) / 1024,
				vglMemFree(VGL_MEM_PHYCONT) / 1024, vglMemFree(VGL_MEM_ALL) / 1024, tex_lru_bytes / 1024, tex_lru_num);
		}
		frame_skipped = 0;
		uint64_t perf_t0 = tune_perf ? perf_now() : 0;
		uint64_t perf_swap_us = 0;
		if (!is_portrait)
			Java_com_yoyogames_runner_RunnerJNILib_Process(fake_env, 0, SCREEN_W, SCREEN_H, sensor.accelerometer.x, sensor.accelerometer.y, sensor.accelerometer.z, 0, 0, 60.0f);
		else
			Java_com_yoyogames_runner_RunnerJNILib_Process(fake_env, 0, SCREEN_H, SCREEN_W, sensor.accelerometer.x, sensor.accelerometer.y, sensor.accelerometer.z, 0, 0x3FF00000, 60.0f);
		uint64_t perf_process_us = tune_perf ? perf_now() - perf_t0 : 0;
		if (!frame_skipped && (!Java_com_yoyogames_runner_RunnerJNILib_canFlip || Java_com_yoyogames_runner_RunnerJNILib_canFlip())) {
			if (is_portrait) {
				int prog;
				glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
				glUseProgram(0);
				glEnable(GL_TEXTURE_2D);
				glEnableClientState(GL_VERTEX_ARRAY);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glViewport(0, 0, SCREEN_W, SCREEN_H);
				glDisable(GL_SCISSOR_TEST);
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
				glMatrixMode(GL_MODELVIEW);
				glLoadIdentity();
				float fb_vertices[] = {
			 SCREEN_W, SCREEN_H, 0,
			        0, SCREEN_H, 0,
			        0,        0, 0,
			 SCREEN_W,        0, 0,
				};
				float fb_texcoords[] = {1, 1, 1, 0, 0, 0, 0, 1};
				float fb_texcoords_flipped[] = {0, 0, 0, 1, 1, 1, 1, 0};
				if (is_portrait > 0)
					glTexCoordPointer(2, GL_FLOAT, 0, fb_texcoords);
				else
					glTexCoordPointer(2, GL_FLOAT, 0, fb_texcoords_flipped);
				glVertexPointer(3, GL_FLOAT, 0, fb_vertices);
				
				glBindTexture(GL_TEXTURE_2D, main_tex);
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
				glDisableClientState(GL_VERTEX_ARRAY);
				glDisableClientState(GL_TEXTURE_COORD_ARRAY);
				glUseProgram(prog);
			}
			if (ime_active) {
				char *r = get_ime_dialog_result();
				if (r) {
					Java_com_yoyogames_runner_RunnerJNILib_InputResult(fake_env, 0, r, 1, ime_index);
					ime_active = 0;
				}
				vglSwapBuffers(GL_TRUE);
			} else if (msg_active) {
				if (get_msg_dialog_result()) {
					Java_com_yoyogames_runner_RunnerJNILib_InputResult(fake_env, 0, "OK", 1, msg_index);
					msg_active = 0;
				}
				vglSwapBuffers(GL_TRUE);
			} else {
				uint64_t perf_s0 = tune_perf ? perf_now() : 0;
				vglSwapBuffers(GL_FALSE);
				if (tune_perf)
					perf_swap_us = perf_now() - perf_s0;
			}
		}
		if (tune_perf)
			perf_frame(perf_process_us, perf_swap_us);
	}
}

void __stack_chk_fail_fake() {
	// Some versions of libyoyo.so apparently stack smash on Startup, with this workaround we prevent the app from crashing
	debugPrintf("Entering main loop after stack smash\n");
	main_loop();
}

double GetPlatform() {
	switch (platTarget) {
	case 1: // Windows
		return 0.0f;
	case 2: // PS4
		return 14.0f;
	default: // Android
		return 4.0f;
	}
}

uint32_t *(*ReadPNGFile) (void *a1, int a2, int *a3, int *a4, int a5);

/* Externalized texture pages are kept under a memory budget: when a new page
 * would exceed it, the least recently bound pages are flushed with the
 * runner's own Graphics::FlushTexture (GL texture deleted, id set to -1), and
 * the runner reloads them from their placeholder the next time they're bound.
 * Recency comes from Graphics::SetTexture, whose PLT slot we redirect. */
#define TEX_LRU_BUDGET (200 * 1024 * 1024) // upper bound; low free memory triggers eviction first
#define TEX_LRU_MAX 512
#define TEX_LRU_MARGIN (16 * 1024 * 1024) // free memory to keep for everything else
typedef struct {
	uint32_t *tex;
	uint32_t gl_id;
	uint32_t bytes;
	uint32_t last_frame;
} tex_lru_entry;
static tex_lru_entry tex_lru[TEX_LRU_MAX];
static void (*Graphics_SetTexture)(int stage, void *tex) = NULL;
static void (*Graphics_FlushTexture)(void *tex) = NULL;

void Graphics_SetTexture_hook(int stage, void *tex) {
	static void *last_tex = NULL;
	static int last_idx = -1;
	if (tex) {
		if (tex == last_tex && last_idx < tex_lru_num && tex_lru[last_idx].tex == tex) {
			tex_lru[last_idx].last_frame = tex_lru_frame;
		} else {
			for (int i = 0; i < tex_lru_num; i++) {
				if (tex_lru[i].tex == tex) {
					tex_lru[i].last_frame = tex_lru_frame;
					last_tex = tex;
					last_idx = i;
					break;
				}
			}
		}
	}
	Graphics_SetTexture(stage, tex);
}

static void tex_lru_remove(int i) {
	tex_lru_bytes -= tex_lru[i].bytes;
	tex_lru[i] = tex_lru[--tex_lru_num];
}

// Returns 1 if anything was flushed (the caller must rebind its texture).
static int tex_lru_make_room(uint32_t *texture, uint32_t bytes) {
	if (!Graphics_FlushTexture)
		return 0;
	int flushed = 0;
	// Freed textures only go back to the pool after vitaGL's garbage collector runs,
	// so count what we release instead of re-reading the free memory
	uint32_t free_mem = vglMemFree(VGL_MEM_ALL), released = 0;
	for (;;) {
		int over_budget = tex_lru_bytes + bytes > TEX_LRU_BUDGET;
		int low_mem = free_mem + released < bytes + TEX_LRU_MARGIN;
		if (!over_budget && !low_mem)
			break;
		int victim = -1;
		for (int i = 0; i < tex_lru_num; i++) {
			if (tex_lru[i].tex == texture)
				continue;
			// Over budget only evicts pages unused for 2+ frames; low memory takes any page
			if (!low_mem && tex_lru[i].last_frame + 1 >= tex_lru_frame)
				continue;
			if (victim < 0 || tex_lru[i].last_frame < tex_lru[victim].last_frame)
				victim = i;
		}
		if (victim < 0)
			break;
		released += tex_lru[victim].bytes;
		uint32_t *t = tex_lru[victim].tex;
		// Only flush if the runner still holds the texture we loaded
		if (t[7] == tex_lru[victim].gl_id) {
			debugPrintf("Evicting texture page %p (%u KB, unused for %u frames)\n", t, tex_lru[victim].bytes / 1024, tex_lru_frame - tex_lru[victim].last_frame);
			Graphics_FlushTexture(t);
			flushed = 1;
		}
		tex_lru_remove(victim);
	}
	return flushed;
}

static void tex_lru_add(uint32_t *texture, uint32_t bytes) {
	if (!Graphics_FlushTexture)
		return;
	for (int i = 0; i < tex_lru_num; i++) {
		if (tex_lru[i].tex == texture) {
			tex_lru_remove(i);
			break;
		}
	}
	if (tex_lru_num == TEX_LRU_MAX)
		return;
	tex_lru[tex_lru_num].tex = texture;
	tex_lru[tex_lru_num].gl_id = texture[7];
	tex_lru[tex_lru_num].bytes = bytes;
	tex_lru[tex_lru_num].last_frame = tex_lru_frame;
	tex_lru_num++;
	tex_lru_bytes += bytes;
}
void (*FreePNGFile) ();
void (*InvalidateTextureState) ();

// Pixel-art games ship huge RGBA texture pages that rarely use more than 256 colours
// (e.g. UFO 50: 58 pages of 2048x2048, ~936MB as RGBA). Uploading those as P8
// paletted textures uses a quarter of the memory and is lossless.
static void upload_rgba_texture(int width, int height, uint32_t *data) {
	if (width * height >= 256 * 256) {
		uint32_t keys[1024];
		uint8_t vals[1024];
		uint8_t used[1024];
		uint32_t palette[256];
		int num_colors = 0;
		sceClibMemset(used, 0, sizeof(used));

		uint8_t *pal_data = vglMalloc(256 * 4 + width * height);
		if (pal_data) {
			uint8_t *idx = pal_data + 256 * 4;
			uint32_t last = data[0] + 1;
			uint8_t last_idx = 0;
			int i, n = width * height;
			for (i = 0; i < n; i++) {
				uint32_t clr = data[i];
				if (clr != last) {
					uint32_t h = (clr * 2654435761u) >> 22;
					while (used[h] && keys[h] != clr)
						h = (h + 1) & 1023;
					if (!used[h]) {
						if (num_colors == 256)
							break;
						used[h] = 1;
						keys[h] = clr;
						vals[h] = num_colors;
						palette[num_colors++] = clr;
					}
					last = clr;
					last_idx = vals[h];
				}
				idx[i] = last_idx;
			}
			if (i == n) {
				sceClibMemset(palette + num_colors, 0, (256 - num_colors) * 4);
				sceClibMemcpy(pal_data, palette, 256 * 4);
				glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, width, height, 0, 256 * 4 + n, pal_data);
				vglFree(pal_data);
				debugPrintf("Uploaded %dx%d texture as P8 (%d colors)\n", width, height, num_colors);
				return;
			}
			vglFree(pal_data);
		}
	}
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

void LoadTextureFromPNG_generic(uint32_t arg1, uint32_t arg2, uint32_t *flags, uint32_t *tex_id, uint32_t *texture) {
	int width, height;
	uint32_t *data;
	// tools/gm_textures.py placeholders are PNGs whose IHDR has the real page size (so the
	// runner computes sprite UVs right) plus a private "yyLd" chunk with the page index.
	// They're never decoded: the page comes straight from <data>/assets/<idx>.pvr.
	static uint32_t page_placeholder[2];
	uint8_t *blob = (uint8_t *)arg1;
	int is_page = blob && arg2 >= 49 && !memcmp(blob + 37, "yyLdYYLP", 8);
	if (is_page) {
		uint32_t idx;
		sceClibMemcpy(&idx, blob + 45, 4);
		page_placeholder[0] = 0xFFBEADDE;
		page_placeholder[1] = 0xFF000000 | idx;
		data = page_placeholder;
		width = 2;
		height = 1;
	} else {
		data = ReadPNGFile(arg1 , arg2, &width, &height, (*flags & 2) == 0);
	}
	if (data) {
		InvalidateTextureState();
		glGenTextures(1, tex_id);
		glBindTexture(GL_TEXTURE_2D, *tex_id);
		if (width == 2 && height == 1) {
			if (data[0] == 0xFFBEADDE) {
				uint32_t *ext_data;
				uint32_t idx = (data[1] << 8) >> 8;
				char fname[256];
#ifdef STANDALONE_MODE
				sprintf(fname, "app0:assets/%u.pvr", idx);
#else
				sprintf(fname, "%s%u.pvr", data_path, idx);
#endif
				// tools/gm_textures.py pages (P8 / RGBA8): sceIo only, so a full newlib heap can't make
				// the open fail (fopen needs malloc) and turn the page into its 2x1 placeholder
				int handled = 0;
				ext_data = NULL;
				SceUID pfd = sceIoOpen(fname, SCE_O_RDONLY, 0);
				if (pfd >= 0) {
					uint32_t hdr[13];
					if (sceIoRead(pfd, hdr, sizeof(hdr)) == sizeof(hdr) && (hdr[2] == 0x100 || hdr[2] == 0x101 || (hdr[2] == 0x0B && hdr[12] == 0)) && hdr[3] == 0) {
						handled = 1;
						uint32_t format = hdr[2];
						height = hdr[6];
						width = hdr[7];
						uint32_t size = sceIoLseek32(pfd, 0, SCE_SEEK_END) - sizeof(hdr) - hdr[12];
						sceIoLseek32(pfd, sizeof(hdr) + hdr[12], SCE_SEEK_SET);
						debugPrintf("Loading page %s (%ux%u %s) [vitaGL free %u KB, pages %u KB in %d]\n", fname, width, height, format == 0x100 ? "P8" : (format == 0x0B ? "DXT5" : "RGBA"), vglMemFree(VGL_MEM_ALL) / 1024, tex_lru_bytes / 1024, tex_lru_num);
						if (tex_lru_make_room(texture, size))
							glBindTexture(GL_TEXTURE_2D, *tex_id);
						// Read into a temporary buffer and let vitaGL copy it (the same upload path
						// the runner-decoded pages use)
						int ok = 0;
						void *tmp = vglMalloc(size);
						if (tmp && sceIoRead(pfd, tmp, size) == size) {
							if (format == 0x0B)
								glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width, height, 0, size, tmp);
							else if (format == 0x100)
								glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, width, height, 0, size, tmp);
							else
								glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
							ok = vglGetTexDataPointer(GL_TEXTURE_2D) != NULL;
						}
						if (tmp)
							vglFree(tmp);
						if (ok) {
							tex_lru_add(texture, size);
						} else {
							// Leave the texture invalid so the runner retries it on a later frame
							debugPrintf("Could not load %s (%u KB, vitaGL free %u KB), retrying later\n", fname, size / 1024, vglMemFree(VGL_MEM_ALL) / 1024);
							glDeleteTextures(1, tex_id);
							*tex_id = 0xFFFFFFFF;
						}
					}
					sceIoClose(pfd);
				}
				FILE *f = handled ? NULL : fopen(fname, "rb");
				if (handled) {
				} else if (f) {
					debugPrintf("Loading externalized texture %s (Raw ID: 0x%X) [vitaGL free %u KB, pages %u KB in %d]\n", fname, data[1], vglMemFree(VGL_MEM_ALL) / 1024, tex_lru_bytes / 1024, tex_lru_num);
					fseek(f, 0, SEEK_END);
					uint32_t size = ftell(f) - 0x34;
					uint32_t metadata_size;
					fseek(f, 0x08, SEEK_SET);
					uint64_t format;
					fread(&format, 1, 8, f);
					fseek(f, 0x18, SEEK_SET);
					fread(&height, 1, 4, f);
					fread(&width, 1, 4, f);
					fseek(f, 0x30, SEEK_SET);
					fread(&metadata_size, 1, 4, f);
					size -= metadata_size;
					if (tex_lru_make_room(texture, size))
						glBindTexture(GL_TEXTURE_2D, *tex_id);
					fseek(f, metadata_size, SEEK_CUR);
					if (format == 0x100 || format == 0x101) {
						// tools/gm_textures.py pages: read straight into texture memory (no temp copy)
						void *pal = NULL, *px = NULL;
						if (format == 0x100) {
							glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, width, height, 0, size, NULL);
							SceGxmTexture *gxm_tex = vglGetGxmTexture(GL_TEXTURE_2D);
							pal = gxm_tex ? sceGxmTextureGetPalette(gxm_tex) : NULL;
						} else {
							glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
						}
						px = vglGetTexDataPointer(GL_TEXTURE_2D);
						if (px && (format == 0x101 || pal)) {
							if (pal)
								fread(pal, 1, 256 * 4, f);
							fread(px, 1, format == 0x100 ? size - 256 * 4 : size, f);
							tex_lru_add(texture, size);
						} else {
							// Out of memory: leave the texture invalid so the runner retries once freed pages are gone
							debugPrintf("Out of memory for %s (%u KB, free %u KB), retrying later\n", fname, size / 1024, vglMemFree(VGL_MEM_ALL) / 1024);
							glDeleteTextures(1, tex_id);
							*tex_id = 0xFFFFFFFF;
						}
						fclose(f);
						ext_data = NULL;
					} else {
					tex_lru_add(texture, size);
					ext_data = vglMalloc(size);
					fread(ext_data, 1, size, f);
					fclose(f);
					}
					switch (ext_data ? format : 0xFFFF) {
					case 0xFFFF:
						break;
					case 0x00:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG, width, height, 0, size, ext_data);
						break;
					case 0x01:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG, width, height, 0, size, ext_data);
						break;
					case 0x02:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, width, height, 0, size, ext_data);
						break;
					case 0x03:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, width, height, 0, size, ext_data);
						break;
					case 0x04:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG, width, height, 0, size, ext_data);
						break;
					case 0x05:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG, width, height, 0, size, ext_data);
						break;
					case 0x06:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, width, height, 0, size, ext_data);
						break;
					case 0x07:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, width, height, 0, size, ext_data);
						break;
					case 0x09:
						glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, width, height, 0, size, ext_data);
						break;
					case 0x0B:
						if (metadata_size == 4) { // Load DXT5 as pre-swizzled
							glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
							SceGxmTexture *gxm_tex = vglGetGxmTexture(GL_TEXTURE_2D);
							vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
							void *tex_data = vglForceAlloc(size);
							sceClibMemcpy(tex_data, ext_data, size);
							sceGxmTextureInitSwizzledArbitrary(gxm_tex, tex_data, SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR, width, height, 0);
							vglOverloadTexDataPointer(GL_TEXTURE_2D, tex_data);
						} else
							glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width, height, 0, size, ext_data);
						break;
					default:
						debugPrintf("Unsupported externalized texture format (0x%llX).\n", format);
						break;
					}
				} else {
					debugPrintf("Loading externalized texture %s (Raw ID: 0x%X).\n", fname, data[1]);
#ifdef STANDALONE_MODE
					sprintf(fname, "app0:assets/%u.png", idx);
#else
					sprintf(fname, "%s%u.png", data_path, idx);
#endif
					ext_data = stbi_load(fname, &width, &height, NULL, 4);
					upload_rgba_texture(width, height, ext_data);
				}
				if (ext_data)
					vglFree(ext_data);
			} else {
				upload_rgba_texture(width, height, data);
			}
		} else {
			upload_rgba_texture(width, height, data);
		}
		*flags = *flags | 0x40;
		if (!is_page)
			FreePNGFile();
		texture[0] = 0x06;
		if (flags != &texture[2]) {
			texture[1] = width;
			texture[2] = height;
		} else {
			texture[1] = ((width * *g_TextureScale - 1) | texture[1] & 0xFFFFE000) & 0xFC001FFF | ((height * *g_TextureScale - 1) << 13);
		}
		debugPrintf("Texture size: %dx%d\n", width, height);
	} else {
		debugPrintf("ERROR: Failed to load a PNG texture!\n");
	}
}

void LoadTextureFromPNG_1(uint32_t *texture, int has_mips) {
	LoadTextureFromPNG_generic(texture[23], texture[24], &texture[5], &texture[6], texture);
}

// GMS 2024.x: data/size at +0x60/+0x64, flags at +0x18, texture id at +0x1C
void LoadTextureFromPNG_5(uint32_t *texture, int has_mips) {
	LoadTextureFromPNG_generic(texture[24], texture[25], &texture[6], &texture[7], texture);
}

// GMS 2024.x QOI/bzip2 pages (the runner's own decoder), uploaded as P8 when they have
// <= 256 colours and kept under the page budget. Mirrors LoadTextureFromQOIF otherwise.
uint32_t *(*ReadQOIFFile)(void *data, int size, int *w, int *h, int flag);
void (*FreeQOIFFile)(void *data);
// Debug Mode: compare a runner-decoded page with the matching gm_textures.py .pvr
// (found through assets/pages.idx: blob size + crc32 of its first 256 bytes)
static void compare_page_with_pvr(uint8_t *blob, uint32_t blob_size, uint32_t *rgba, int w, int h) {
	static uint32_t *idx = NULL;
	static int idx_num = -1;
	char fname[256];
	if (idx_num < 0) {
		idx_num = 0;
		sprintf(fname, "%spages.idx", data_path);
		SceUID fd = sceIoOpen(fname, SCE_O_RDONLY, 0);
		if (fd >= 0) {
			int sz = sceIoLseek32(fd, 0, SCE_SEEK_END);
			sceIoLseek32(fd, 0, SCE_SEEK_SET);
			idx = malloc(sz);
			if (idx && sceIoRead(fd, idx, sz) == sz)
				idx_num = sz / 8;
			sceIoClose(fd);
		}
	}
	uint32_t crc = crc32(0, blob, blob_size < 256 ? blob_size : 256);
	int page = -1;
	for (int i = 0; i < idx_num; i++) {
		if (idx[i * 2] == blob_size && idx[i * 2 + 1] == crc) {
			page = i;
			break;
		}
	}
	if (page < 0) {
		debugPrintf("[CMP] no pages.idx match for a %dx%d page\n", w, h);
		return;
	}
	sprintf(fname, "%s%d.pvr", data_path, page);
	SceUID fd = sceIoOpen(fname, SCE_O_RDONLY, 0);
	if (fd < 0) {
		debugPrintf("[CMP] page %d: %s missing\n", page, fname);
		return;
	}
	uint32_t hdr[13];
	sceIoRead(fd, hdr, sizeof(hdr));
	uint32_t n = w * h, mism = 0, first = 0xFFFFFFFF, got_first = 0, want_first = 0;
	uint8_t *buf = vglMalloc(hdr[2] == 0x100 ? 1024 + n : n * 4);
	if (!buf) {
		sceIoClose(fd);
		return;
	}
	sceIoRead(fd, buf, hdr[2] == 0x100 ? 1024 + n : n * 4);
	sceIoClose(fd);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t px = hdr[2] == 0x100 ? ((uint32_t *)buf)[buf[1024 + i]] : ((uint32_t *)buf)[i];
		if (px != rgba[i]) {
			if (!mism) {
				first = i;
				got_first = rgba[i];
				want_first = px;
			}
			mism++;
		}
	}
	vglFree(buf);
	if (mism)
		debugPrintf("[CMP] page %d (%ux%u fmt 0x%X vs %dx%d): %u pixels differ, first at (%u,%u): runner %08X pvr %08X\n",
			page, hdr[7], hdr[6], hdr[2], w, h, mism, first % w, first / w, got_first, want_first);
	else
		debugPrintf("[CMP] page %d: identical to runner decode\n", page);
}

static int LoadTextureFromQOIF_impl(uint32_t *texture, int has_mips);

// Page loads (first load or reload after eviction), timed for the perf log
int LoadTextureFromQOIF_hook(uint32_t *texture, int has_mips) {
	if (tune_perf < 2)
		return LoadTextureFromQOIF_impl(texture, has_mips);
	uint64_t t0 = perf_now();
	int r = LoadTextureFromQOIF_impl(texture, has_mips);
	perf_probe_us[PB_PAGELOAD] += perf_now() - t0;
	perf_probe_calls[PB_PAGELOAD]++;
	return r;
}

static int LoadTextureFromQOIF_impl(uint32_t *texture, int has_mips) {
	// gm_textures.py QOI placeholders (real size in the header, marker at +37): load the .pvr
	uint8_t *blob = (uint8_t *)texture[24];
	if (blob && texture[25] >= 49 && !memcmp(blob + 37, "yyLdYYLP", 8)) {
		LoadTextureFromPNG_generic(texture[24], texture[25], &texture[6], &texture[7], texture);
		return texture[7];
	}
	int width, height;
	uint32_t *data = ReadQOIFFile((void *)texture[24], texture[25], &width, &height, (texture[6] & 2) == 0);
	if (!data) {
		debugPrintf("ERROR: Failed to load a QOI texture!\n");
		return 0;
	}
	if (debugMode)
		compare_page_with_pvr((uint8_t *)texture[24], texture[25], data, width, height);
	InvalidateTextureState();
	texture[1] = width;
	texture[2] = height;
	uint32_t bytes = width * height; // P8 estimate for the budget
	tex_lru_make_room(texture, bytes);
	glGenTextures(1, &texture[7]);
	glBindTexture(GL_TEXTURE_2D, texture[7]);
	upload_rgba_texture(width, height, data);
	texture[6] |= 0x40;
	FreeQOIFFile(data);
	texture[0] = 6;
	tex_lru_add(texture, bytes);
	debugPrintf("QOI page %dx%d [vitaGL free %u KB, pages %u KB in %d]\n", width, height, vglMemFree(VGL_MEM_ALL) / 1024, tex_lru_bytes / 1024, tex_lru_num);
	return texture[7];
}

void LoadTextureFromPNG_2(uint32_t *texture, int has_mips) {
	LoadTextureFromPNG_generic(texture[11], texture[12], &texture[4], &texture[5], texture);
}

void LoadTextureFromPNG_3(uint32_t *texture) {
	LoadTextureFromPNG_generic(texture[9], texture[10], &texture[2], &texture[3], texture);
}

void LoadTextureFromPNG_4(uint32_t *texture) {
	LoadTextureFromPNG_generic(texture[8], texture[9], &texture[2], &texture[3], texture);
}

int image_preload_idx = 0;
uint32_t png_get_IHDR_hook(uint32_t *png_ptr, uint32_t *info_ptr, uint32_t *width, uint32_t *height, int *bit_depth, int *color_type, int *interlace_type, int *compression_type, int *filter_type) {
	if (!png_ptr || !info_ptr || !width || !height)
		return 0;
	
	*width = info_ptr[0];
	*height = info_ptr[1];

	if (bit_depth)
		*bit_depth = *((uint8_t *)info_ptr + 24);

	if (color_type)
		*color_type = *((uint8_t *)info_ptr + 25);

	if (compression_type)
		*compression_type = *((uint8_t *)info_ptr + 26);

	if (filter_type)
		*filter_type = *((uint8_t *)info_ptr + 27);

	if (interlace_type)
		*interlace_type = *((uint8_t *)info_ptr + 28);

	if (!setup_ended && *width == 2 && *height == 1) {
		char fname[256];
#ifdef STANDALONE_MODE
		sprintf(fname, "app0:assets/%d.pvr", image_preload_idx);
#else
		sprintf(fname, "%s%d.pvr", data_path, image_preload_idx);
#endif
		FILE *f = fopen(fname, "rb");
		if (f) {
			fseek(f, 0x18, SEEK_SET);
			fread(height, 1, 4, f);
			fread(width, 1, 4, f);
			fclose(f);
		} else {
#ifdef STANDALONE_MODE
			sprintf(fname, "app0:assets/%d.pvr", image_preload_idx);
#else
			sprintf(fname, "%s%d.png", data_path, image_preload_idx);
#endif
			int dummy;
			stbi_info(fname, width, height, &dummy);
		}
		image_preload_idx++;
	}
	return 1;
}

void SetWorkingDirectory() {
	// This is the smallest to reimplement function after ProcessCommandLine where we can disable audio if required
	if (disableAudio)
		*g_fNoAudio = 1;
	
	if (!*g_pWorkingDirectory)
		*g_pWorkingDirectory = strdup("assets/");
}

// Some data files (e.g. GMS 2024.8 builds) store shader entries at offsets that
// aren't 4-byte aligned. Shader_Load reads them with LDMIB, which faults on Vita
// (Android kernels silently fix unaligned LDM up). Swap that block for plain LDRs.
static const uint32_t shader_load_ldm[21] = {
	0xE590601C, 0xE590A014, 0xE3560000, 0xE590400C, 0x10866007, 0xE35A0000, 0xE9900006,
	0x108AA007, 0xE3540000, 0x10844007, 0xE58D2028, 0xE5902010, 0xE3510000, 0xE58D2024,
	0x10811007, 0xE5902018, 0xE5900020, 0xE58D2020, 0xE58D001C, 0xE58D102C, 0xEA000019
};
static const uint32_t shader_load_ldr[21] = {
	0xE590601C, 0xE3560000, 0x10866007, 0xE590A014, 0xE35A0000, 0x108AA007, 0xE590400C,
	0xE3540000, 0x10844007, 0xE5901020, 0xE5902018, 0xE590C010, 0xE590E008, 0xE5900004,
	0xE3500000, 0x10800007, 0xE58D002C, 0xE28D001C, 0xE8805006, 0xEA00001A, 0xE320F000
};

void patch_shader_load_alignment(void) {
	uint32_t *func = (uint32_t *)so_symbol(&yoyoloader_mod, "_Z11Shader_LoadPhjS_");
	if (!func || ((uintptr_t)func & 1))
		return;
	for (int i = 0; i < 0x200; i++) {
		if (!memcmp(&func[i], shader_load_ldm, sizeof(shader_load_ldm))) {
			debugPrintf("Patching Shader_Load unaligned LDM at 0x%08X\n", (uintptr_t)&func[i]);
			kuKernelCpuUnrestrictedMemcpy(&func[i], shader_load_ldr, sizeof(shader_load_ldr));
			return;
		}
	}
}

void patch_runner(void) {
	patch_shader_load_alignment();

	FreePNGFile = so_symbol(&yoyoloader_mod, "_Z11FreePNGFilev");
	ReadPNGFile = so_symbol(&yoyoloader_mod, "_Z11ReadPNGFilePviPiS0_b");
	InvalidateTextureState = so_symbol(&yoyoloader_mod, "_Z23_InvalidateTextureStatev");
	
	hook_addr(so_symbol(&yoyoloader_mod, "png_get_IHDR"), (uintptr_t)&png_get_IHDR_hook);
	hook_addr(so_symbol(&yoyoloader_mod, "_Z19SetWorkingDirectoryv"), (uintptr_t)&SetWorkingDirectory);
	
	uint8_t has_mips = 1;
	uint32_t *LoadTextureFromPNG = (uint32_t *)so_symbol(&yoyoloader_mod, "_Z18LoadTextureFromPNGP7Texture10eMipEnable");
	if (!LoadTextureFromPNG) {
		LoadTextureFromPNG = (uint32_t *)so_symbol(&yoyoloader_mod, "_Z18LoadTextureFromPNGP7Texture");
		has_mips = 0;
	}
	
	debugPrintf("LoadTextureFromPNG has signature: 0x%X\n", *LoadTextureFromPNG);
	if (!has_mips) {
		uint32_t *p = LoadTextureFromPNG;
		for (;;) {
			if (*p == 0xE5900020) { // LDR R0, [R0,#0x20]
				debugPrintf("Patching LoadTextureFromPNG to variant #4\n");
				hook_addr(LoadTextureFromPNG, (uintptr_t)&LoadTextureFromPNG_4);
				break;
			} else if (*p == 0xE5900024) { // LDR R0, [R0,#0x24]
				debugPrintf("Patching LoadTextureFromPNG to variant #3\n");
				hook_addr(LoadTextureFromPNG, (uintptr_t)&LoadTextureFromPNG_3);
				break;
			}
			p++;
		}
	} else {
		int is_2024 = 0;
		for (int i = 0; i < 32; i++) {
			if (LoadTextureFromPNG[i] == 0xE1C466D0) { // LDRD R6, R7, [R4,#0x60]
				is_2024 = 1;
				break;
			}
		}
		switch (is_2024 ? 0 : (*LoadTextureFromPNG >> 16)) {
		case 0:
			debugPrintf("Patching LoadTextureFromPNG to variant #5\n");
			hook_addr(LoadTextureFromPNG, (uintptr_t)&LoadTextureFromPNG_5);
			// Texture page budget (see tex_lru_make_room); relies on the 2024 Texture layout
			Graphics_SetTexture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics10SetTextureEiPv");
			if (Graphics_SetTexture && so_redirect_plt(&yoyoloader_mod, "_ZN8Graphics10SetTextureEiPv", (uintptr_t)&Graphics_SetTexture_hook))
				Graphics_FlushTexture = (void *)so_symbol(&yoyoloader_mod, "_ZN8Graphics12FlushTextureEPv");
			ReadQOIFFile = (void *)so_symbol(&yoyoloader_mod, "_Z12ReadQOIFFilePviPiS0_b");
			FreeQOIFFile = (void *)so_symbol(&yoyoloader_mod, "_Z12FreeQOIFFilePh");
			if (ReadQOIFFile && FreeQOIFFile)
				hook_addr(so_symbol(&yoyoloader_mod, "_Z19LoadTextureFromQOIFP7Texture10eMipEnable"), (uintptr_t)&LoadTextureFromQOIF_hook);
			break;
		case 0xE92D:
			debugPrintf("Patching LoadTextureFromPNG to variant #1\n");
			hook_addr(LoadTextureFromPNG, (uintptr_t)&LoadTextureFromPNG_1);
			break;
		case 0xE590:
			debugPrintf("Patching LoadTextureFromPNG to variant #2\n");
			hook_addr(LoadTextureFromPNG, (uintptr_t)&LoadTextureFromPNG_2);
			break;
		default:
			fatal_error("Error: Unrecognized LoadTextureFromPNG signature: 0x%08X.", *LoadTextureFromPNG);
			break;
		}
	}

	hook_addr(so_symbol(&yoyoloader_mod, "_ZN9DbgServer4InitEv"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&yoyoloader_mod, "_ZN9DbgServerC2Eb"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&yoyoloader_mod, "_ZN9DbgServerD2Ev"), (uintptr_t)&ret0);
	
	hook_addr(so_symbol(&yoyoloader_mod, "_Z30PackageManagerHasSystemFeaturePKc"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&yoyoloader_mod, "_Z17alBufferDebugNamejPKc"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&yoyoloader_mod, "_ZN13MemoryManager10DumpMemoryEP7__sFILE"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&yoyoloader_mod, "_ZN13MemoryManager10DumpMemoryEPvS0_"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&yoyoloader_mod, "_ZN13MemoryManager10DumpMemoryEPvS0_b"), (uintptr_t)&ret0);

	hook_addr(so_symbol(&yoyoloader_mod, "_Z23YoYo_GetPlatform_DoWorkv"), (uintptr_t)&GetPlatform);
	hook_addr(so_symbol(&yoyoloader_mod, "_Z20GET_YoYo_GetPlatformP9CInstanceiP6RValue"), (uintptr_t)&GetPlatformInstance);
	
	so_symbol_fix_ldmia(&yoyoloader_mod, "_Z11Shader_LoadPhjS_");
	so_symbol_fix_ldmia(&yoyoloader_mod, "_Z10YYGetInt32PK6RValuei");

	if (debugMode) {
		hook_addr(so_symbol(&yoyoloader_mod, "_ZN11TRelConsole6OutputEPKcz"), (uintptr_t)&DebugPrintf);
		hook_addr(so_symbol(&yoyoloader_mod, "_ZN17TErrStreamConsole6OutputEPKcz"), (uintptr_t)&DebugPrintf);
		hook_addr(so_symbol(&yoyoloader_mod, "_Z7YYErrorPKcz"), (uintptr_t)&debugPrintf);
	} else {
		hook_addr(so_symbol(&yoyoloader_mod, "_ZN11TRelConsole6OutputEPKcz"), (uintptr_t)&ret0);
		hook_addr(so_symbol(&yoyoloader_mod, "_ZN17TErrStreamConsole6OutputEPKcz"), (uintptr_t)&ret0);
		hook_addr(so_symbol(&yoyoloader_mod, "_Z7YYErrorPKcz"), (uintptr_t)&ret0);
	}
}

void patch_runner_post_init(void) {
	g_fNoAudio = (uint8_t *)so_symbol(&yoyoloader_mod, "g_fNoAudio");
	g_pWorkingDirectory = (char *)so_symbol(&yoyoloader_mod, "g_pWorkingDirectory");
	g_TextureScale = (int *)so_symbol(&yoyoloader_mod, "g_TextureScale");
	
	int *dbg_csol = (int *)so_symbol(&yoyoloader_mod, "_dbg_csol");
	if (dbg_csol) {
		kuKernelCpuUnrestrictedMemcpy((void *)(*(int *)so_symbol(&yoyoloader_mod, "_dbg_csol") + 0x0C), (void *)(so_symbol(&yoyoloader_mod, "_ZTV11TRelConsole") + 0x14), 4);
		kuKernelCpuUnrestrictedMemcpy((void *)(*(int *)so_symbol(&yoyoloader_mod, "_rel_csol") + 0x0C), (void *)(so_symbol(&yoyoloader_mod, "_ZTV11TRelConsole") + 0x14), 4);
	}
}

extern void *_Znaj;
extern void *_Znwj;
extern void *_ZdlPv;
extern void *_ZdaPv;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZNSt12length_errorD1Ev;
extern void *_ZNSt13runtime_errorD1Ev;
extern void *_ZTVSt12length_error;
extern void *__aeabi_memclr;
extern void *__aeabi_memclr4;
extern void *__aeabi_memclr8;
extern void *__aeabi_memcpy4;
extern void *__aeabi_memcpy8;
extern void *__aeabi_memmove4;
extern void *__aeabi_memmove8;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_atexit;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__aeabi_f2d;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_d2uiz;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_ui2d;
extern void *__aeabi_ul2d;
extern void *__aeabi_ddiv;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmplt;
extern void *__aeabi_dmul;
extern void *__aeabi_dsub;
extern void *__aeabi_dcmpge;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_i2d;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__cxa_allocate_exception;
extern void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *));
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;

char __progname[32] = {0};
int __page_size = 0;

int open(const char *pathname, int flags);

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

size_t __ctype_get_mb_cur_max() {
	return 1;
}

static FILE __sF_fake[0x100][3];

int stat_hook(const char *pathname, void *statbuf) {
	struct stat st;
	int res = stat(pathname, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

typedef struct {
	uint8_t *buf;
	off_t sz;
	int offs;
} AAssetHandle;

AAssetHandle *AAssetManager_open(unzFile apk_file, const char *fname, int mode) {
	debugPrintf("AAssetManager_open %s\n", fname);
	char path[256];
	sprintf(path, "assets/%s", fname);
	int res = unzLocateFile(apk_file, path, NULL);
	if (res != UNZ_OK)
		return NULL;
	AAssetHandle *ret = (AAssetHandle *)vglMalloc(sizeof(AAssetHandle));
	unz_file_info file_info;
	unzGetCurrentFileInfo(apk_file, &file_info, NULL, 0, NULL, 0, NULL, 0);
	ret->sz = file_info.uncompressed_size;
	ret->buf = (uint8_t *)vglMalloc(ret->sz);
	ret->offs = 0;
	unzOpenCurrentFile(apk_file);
	unzReadCurrentFile(apk_file, ret->buf, ret->sz);
	unzCloseCurrentFile(apk_file);
	return ret;
}

void AAsset_close(AAssetHandle *f) {
	if (f) {
		vglFree(f->buf);
		vglFree(f);
	}
}

unzFile AAssetManager_fromJava(void *env, void *obj) {
	return unzOpen(apk_path);
}

int AAsset_read(AAssetHandle *f, void *buf, size_t count) {
	int read_count = (f->offs + count) > f->sz ? (f->sz - f->offs) : count;
	sceClibMemcpy(buf, &f->buf[f->offs], read_count);
	f->offs += read_count;
	return read_count;
}

off_t AAsset_seek(AAssetHandle *f, off_t offs, int whence) {
	switch (whence) {
	case SEEK_SET:
		f->offs = offs;
		break;
	case SEEK_END:
		f->offs = f->sz + offs;
		break;
	case SEEK_CUR:
		f->offs += offs;
		break;
	}
	return f->offs;
}

off_t AAsset_getLength(AAssetHandle *f) {
	return f->sz;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

void *dlopen_hook(const char *filename, int flags) {
	debugPrintf("Opening %s\n", filename);
	if (forceGL1 && strstr(filename, "v2"))
		return NULL;
#if 0	
	if (!strcmp(filename, "libOpenSLES.so"))
		return NULL;
#endif
	return (void *)0xDEADBEEF;
}

void glTexParameteriHook(GLenum target, GLenum pname, GLint param) {
	if (forceBilinear && (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)) {
		param = GL_LINEAR;
	}
	glTexParameteri(target, pname, param);
}

void glTexParameterfHook(GLenum target, GLenum pname, GLfloat param) {
	if (forceBilinear && (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)) {
		param = GL_LINEAR;
	}
	glTexParameteri(target, pname, param);
}

void *retJNI(int dummy) {
	return fake_env;
}

void glBindFramebufferHook(GLenum target, GLuint framebuffer) {
	if (!framebuffer && is_portrait) {
		framebuffer = main_fb;
	}
	if (debugMode && tex_lru_frame % 600 < 3) // clear/target diagnostics, see glClearHook
		debugPrintf("[FBO] frame %u bind %u\n", tex_lru_frame, framebuffer);
	glBindFramebuffer(target, framebuffer);
}

const char *gl_ret0[] = {
	"glDeleteRenderbuffers",
	"glDiscardFramebufferEXT",
	"glFramebufferRenderbuffer",
	"glGenRenderbuffers",
	"glGetError",
	"glBindRenderbuffer",
	"glHint",
	"glLightf",
	"glMaterialx",
	"glNormalPointer",
	"glPixelStorei",
	"glRenderbufferStorage",
	"glShadeModel",
};
static size_t gl_numret = sizeof(gl_ret0) / sizeof(*gl_ret0);

void glReadPixelsHook(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *data) {
	if (deltarune_hack)
		glFinish();
	glReadPixels(x, y, width, height, format, type, data);
}

// Debug Mode: every 600 frames, log the clears of the next 3 frames (target, scissor, viewport)
void glClearHook(GLbitfield mask) {
	if (debugMode && tex_lru_frame % 600 < 3) {
		GLint fb = 0, sc[4] = {0}, vp[4] = {0};
		GLfloat cc[4] = {0};
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
		glGetIntegerv(GL_SCISSOR_BOX, sc);
		glGetIntegerv(GL_VIEWPORT, vp);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
		debugPrintf("[CLR] frame %u mask 0x%X fb %d scissor %d (%d,%d %dx%d) viewport (%d,%d %dx%d) color %.2f %.2f %.2f %.2f\n",
			tex_lru_frame, mask, fb, glIsEnabled(GL_SCISSOR_TEST), sc[0], sc[1], sc[2], sc[3], vp[0], vp[1], vp[2], vp[3], cc[0], cc[1], cc[2], cc[3]);
	}
	glClear(mask);
}

void glShaderSourceHook(GLuint shader, GLsizei count, const GLchar **string, const GLint *length) {
	if (debugShaders) {
		char glsl_path[256];
		static int shader_idx = 0;
		snprintf(glsl_path, sizeof(glsl_path), "%s/%d.glsl", GLSL_PATH, shader_idx++);
		FILE *file = fopen(glsl_path, "w");
		fprintf(file, "%s", *string);
		fclose(file);
	}
	
	glShaderSource(shader, count, string, length);
}

static so_default_dynlib gl_hook[] = {
	{"glTexParameterf", (uintptr_t)&glTexParameterfHook},
	{"glTexParameteri", (uintptr_t)&glTexParameteriHook},
	{"glBindFramebuffer", (uintptr_t)&glBindFramebufferHook},
	{"glClear", (uintptr_t)&glClearHook},
	{"glReadPixels", (uintptr_t)&glReadPixelsHook},
	{"glShaderSource", (uintptr_t)&glShaderSourceHook},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

void *dlsym_hook( void *handle, const char *symbol);

FILE *fopen_hook(char *file, char *mode) {
	char *s = strstr(file, "/ux0:");
	if (s)
		file = s + 1;
	else {
		s = strstr(file, "ux0:");
		if (!s) {
#ifdef STANDALONE_MODE
			s = strstr(file, "app0:");
			if (!s)
#endif
			{
#ifdef STANDALONE_MODE
				FILE *f = NULL;
				if (mode[0] != 'w') {
					char patched_fname[256];
					sprintf(patched_fname, "app0:%s", file);
					f = fopen(patched_fname, mode);
				}
				
				if (f)
					return f;
#endif
				char patched_fname[256];
				sprintf(patched_fname, "%s%s", data_path_root, file);
				return fopen(patched_fname, mode);
			}
		}
	}
	if (mode[0] == 'w')
		recursive_mkdir(file);
	return fopen(file, mode);
}

void *sceClibMemclr(void *dst, SceSize len) {
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

#define IS_LEAP(n) ((!(((n) + 1900) % 400) || (!(((n) + 1900) % 4) && (((n) + 1900) % 100))) != 0)
#define days_in_gregorian_cycle ((365 * 400) + 100 - 4 + 1)
static const int length_of_year[2] = { 365, 366 };
static const int julian_days_by_month[2][12] = {
	{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
	{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335},
};

struct tm *localtime64(const int64_t *time) {
	time_t _t = *time;
	return localtime(&_t);
}

struct tm *gmtime64(const int64_t *time) {
	time_t _t = *time;
	return gmtime(&_t);
}

int64_t mktime64(struct tm *time) {
	return mktime(time);
}

int64_t timegm64(const struct tm *date) {
	int64_t days = 0;
	int64_t seconds = 0;
	int64_t year;
	int64_t orig_year = (int64_t)date->tm_year;
	int cycles  = 0;
	if( orig_year > 100 ) {
		cycles = (orig_year - 100) / 400;
		orig_year -= cycles * 400;
		days += (int64_t)cycles * days_in_gregorian_cycle;
	}
	else if( orig_year < -300 ) {
		cycles = (orig_year - 100) / 400;
		orig_year -= cycles * 400;
		days += (int64_t)cycles * days_in_gregorian_cycle;
	}

	if( orig_year > 70 ) {
		year = 70;
		while( year < orig_year ) {
			days += length_of_year[IS_LEAP(year)];
			year++;
		}
	}
	else if ( orig_year < 70 ) {
		year = 69;
		do {
			days -= length_of_year[IS_LEAP(year)];
			year--;
		} while( year >= orig_year );
	}
	days += julian_days_by_month[IS_LEAP(orig_year)][date->tm_mon];
	days += date->tm_mday - 1;
	seconds = days * 60 * 60 * 24;
	seconds += date->tm_hour * 60 * 60;
	seconds += date->tm_min * 60;
	seconds += date->tm_sec;
	return seconds;
}

int is_prime(int n) {
	if (n <= 3)
		return 1;
	
	if (n % 2 == 0 || n % 3 == 0)
		return 0;
	
	for (int i = 5; i * i <= n; i = i + 6) {
		if (n % i == 0 || n % (i + 2) == 0)
			return 0;
	}
	
	return 1;
}

int _ZNSt6__ndk112__next_primeEj(void *this, int n) {
	if (n <= 1)
		return 2;
	
	while (!is_prime(n)) {
		n++;
	}
	
	return n;
}

void __cxa_throw_hook(void *thrown_exception, void *tinfo, void (*dest)(void *)) {
	if (tinfo == so_symbol(&yoyoloader_mod, "_ZTI14YYGMLException")) {
		void (* YYCatchGMLException)(void *exception) = so_symbol(&yoyoloader_mod, "_Z19YYCatchGMLExceptionRK14YYGMLException");
		YYCatchGMLException(thrown_exception);
		if (dest)
			dest(thrown_exception);
	} else
		__cxa_throw(thrown_exception, tinfo, dest);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return vglMalloc(length);
}

int munmap(void *addr, size_t length) {
	vglFree(addr);
	return 0;
}

int nanosleep_hook(const struct timespec *req, struct timespec *rem) {
	const uint32_t usec = req->tv_sec * 1000 * 1000 + req->tv_nsec / 1000;
	return sceKernelDelayThreadCB(usec);
}

size_t __strlen_chk(const char *s, size_t s_len) {
	return strlen(s);
}

int __vsprintf_chk(char* dest, int flags, size_t dest_len_from_compiler, const char *format, va_list va) {
	return vsprintf(dest, format, va);
}

void *__memmove_chk(void *dest, const void *src, size_t len, size_t dstlen) {
	return memmove(dest, src, len);
}

void *__memset_chk(void *dest, int val, size_t len, size_t dstlen) {
	return memset(dest, val, len);
}

size_t __strlcat_chk (char *dest, char *src, size_t len, size_t dstlen) {
	return strlcat(dest, src, len);
}

size_t __strlcpy_chk (char *dest, char *src, size_t len, size_t dstlen) {
	return strlcpy(dest, src, len);
}

char* __strchr_chk(const char* p, int ch, size_t s_len) {
	return strchr(p, ch);
}

char *__strcat_chk(char *dest, const char *src, size_t destlen) {
	return strcat(dest, src);
}

char *__strrchr_chk(const char *p, int ch, size_t s_len) {
	return strrchr(p, ch);
}

char *__strcpy_chk(char *dest, const char *src, size_t destlen) {
	return strcpy(dest, src);
}

char *__strncat_chk(char *s1, const char *s2, size_t n, size_t s1len) {
	return strncat(s1, s2, n);
}

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen) {
	return sceClibMemcpy(dest, src, len);
}

int __vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *format, va_list args) {
	return vsnprintf(s, maxlen, format, args);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr = vglMemalign(alignment, size);
	return 0;
}

static so_default_dynlib net_dynlib[] = {
	{ "bind", (uintptr_t)&bind },
	{ "socket", (uintptr_t)&socket },
};

void abort_hook() {
	debugPrintf("abort called by %p %s\n", __builtin_return_address(0));
	sceKernelExitProcess(0);
}

void __assert2(const char *file, int line, const char *func, const char *expr) {
	debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
}

// libc imports newer runners (GMS 2024.x) pull in
// Existing dirs must fail with EEXIST: the runner creates save paths one level at a time
// and gives up on any other error (newlib reported ENOMEM for "ux0:data")
int mkdir_hook(const char *path, mode_t mode) {
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		errno = EEXIST;
		return -1;
	}
	return mkdir(path, mode);
}

char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len) {
	return strncpy(dst, src, n);
}

char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len, size_t src_len) {
	return strncpy(dst, src, n);
}

int __open_2(const char *path, int flags) {
	return open(path, flags);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
	return read(fd, buf, count);
}

int fileno_hook(FILE *f) {
	return fileno(f);
}

int getpagesize_hook(void) {
	return 4096;
}

void _exit_hook(int status) {
	sceKernelExitProcess(status);
}

static so_default_dynlib default_dynlib[] = {
	{ "__strncpy_chk", (uintptr_t)&__strncpy_chk },
	{ "__strncpy_chk2", (uintptr_t)&__strncpy_chk2 },
	{ "__open_2", (uintptr_t)&__open_2 },
	{ "__read_chk", (uintptr_t)&__read_chk },
	{ "_exit", (uintptr_t)&_exit_hook },
	{ "android_set_abort_message", (uintptr_t)&ret0 },
	{ "asprintf", (uintptr_t)&asprintf },
	{ "atof", (uintptr_t)&atof },
	{ "cbrt", (uintptr_t)&cbrt },
	{ "closedir", (uintptr_t)&closedir },
	{ "closelog", (uintptr_t)&ret0 },
	{ "dl_iterate_phdr", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	{ "exp2", (uintptr_t)&exp2 },
	{ "fileno", (uintptr_t)&fileno_hook },
	{ "getpagesize", (uintptr_t)&getpagesize_hook },
	{ "getpgid", (uintptr_t)&ret0 },
	{ "getppid", (uintptr_t)&ret0 },
	{ "getpriority", (uintptr_t)&ret0 },
	{ "getuid", (uintptr_t)&ret0 },
	{ "gmtime", (uintptr_t)&gmtime },
	{ "gmtime_r", (uintptr_t)&gmtime_r },
	{ "localtime", (uintptr_t)&localtime },
	{ "mkstemp", (uintptr_t)&mkstemp },
	{ "opendir", (uintptr_t)&opendir },
	{ "openlog", (uintptr_t)&ret0 },
	{ "perror", (uintptr_t)&perror },
	{ "pthread_atfork", (uintptr_t)&ret0 },
	{ "puts", (uintptr_t)&puts },
	{ "raise", (uintptr_t)&ret0 },
	{ "rand", (uintptr_t)&rand },
	{ "readdir", (uintptr_t)&readdir },
	{ "sigaction", (uintptr_t)&ret0 },
	{ "signal", (uintptr_t)&ret0 },
	{ "sigpending", (uintptr_t)&ret0 },
	{ "sigprocmask", (uintptr_t)&ret0 },
	{ "strspn", (uintptr_t)&strspn },
	{ "syslog", (uintptr_t)&ret0 },
	{ "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE},
	{ "SL_IID_AUDIOIODEVICECAPABILITIES", (uintptr_t)&SL_IID_AUDIOIODEVICECAPABILITIES},
	{ "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE},
	{ "SL_IID_DYNAMICSOURCE", (uintptr_t)&SL_IID_DYNAMICSOURCE},
	{ "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE},
	{ "SL_IID_LED", (uintptr_t)&SL_IID_LED},
	{ "SL_IID_NULL", (uintptr_t)&SL_IID_NULL},
	{ "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION},
	{ "SL_IID_METADATATRAVERSAL", (uintptr_t)&SL_IID_METADATATRAVERSAL},
	{ "SL_IID_OBJECT", (uintptr_t)&SL_IID_OBJECT},
	{ "SL_IID_OUTPUTMIX", (uintptr_t)&SL_IID_OUTPUTMIX},
	{ "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY},
	{ "SL_IID_VIBRA", (uintptr_t)&SL_IID_VIBRA},
	{ "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME},
	{ "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS},
	{ "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE},
	{ "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK},
	{ "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD},
	{ "SL_IID_EQUALIZER", (uintptr_t)&SL_IID_EQUALIZER},
	{ "SL_IID_DEVICEVOLUME", (uintptr_t)&SL_IID_DEVICEVOLUME},
	{ "SL_IID_PRESETREVERB", (uintptr_t)&SL_IID_PRESETREVERB},
	{ "SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB},
	{ "SL_IID_EFFECTSEND", (uintptr_t)&SL_IID_EFFECTSEND},
	{ "SL_IID_3DGROUPING", (uintptr_t)&SL_IID_3DGROUPING},
	{ "SL_IID_3DCOMMIT", (uintptr_t)&SL_IID_3DCOMMIT},
	{ "SL_IID_3DLOCATION", (uintptr_t)&SL_IID_3DLOCATION},
	{ "SL_IID_3DDOPPLER", (uintptr_t)&SL_IID_3DDOPPLER},
	{ "SL_IID_3DSOURCE", (uintptr_t)&SL_IID_3DSOURCE},
	{ "SL_IID_3DMACROSCOPIC", (uintptr_t)&SL_IID_3DMACROSCOPIC},
	{ "SL_IID_MUTESOLO", (uintptr_t)&SL_IID_MUTESOLO},
	{ "SL_IID_DYNAMICINTERFACEMANAGEMENT", (uintptr_t)&SL_IID_DYNAMICINTERFACEMANAGEMENT},
	{ "SL_IID_MIDIMESSAGE", (uintptr_t)&SL_IID_MIDIMESSAGE},
	{ "SL_IID_MIDIMUTESOLO", (uintptr_t)&SL_IID_MIDIMUTESOLO},
	{ "SL_IID_MIDITEMPO", (uintptr_t)&SL_IID_MIDITEMPO},
	{ "SL_IID_MIDITIME", (uintptr_t)&SL_IID_MIDITIME},
	{ "SL_IID_AUDIODECODERCAPABILITIES", (uintptr_t)&SL_IID_AUDIODECODERCAPABILITIES},
	{ "SL_IID_AUDIOENCODERCAPABILITIES", (uintptr_t)&SL_IID_AUDIOENCODERCAPABILITIES},
	{ "SL_IID_AUDIOENCODER", (uintptr_t)&SL_IID_AUDIOENCODER},
	{ "SL_IID_BASSBOOST", (uintptr_t)&SL_IID_BASSBOOST},
	{ "SL_IID_PITCH", (uintptr_t)&SL_IID_PITCH},
	{ "SL_IID_RATEPITCH", (uintptr_t)&SL_IID_RATEPITCH},
	{ "SL_IID_VIRTUALIZER", (uintptr_t)&SL_IID_VIRTUALIZER},
	{ "SL_IID_VISUALIZATION", (uintptr_t)&SL_IID_VISUALIZATION},
	{ "SL_IID_ENGINECAPABILITIES", (uintptr_t)&SL_IID_ENGINECAPABILITIES},
	{ "SL_IID_THREADSYNC", (uintptr_t)&SL_IID_THREADSYNC},
	{ "SL_IID_ANDROIDEFFECT", (uintptr_t)&SL_IID_ANDROIDEFFECT},
	{ "SL_IID_ANDROIDEFFECTSEND", (uintptr_t)&SL_IID_ANDROIDEFFECTSEND},
	{ "SL_IID_ANDROIDEFFECTCAPABILITIES", (uintptr_t)&SL_IID_ANDROIDEFFECTCAPABILITIES},
	{ "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION},
	{ "slCreateEngine", (uintptr_t)&slCreateEngine },
	{ "AAssetManager_open", (uintptr_t)&AAssetManager_open},
	{ "AAsset_close", (uintptr_t)&AAsset_close},
	{ "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava},
	{ "AAsset_read", (uintptr_t)&AAsset_read},
	{ "AAsset_seek", (uintptr_t)&AAsset_seek},
	{ "AAsset_getLength", (uintptr_t)&AAsset_getLength},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "_Znaj", (uintptr_t)&_Znaj },
	{ "_Znwj", (uintptr_t)&_Znwj },
	{ "_ZdaPv", (uintptr_t)&_ZdaPv },
	{ "_ZdlPv", (uintptr_t)&_ZdlPv },
	{ "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE},
	{ "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE},
	{ "_ZNSt12length_errorD1Ev", (uintptr_t)&_ZNSt12length_errorD1Ev},
	{ "_ZNSt13runtime_errorD1Ev", (uintptr_t)&_ZNSt13runtime_errorD1Ev},
	{ "_ZTVSt12length_error", (uintptr_t)&_ZTVSt12length_error},
	{ "_ZNSt6__ndk112__next_primeEj", &_ZNSt6__ndk112__next_primeEj},
	{ "__aeabi_f2d", (uintptr_t)&__aeabi_f2d },
	{ "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
	{ "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
	{ "__aeabi_d2uiz", (uintptr_t)&__aeabi_d2uiz },
	{ "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
	{ "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
	{ "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
	{ "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
	{ "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
	{ "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
	{ "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
	{ "__aeabi_dcmpge", (uintptr_t)&__aeabi_dcmpge },
	{ "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
	{ "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
	{ "__aeabi_dsub", (uintptr_t)&__aeabi_dsub },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memmove8", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&sceClibMemmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__assert2", (uintptr_t)&__assert2 },
	{ "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max },
	{ "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
	{ "__cxa_thread_atexit_impl", (uintptr_t)&ret0 },
	{ "__cxa_throw", (uintptr_t)&__cxa_throw_hook },
	{ "__errno", (uintptr_t)&__errno },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "__memcpy_chk", (uintptr_t)&__memcpy_chk },
	{ "__memmove_chk", (uintptr_t)&__memmove_chk },
	{ "__memset_chk", (uintptr_t)&__memset_chk },
	{ "__progname", (uintptr_t)&__progname },
	{ "__page_size", (uintptr_t)&__page_size },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "__strcat_chk", (uintptr_t)&__strcat_chk },
	{ "__strchr_chk", (uintptr_t)&__strchr_chk },
	{ "__strcpy_chk", (uintptr_t)&__strcpy_chk },
	{ "__strlcat_chk", (uintptr_t)&__strlcat_chk },
	{ "__strlcpy_chk", (uintptr_t)&__strlcpy_chk },
	{ "__strlen_chk", (uintptr_t)&__strlen_chk },
	{ "__strncat_chk", (uintptr_t)&__strncat_chk },
	{ "__strrchr_chk", (uintptr_t)&__strrchr_chk },
	{ "__vsprintf_chk", (uintptr_t)&__vsprintf_chk },
	{ "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "abort", (uintptr_t)&abort_hook },
	//{ "accept", (uintptr_t)&accept },
	{ "acos", (uintptr_t)&acos },
	{ "acosf", (uintptr_t)&acosf },
	{ "alBufferData", (uintptr_t)&alBufferData },
	{ "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
	{ "alDeleteSources", (uintptr_t)&alDeleteSources },
	{ "alDistanceModel", (uintptr_t)&alDistanceModel },
	{ "alGenBuffers", (uintptr_t)&alGenBuffers },
	{ "alGenSources", (uintptr_t)&alGenSources },
	{ "alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext },
	{ "alGetBufferi", (uintptr_t)&alGetBufferi },
	{ "alGetError", (uintptr_t)&alGetError },
	{ "alGetSourcei", (uintptr_t)&alGetSourcei },
	{ "alGetSourcef", (uintptr_t)&alGetSourcef },
	{ "alIsBuffer", (uintptr_t)&alIsBuffer },
	{ "alListener3f", (uintptr_t)&alListener3f },
	{ "alListenerf", (uintptr_t)&alListenerf },
	{ "alListenerfv", (uintptr_t)&alListenerfv },
	{ "alSource3f", (uintptr_t)&alSource3f },
	{ "alSourcePause", (uintptr_t)&alSourcePause },
	{ "alSourcePlay", (uintptr_t)&alSourcePlay },
	{ "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
	{ "alSourceStop", (uintptr_t)&alSourceStop },
	{ "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
	{ "alSourcef", (uintptr_t)&alSourcef },
	{ "alSourcei", (uintptr_t)&alSourcei },
	{ "alcCaptureSamples", (uintptr_t)&alcCaptureSamples },
	{ "alcCaptureStart", (uintptr_t)&alcCaptureStart },
	{ "alcCaptureStop", (uintptr_t)&alcCaptureStop },
	{ "alcCaptureOpenDevice", (uintptr_t)&alcCaptureOpenDevice },
	{ "alcCloseDevice", (uintptr_t)&alcCloseDevice },
	{ "alcCreateContext", (uintptr_t)&alcCreateContext },
	{ "alcGetContextsDevice", (uintptr_t)&alcGetContextsDevice },
	{ "alcGetError", (uintptr_t)&alcGetError },
	{ "alcGetIntegerv", (uintptr_t)&alcGetIntegerv },
	{ "alcGetString", (uintptr_t)&alcGetString },
	{ "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
	{ "alcDestroyContext", (uintptr_t)&alcDestroyContext },
	{ "alcOpenDevice", (uintptr_t)&alcOpenDevice },
	{ "alcProcessContext", (uintptr_t)&alcProcessContext },
	{ "alcPauseCurrentDevice", (uintptr_t)&ret0 },
	{ "alcResumeCurrentDevice", (uintptr_t)&ret0 },
	{ "alcSuspendContext", (uintptr_t)&alcSuspendContext },
	{ "asin", (uintptr_t)&asin },
	{ "asinf", (uintptr_t)&asinf },
	{ "asinh", (uintptr_t)&asinh },
	{ "atan", (uintptr_t)&atan },
	{ "atan2", (uintptr_t)&lc_atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "bind", (uintptr_t)&bind },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&lc_calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock_gettime", (uintptr_t)&clock_gettime },
	{ "close", (uintptr_t)&close },
	{ "compress", (uintptr_t)&compress },	
	//{ "connect", (uintptr_t)&connect },
	{ "cos", (uintptr_t)&lc_cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlclose", (uintptr_t)&ret0 },
	{ "dlopen", (uintptr_t)&dlopen_hook },
	{ "dlsym", (uintptr_t)&dlsym_hook },
	{ "dlerror", (uintptr_t)&ret0 },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "expf", (uintptr_t)&expf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	{ "fdopen", (uintptr_t)&fdopen },
	{ "feof", (uintptr_t)&feof },
	{ "ferror", (uintptr_t)&ferror },
	{ "fflush", (uintptr_t)&fflush },
	{ "fgetpos", (uintptr_t)&fgetpos },
	{ "fgetc", (uintptr_t)&fgetc },
	{ "fgets", (uintptr_t)&fgets },
	{ "floor", (uintptr_t)&lc_floor },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmax", (uintptr_t)&fmax },
	{ "fmaxf", (uintptr_t)&fmaxf },
	{ "fmin", (uintptr_t)&fmin },
	{ "fminf", (uintptr_t)&fminf },
	{ "fmod", (uintptr_t)&lc_fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	{ "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&lc_free },
	{ "freelocale", (uintptr_t)&freelocale },
	//{ "freeaddrinfo", (uintptr_t)&freeaddrinfo },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	{ "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat_hook },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getaddrinfo", (uintptr_t)&getaddrinfo },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getenv", (uintptr_t)&ret0 },
	//{ "getsockopt", (uintptr_t)&getsockopt },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "glAlphaFunc", (uintptr_t)&glAlphaFunc },
	{ "glBindBuffer", (uintptr_t)&glBindBuffer },
	{ "glBindFramebufferOES", (uintptr_t)&glBindFramebufferHook },
	{ "glBindTexture", (uintptr_t)&glBindTexture },
	{ "glBlendFunc", (uintptr_t)&glBlendFunc },
	{ "glBufferData", (uintptr_t)&perf_glBufferData },
	{ "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus },
	{ "glClear", (uintptr_t)&glClearHook },
	{ "glClearColor", (uintptr_t)&glClearColor },
	{ "glClearDepthf", (uintptr_t)&glClearDepthf },
	{ "glColorMask", (uintptr_t)&glColorMask },
	{ "glColorPointer", (uintptr_t)&glColorPointer },
	{ "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
	{ "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers },
	{ "glDeleteTextures", (uintptr_t)&glDeleteTextures },
	{ "glDepthFunc", (uintptr_t)&glDepthFunc },
	{ "glDepthMask", (uintptr_t)&glDepthMask },
	{ "glDepthRangef", (uintptr_t)&glDepthRangef },
	{ "glDisable", (uintptr_t)&glDisable },
	{ "glDisableClientState", (uintptr_t)&glDisableClientState },
	{ "glDrawArrays", (uintptr_t)&perf_glDrawArrays },
	{ "glEnable", (uintptr_t)&glEnable },
	{ "glEnableClientState", (uintptr_t)&glEnableClientState },
	{ "glFlush", (uintptr_t)&glFlush },
	{ "glFogf", (uintptr_t)&glFogf },
	{ "glFogfv", (uintptr_t)&glFogfv },
	{ "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D },
	{ "glFrontFace", (uintptr_t)&glFrontFace },
	{ "glGenBuffers", (uintptr_t)&glGenBuffers },
	{ "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
	{ "glGenTextures", (uintptr_t)&glGenTextures },
	{ "glGetError", (uintptr_t)&glGetError },
	{ "glGetString", (uintptr_t)&glGetString },
	{ "glHint", (uintptr_t)&ret0 },
	{ "glLightModelfv", (uintptr_t)&glLightModelfv },
	{ "glLightf", (uintptr_t)&ret0 },
	{ "glLightfv", (uintptr_t)&glLightfv },
	{ "glLoadIdentity", (uintptr_t)&glLoadIdentity },
	{ "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
	{ "glMaterialfv", (uintptr_t)&glMaterialfv },
	{ "glMatrixMode", (uintptr_t)&glMatrixMode },
	{ "glNormalPointer", (uintptr_t)&ret0 },
	{ "glPixelStorei", (uintptr_t)&ret0 },
	{ "glPopMatrix", (uintptr_t)&glPopMatrix },
	{ "glPushMatrix", (uintptr_t)&glPushMatrix },
	{ "glReadPixels", (uintptr_t)&glReadPixelsHook },
	{ "glScissor", (uintptr_t)&glScissor },
	{ "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
	{ "glTexEnvi", (uintptr_t)&glTexEnvi },
	{ "glTexImage2D", (uintptr_t)&perf_glTexImage2D },
	{ "glTexParameterf", (uintptr_t)&glTexParameterfHook },
	{ "glTexParameteri", (uintptr_t)&glTexParameteriHook },
	{ "glVertexPointer", (uintptr_t)&glVertexPointer },
	{ "glViewport", (uintptr_t)&glViewport },
	{ "gmtime64", (uintptr_t)&gmtime64 },
	{ "inet_addr", (uintptr_t)&inet_addr },
	{ "inet_ntoa", (uintptr_t)&inet_ntoa },
	//{ "inet_ntop", (uintptr_t)&inet_ntop },
	//{ "inet_pton", (uintptr_t)&inet_pton },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "ioctl", (uintptr_t)&ret0 },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "isblank", (uintptr_t)&isblank },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "islower", (uintptr_t)&islower },
	{ "isnan", (uintptr_t)&isnan },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldiv", (uintptr_t)&ldiv },
	//{ "listen", (uintptr_t)&listen },
	{ "llrint", (uintptr_t)&llrint },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "localtime64", (uintptr_t)&localtime64 },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log2", (uintptr_t)&log2 },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lround", (uintptr_t)&lround },
	{ "lroundf", (uintptr_t)&lroundf },
	{ "lseek", (uintptr_t)&lseek },
	{ "malloc", (uintptr_t)&lc_malloc },
	{ "mbtowc", (uintptr_t)&mbtowc },
	{ "mbrlen", (uintptr_t)&mbrlen },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "mbsrtowcs", (uintptr_t)&mbsrtowcs },
	{ "memalign", (uintptr_t)&vglMemalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&sceClibMemmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir_hook },
	{ "mktime", (uintptr_t)&mktime },
	{ "mktime64", (uintptr_t)&mktime64 },
	{ "mmap", (uintptr_t)&mmap },
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	{ "munmap", (uintptr_t)&munmap },
	{ "nanosleep", (uintptr_t)&nanosleep_hook },
	{ "newlocale", (uintptr_t)&newlocale },
	{ "open", (uintptr_t)&open },
	{ "posix_memalign", (uintptr_t)&posix_memalign },
	{ "pow", (uintptr_t)&lc_pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&debugPrintf },
	{ "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
	{ "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
	{ "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
	{ "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_soloader },
	{ "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_soloader },
	{ "pthread_attr_setschedpolicy", (uintptr_t)&ret0 },
	{ "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_soloader },
	{ "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
	{ "pthread_attr_getstacksize", (uintptr_t) &pthread_attr_getstacksize_soloader },
	{ "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
	{ "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
	{ "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
	{ "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
	{ "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
	{ "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
	{ "pthread_condattr_init", (uintptr_t) &pthread_condattr_init_soloader },
	{ "pthread_condattr_destroy", (uintptr_t) &pthread_condattr_destroy_soloader },
	{ "pthread_create", (uintptr_t) &pthread_create_soloader },
	{ "pthread_detach", (uintptr_t) &pthread_detach_soloader },
	{ "pthread_equal", (uintptr_t) &pthread_equal_soloader },
	{ "pthread_exit", (uintptr_t) &pthread_exit },
	{ "pthread_getattr_np", (uintptr_t) &pthread_getattr_np_soloader },
	{ "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_join", (uintptr_t)&pthread_join_soloader },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_rwlock_init", (uintptr_t) &pthread_rwlock_init_soloader },
	{ "pthread_rwlock_destroy", (uintptr_t) &pthread_rwlock_destroy_soloader },
	{ "pthread_rwlock_rdlock", (uintptr_t) &pthread_rwlock_rdlock_soloader },
	{ "pthread_rwlock_tryrdlock", (uintptr_t) &pthread_rwlock_tryrdlock_soloader },
	{ "pthread_rwlock_timedrdlock", (uintptr_t) &pthread_rwlock_timedrdlock_soloader },
	{ "pthread_rwlock_wrlock", (uintptr_t) &pthread_rwlock_wrlock_soloader },
	{ "pthread_rwlock_trywrlock", (uintptr_t) &pthread_rwlock_trywrlock_soloader },
	{ "pthread_rwlock_timedwrlock", (uintptr_t) &pthread_rwlock_timedwrlock_soloader },
	{ "pthread_rwlock_unlock", (uintptr_t) &pthread_rwlock_unlock_soloader },
	{ "pthread_rwlockattr_init", (uintptr_t) &ret0 },
	{ "pthread_rwlockattr_destroy", (uintptr_t) &ret0 },
	{ "pthread_rwlockattr_setpshared", (uintptr_t) &ret0 },
	{ "pthread_rwlockattr_setkind_np", (uintptr_t) &ret0 },
	{ "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
	{ "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
	{ "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
	{ "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader},
	{ "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
	{ "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader},
	{ "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader},
	{ "pthread_mutexattr_setpshared", (uintptr_t) &pthread_mutexattr_setpshared_soloader},
	{ "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader},
	{ "pthread_once", (uintptr_t)&pthread_once },
	{ "pthread_self", (uintptr_t) &pthread_self },
	{ "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "putc", (uintptr_t)&putc },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "read", (uintptr_t)&read },
	{ "realloc", (uintptr_t)&lc_realloc },
	//{ "recv", (uintptr_t)&recv },
	//{ "recvfrom", (uintptr_t)&recvfrom },
	{ "remove", (uintptr_t)&sceIoRemove },
	{ "rename", (uintptr_t)&sceIoRename },
	{ "rint", (uintptr_t)&rint },
	{ "round", (uintptr_t)&round },
	{ "roundf", (uintptr_t)&roundf },
	{ "scandir", (uintptr_t)&scandir_hook },
	//{ "send", (uintptr_t)&send },
	//{ "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	//{ "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&lc_sin },
	{ "sincos", (uintptr_t)&sincos },
	{ "sincosf", (uintptr_t)&sincosf },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	{ "snprintf", (uintptr_t)&snprintf },
	{ "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&lc_sqrt },
	{ "sqrtf", (uintptr_t)&lc_sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&lc_strcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strndup", (uintptr_t)&strndup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strerror_r", (uintptr_t)&strerror_r },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlen", (uintptr_t)&lc_strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtof", (uintptr_t)&strtof },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtoimax", (uintptr_t)&strtoimax },
	{ "strtok", (uintptr_t)&strtok },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtold", (uintptr_t)&strtold },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strtoumax", (uintptr_t)&strtoumax },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "swprintf", (uintptr_t)&swprintf },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "timegm64", (uintptr_t)&timegm64 },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "uselocale", (uintptr_t)&uselocale },
	{ "usleep", (uintptr_t)&usleep },
	{ "vasprintf", (uintptr_t)&vasprintf },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vsscanf", (uintptr_t)&vsscanf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsnrtombs", (uintptr_t)&wcsnrtombs },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wcstod", (uintptr_t)&wcstod },
	{ "wcstof", (uintptr_t)&wcstof },
	{ "wcstol", (uintptr_t)&wcstol },
	{ "wcstold", (uintptr_t)&wcstold },
	{ "wcstoll", (uintptr_t)&wcstoll },
	{ "wcstoul", (uintptr_t)&wcstoul },
	{ "wcstoull", (uintptr_t)&wcstoull },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
};

void *dlsym_hook( void *handle, const char *symbol) {
	void *perf_fn = perf_gl_lookup(symbol);
	if (perf_fn)
		return perf_fn;
	for (size_t i = 0; i < gl_numret; ++i) {
		if (!strcmp(symbol, gl_ret0[i])) {
			return ret0;
		}
	}
	for (size_t i = 0; i < gl_numhook; ++i) {
		if (!strcmp(symbol, gl_hook[i].symbol)) {
			return (void *)gl_hook[i].func;
		}
	}
	
	void *func = vglGetProcAddress(symbol);
	
	if (!func) {
		for (size_t i = 0; i < sizeof(default_dynlib) / sizeof(so_default_dynlib); i++) {
			if (!strcmp(symbol, default_dynlib[i].symbol)) {
				return default_dynlib[i].func;
			}
		}
	}
	
	return func;
}

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	CALL_EXTENSION_FUNCTION,
	DOUBLE_VALUE,
	GAMEPAD_CONNECTED,
	GAMEPAD_DESCRIPTION,
	GET_UDID,
	GET_DEFAULT_FRAMEBUFFER,
	HTTP_POST,
	HTTP_GET,
	INIT,
	INPUT_STRING_ASYNC,
	OS_GET_INFO,
	PAUSE_MP3,
	PLAY_MP3,
	RESUME_MP3,
	SHOW_MESSAGE,
	SHOW_MESSAGE_ASYNC,
	STOP_MP3,
	PLAYING_MP3,
	GET_MIN_BUFFER_SIZE,
	PLAY,
	STOP,
	RELEASE,
	WRITE
} MethodIDs;

typedef struct {
	char *name;
	int id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "CallExtensionFunction", CALL_EXTENSION_FUNCTION },
	{ "doubleValue", DOUBLE_VALUE },
	{ "GamepadConnected", GAMEPAD_CONNECTED },
	{ "GamepadDescription", GAMEPAD_DESCRIPTION },
	{ "GetUDID", GET_UDID },
	{ "GetDefaultFrameBuffer", GET_DEFAULT_FRAMEBUFFER },
	{ "HttpGet", HTTP_GET },
	{ "HttpPost", HTTP_POST },
	{ "<init>", INIT },
	{ "InputStringAsync", INPUT_STRING_ASYNC },
	{ "OsGetInfo", OS_GET_INFO },
	{ "PauseMP3", PAUSE_MP3 },
	{ "PlayMP3", PLAY_MP3 },
	{ "PlayingMP3", PLAYING_MP3 },
	{ "ResumeMP3", RESUME_MP3 },
	{ "ShowMessage", SHOW_MESSAGE },
	{ "ShowMessageAsync", SHOW_MESSAGE_ASYNC },
	{ "StopMP3", STOP_MP3 },
	{ "getMinBufferSize", GET_MIN_BUFFER_SIZE },
	{ "play", PLAY },
	{ "stop", STOP },
	{ "release", RELEASE },
	{ "write", WRITE },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	debugPrintf("Attempted to get an unknown method ID with name %s\n", name);
	return 0;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	debugPrintf("Attempted to get an unknown static method ID with name %s\n", name);
	return 0;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case SHOW_MESSAGE:
		debugPrintf(args[0]);
		break;
	case INPUT_STRING_ASYNC:
		init_ime_dialog(args[0], args[1]);
		ime_index = (int)args[2];
		ime_active = 1;
		break;
	case SHOW_MESSAGE_ASYNC:
		init_msg_dialog(args[0]);
		msg_index = (int)args[1];
		msg_active = 1;
		break;
	case HTTP_POST:
		if (has_net) {
			send_post_request(args[0], args[1]);
			post_index = (int)args[2];
			post_active = 1;
		}
		break;
	case HTTP_GET:
		if (has_net) {
			send_get_request(args[0]);
			get_index = (int)args[1];
			get_active = 1;
		}
		break;
	case PLAY_MP3:
		audio_player_play(args[0], args[1]);
		break;
	case STOP_MP3:
		audio_player_stop();
		break;
	case PAUSE_MP3:
		audio_player_pause();
		break;
	case RESUME_MP3:
		audio_player_resume();
		break;
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallStaticVoidMethodV(%d)\n", methodID);
		break;
	}
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case GAMEPAD_CONNECTED:
		return is_gamepad_connected(args[0]);
	case PLAYING_MP3:
		return audio_player_is_playing();
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallStaticBooleanMethodV(%d)\n", methodID);
		return 0;
	}
}

int CallStaticByteMethod(void *env, void *obj, int methodID, uintptr_t *args) {
	if (methodID != UNKNOWN)
		debugPrintf("CallStaticByteMethodV(%d)\n", methodID);
	return 0;
}


uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	if (methodID != UNKNOWN)
		debugPrintf("CallLongMethodV(%d)\n", methodID);
	return 0;
}

enum ClassIDs {
	STRING,
	AUDIO_TRACK
};

int FindClass(void *env, const char *name) {
	if (strcmp(name, "java/lang/Object")) // looked up on every extension call
		debugPrintf("FindClass %s\n", name);
	if (!strcmp(name, "java/lang/String")) {
		return STRING;
	} else if (!strcmp(name, "android/media/AudioTrack")) {
		return AUDIO_TRACK;
	}
	return 0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void *NewWeakGlobalRef(void *env, char *str) {
	return (void *)0x45454545;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewString(void *env, char *bytes) {
	return bytes;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	if (isCopy)
		*isCopy = 0;
	return string;
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

enum {
	UNKNOWN2,
	MANUFACTURER	
};

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	if (!strcmp("MANUFACTURER",name))
		return MANUFACTURER;
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	static char *r = NULL;
	switch (fieldID) {
	case MANUFACTURER:
		if (!r)
			r = malloc(0x100);
		strcpy(r, "Rinnegatamante");
		return r;
	default:
		return NULL;
	}
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 0;
}

void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	if (methodID != UNKNOWN)
		debugPrintf("CallObjectMethodV(%d)\n", methodID);
	return NULL;
}

typedef struct {
	char *module_name;
	char *method_name;
	int argc;
	double *double_array;
	void *object_array;
} ext_func;

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	static char r[512];
	switch (methodID) {
	case GAMEPAD_DESCRIPTION:
		return "Generic Gamepad";
	case GET_UDID:
		return "1234";
	case CALL_EXTENSION_FUNCTION:
		{
			ext_func *f = (ext_func *)args;
			if (!strcmp(f->module_name, "PickMe")) { // Used by Super Mario Maker: World Engine
				if (!strcmp(f->method_name, "getDire1")) {
					sprintf(r, "%s%s/", data_path, (char *)f->object_array);
					recursive_mkdir(r);
					return f->object_array;
				}
			} else if (!strcmp(f->module_name, "NOTCH")) { // Used by Forager
				jni_double = 0.0;
				return &jni_double;
			} else if (!strcmp(f->module_name, "OUYAExt")) { // Used by Angry Ranook
				if (!strcmp(f->method_name, "ouyaIsOUYA")) {
					jni_double = 1.0;
					return &jni_double;
				}
			} else if (!strcmp(f->module_name, "myclass")) { // Used by IMSCARED
				if (!strcmp(f->method_name, "vibrate_start")) {
					return NULL;
				} else if (!strcmp(f->method_name, "getScreenBrightness")) {
					int val;
					if (sceRegMgrGetKeyInt("/CONFIG/DISPLAY/", "brightness", &val) < 0)
						jni_double = -1.0;
					else
						jni_double = ((double)val / 65536.0) * 128.0;
					return &jni_double;
				}
			}
			// PC ports poll Steam every frame; don't flood the log with it
			if (!strncmp(f->method_name, "steam_", 6))
				return NULL;
			debugPrintf("Called undefined extension function from module %s with name %s\n", f->module_name, f->method_name);
			return NULL;
		}
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallStaticObjectMethodV(%d)\n", methodID);
		return NULL;
	}
}

int audio_samplerate;
int audio_channels;
SceUID audio_port;

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case GET_DEFAULT_FRAMEBUFFER:
		return 0;
	case GET_MIN_BUFFER_SIZE:
		audio_samplerate = args[0];
		audio_channels = args[1] == 0x03 ? 2 : 1;
		audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, 4096 / (2 * audio_channels), audio_samplerate, (SceAudioOutMode)(audio_channels - 1));
		debugPrintf("Asking for %s audio at %dhz samplerate. (Opened port %d)\n", audio_channels == 2 ? "stereo" : "mono", audio_samplerate, audio_port);
		return 4096;
	case OS_GET_INFO:
		return Java_com_yoyogames_runner_RunnerJNILib_CreateVersionDSMap(fake_env, 0, 7, "v1.0", "PSVita", "PSVita", "Sony Computer Entertainment", "armeabi", "armeabi-v7a", "YoYo Loader", "ARM Cortex A9", "v1.0", "Global", "v1.0", 0);
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallStaticIntMethodV(%d)\n", methodID);
		return 0;
	}
}

double CallStaticDoubleMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallStaticDoubleMethodV(%d)\n", methodID);
		return 0;
	}
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallBooleanMethodV(%d)\n", methodID);
		return 0;
	}
}

double CallDoubleMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	double r;
	switch (methodID) {
	case DOUBLE_VALUE:
		r = jni_double;
		jni_double = 0.0f;
		return r;
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallDoubleMethodV(%d)\n", methodID);
		break;
	}
	return 0.0;
}

int CallNonVirtualIntMethod(void *env, void *obj, int classID, int methodID, uintptr_t *args) {
	switch (methodID) {
	case WRITE:
		{
			sceAudioOutOutput(audio_port, (void *)args[0]);
			return (int)args[2];
		}
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallNonVirtualIntMethod(0x%x, %d)\n", classID, methodID);
		break;
	}
	return 0;
}

void CallNonVirtualVoidMethod(void *env, void *obj, int classID, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallNonVirtualVoidMethod(0x%x, %d)\n", classID, methodID);
		break;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN)
			debugPrintf("CallVoidMethodV(%d)\n", methodID);
		break;
	}
}

void *NewIntArray(void *env, int size) {
	return vglMalloc(sizeof(int) * size);	
}

void *NewCharArray(void *env, int size) {
	return vglMalloc(sizeof(char) * size);
}

void *NewObjectArray(void *env, int size, int clazz, void *elements) {
	if (disableObjectsArray)
		return NULL;
	void *r = vglMalloc(size);
	if (elements) {
		sceClibMemcpy(r, elements, size);
	}
	return r;
}

void *NewDoubleArray(void *env, int size) {
	return vglMalloc(sizeof(double) * size);	
}

int GetArrayLength(void *env, void *array) {
	return (int)downloaded_bytes;
}

void SetIntArrayRegion(void *env, int *array, int start, int len, int *buf) {
	sceClibMemcpy(&array[start], buf, sizeof(int) * len);
}

void SetDoubleArrayRegion(void *env, double *array, int start, int len, double *buf) {
	sceClibMemcpy(&array[start], buf, sizeof(double) * len);
}

void SetObjectArrayElement(void *env, uint8_t *array, int index, void *val) {
	if (array)
		strcpy(&array[index], val);
}

void GetByteArrayRegion(void *env, uint8_t *array, int start, int len, void *buf) {
	sceClibMemcpy(buf, &array[start], len);
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

int PushLocalFrame(void *env, int capacity) {
    return 0;
}

void *PopLocalFrame(void *env, void *obj) {
    return NULL;
}

void *GetPrimitiveArrayCritical(void *env, void *arr, int *isCopy) {
	if (isCopy)
		*isCopy = 0;

    return arr;
}

void ReleasePrimitiveArrayCritical(void *env, void *arr, void *carray, int mode) {
}

static void game_end()
{
	if (voidstranger_hack) {
		void (*Run_EndGame) () = (void *)so_symbol(&yoyoloader_mod, "_Z11Run_EndGamev");
		Run_EndGame();
	}
#ifdef STANDALONE_MODE
	sceKernelExitProcess(0);
#else
	sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
#endif
}

static int last_track_id = -1;
static double last_track_pos = 0.f;
static uint32_t last_track_pos_frame = 0;
static void audio_sound_get_track_position(retval_t *ret, void *self, void *other, int argc, retval_t *args) {
	if (*g_fNoAudio)
		return;
	
	ret->kind = VALUE_REAL;
	int sound_id = YYGetInt32(args, 0);
	
	if ((last_track_id != sound_id) || (*g_IOFrameCount - last_track_pos_frame > 1)) {
		ret->rvalue.val = Audio_GetTrackPos(sound_id);
	} else {
		if (last_track_pos_frame == *g_IOFrameCount)
			ret->rvalue.val = last_track_pos;
		else {
			ret->rvalue.val = Audio_GetTrackPos(sound_id);
			if (ret->rvalue.val < last_track_pos && fabs(ret->rvalue.val - last_track_pos) < 0.1f)
				ret->rvalue.val = last_track_pos + (double)*g_GML_DeltaTime / 1000000.0f;
		}
	}
	
	last_track_pos_frame = *g_IOFrameCount;
	last_track_id = sound_id;
	last_track_pos = ret->rvalue.val;
}

void *pthread_main(void *arg);

int main(int argc, char *argv[]) {
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
	pthread_create(&t, &attr, pthread_main, NULL);

	return sceKernelExitDeleteThread(0);
}

void *pthread_main(void *arg) {
#if 0
	// Debug
	sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
#endif
#ifdef HAS_VIDEO_PLAYBACK_SUPPORT
	sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
#endif
	
	// Checking requested game launch
	char game_name[0x200];
#ifndef STANDALONE_MODE
	FILE *f = fopen(LAUNCH_FILE_PATH, "r");
	if (f) {
		size_t size = fread(game_name, 1, 0x200, f);
		fclose(f);
		sceIoRemove(LAUNCH_FILE_PATH);
		game_name[size] = 0;
	} else {
		strcpy(game_name, "test"); // Debug
	}
#else
	sceAppMgrAppParamGetString(0, 12, game_name, 256);
#endif

	// Enabling analogs and touch sampling
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
	sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
	sceMotionReset();
	sceMotionStartSampling();

	// Maximizing clocks
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	
	// Populating required strings and creating required folders
	char pkg_name[256];
#ifdef STANDALONE_MODE
	sprintf(apk_path, "app0:game.apk");
#else
	sprintf(apk_path, "%s/%s/game.apk", DATA_PATH, game_name);
#endif
	sprintf(data_path_root, "%s/%s/", DATA_PATH, game_name);
	read_tune(data_path_root);
	sprintf(data_path, "%s/%s/assets/", DATA_PATH, game_name);
	recursive_mkdir(data_path);
	
	sprintf(gxp_path, "ux0:data/gms/shared/gxp/%s", game_name);
	sceIoMkdir("ux0:data/gms/shared", 0777);
	sceIoMkdir("ux0:data/gms/shared/gxp", 0777);
	sceIoMkdir("ux0:data/gms/shared/glsl", 0777);
	sceIoMkdir(gxp_path, 0777);
	strcpy(pkg_name, "com.rinnegatamante.loader");

	// Checking for dependencies
	if (check_kubridge() < 0)
		fatal_error("Error: kubridge.skprx is not installed.");
	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error: libshacccg.suprx is not installed.");
	
	// Loading shared C++ executable, if present, from the apk
	GLboolean has_cpp_so = GL_FALSE;
	GLboolean cpp_warn = GL_FALSE;
	unz_file_info file_info;
	unzFile apk_file = unzOpen(apk_path);
	if (!apk_file)
		fatal_error("Error could not find %s.", apk_path);
	int res = unzLocateFile(apk_file, "lib/armeabi-v7a/libc++_shared.so", NULL);
	if (res == UNZ_OK) {
		unzGetCurrentFileInfo(apk_file, &file_info, NULL, 0, NULL, 0, NULL, 0);
		unzOpenCurrentFile(apk_file);
		uint64_t so_size = file_info.uncompressed_size;
		uint8_t *so_buffer = (uint8_t *)malloc(so_size);
		unzReadCurrentFile(apk_file, so_buffer, so_size);
		unzCloseCurrentFile(apk_file);
		res = so_mem_load(&cpp_mod, so_buffer, so_size, LOAD_ADDRESS);
		if (res >= 0) {
			has_cpp_so = GL_TRUE;
		} else {
			cpp_warn = GL_TRUE;
		}
		free(so_buffer);
	}
	
	// Loading ARMv7 executable from the apk
	unzLocateFile(apk_file, "lib/armeabi-v7a/libyoyo.so", NULL);
	unzGetCurrentFileInfo(apk_file, &file_info, NULL, 0, NULL, 0, NULL, 0);
	unzOpenCurrentFile(apk_file);
	uint64_t so_size = file_info.uncompressed_size;
	uint8_t *so_buffer = (uint8_t *)malloc(so_size);
	unzReadCurrentFile(apk_file, so_buffer, so_size);
	unzCloseCurrentFile(apk_file);
	res = so_mem_load(&yoyoloader_mod, so_buffer, so_size, LOAD_ADDRESS + 0x1000000);
	if (res < 0)
		fatal_error("Error could not load lib/armeabi-v7a/libyoyo.so from inside game.apk. (Errorcode: 0x%08X)", res);
	free(so_buffer);
	
	// Loading config file
	char *platforms[] = {
		"Mob",
		"Win",
		"PS4"
	};
	loadConfig(game_name);
	debugPrintf("+--------------------------------------------+\n");
	debugPrintf("|YoYo Loader Setup                           |\n");
	debugPrintf("+--------------------------------------------+\n");
	debugPrintf("|Force GLES1 Mode: %s                         |\n", forceGL1 ? "Y" : "N");
	debugPrintf("|Skip Splashscreen at Boot: %s                |\n", forceSplashSkip ? "Y" : "N");
	debugPrintf("|Platform Target: %s                        |\n", platforms[platTarget]);
	debugPrintf("|Use Uncached Mem: %s                         |\n", uncached_mem ? "Y" : "N");
	debugPrintf("|Run with Extended Mem Mode: %s               |\n", maximizeMem ? "Y" : "N");
	debugPrintf("|Run with Extended Runner Pool: %s            |\n", _newlib_heap_size > 256 * 1024 * 1024 ? "Y" : "N");
	debugPrintf("|Run with Mem Squeezing: %s                   |\n", squeeze_mem ? "Y" : "N");
	debugPrintf("|Use Double Buffering: %s                     |\n", double_buffering ? "Y" : "N");
#ifdef HAS_VIDEO_PLAYBACK_SUPPORT
	debugPrintf("|Enable Video Player: Y                      |\n");
#else
	debugPrintf("|Enable Video Player: N                      |\n");
#endif
	debugPrintf("|Enable Network Features: %s                  |\n", has_net ? "Y" : "N");
	debugPrintf("|Force Bilinear Filtering: %s                 |\n", forceBilinear ? "Y" : "N");
	debugPrintf("|Has custom C++ shared lib: %s                |\n", has_cpp_so ? "Y" : "N");
	debugPrintf("+--------------------------------------------+\n\n\n");
	
	if (cpp_warn) {
		debugPrintf("WARNING: Found libc++_shared.so but failed to load.\n");
	}
	
	if (has_net) {
		// Init Net
		debugPrintf("Initializing sceNet...\n");
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
		int ret = sceNetShowNetstat();
		SceNetInitParam initparam;
		if (ret == SCE_NET_ERROR_ENOTINIT) {
			initparam.memory = malloc(141 * 1024);
			initparam.size = 141 * 1024;
			initparam.flags = 0;
			sceNetInit(&initparam);
		}
	} else {
		/* 
		 * FIXME: Object arrays are handled badly and cause crashes in several games.
		 * The only games actually requiring them are the ones using HTTP methods, so we enable them
		 * only if network functionalities are requested.
		 */
		disableObjectsArray = 1;
	}
	
	// Loading splash screen from the apk
	uint8_t *splash_buf = NULL;
	uint32_t splash_size;
	if (!forceSplashSkip && unzLocateFile(apk_file, "assets/splash.png", NULL) == UNZ_OK) {
		unzGetCurrentFileInfo(apk_file, &file_info, NULL, 0, NULL, 0, NULL, 0);
		unzOpenCurrentFile(apk_file);
		splash_size = file_info.uncompressed_size;
		splash_buf = (uint8_t *)malloc(splash_size);
		unzReadCurrentFile(apk_file, splash_buf, splash_size);
		unzCloseCurrentFile(apk_file);
	}
	
	// Loading cpp library if present
	if (has_cpp_so) {
		so_relocate(&cpp_mod);
		so_resolve(&cpp_mod, default_dynlib, sizeof(default_dynlib), 0);
		so_flush_caches(&cpp_mod);
		so_initialize(&cpp_mod);
	}

	// Patching the executable
	so_relocate(&yoyoloader_mod);
	so_resolve(&yoyoloader_mod, default_dynlib, sizeof(default_dynlib), 0);
	if (!has_net)
		so_resolve_with_dummy(&yoyoloader_mod, net_dynlib, sizeof(net_dynlib), 0);
	patch_openal();
	patch_runner();
#ifdef HAS_VIDEO_PLAYBACK_SUPPORT
	patch_video_player();
#endif
	so_flush_caches(&yoyoloader_mod);
	so_initialize(&yoyoloader_mod);
	
	// Initializing vitaGL
	// Default stays tiny (3 KB). It was raised to 4 MB once, together with the broken page decoder
	// later fixed in gm_textures.py, so a bigger pool is now worth re-testing: set it in tune.txt.
	vglSetCircularPoolSize(tune_pool_kb * 1024);
	vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
	if (debugMode)
		vglSetDisplayCallback(mem_profiler);
	vglSetupGarbageCollector(127, 0x20000);
	if (squeeze_mem)
		vglSetParamBufferSize(2 * 1024 * 1024);
	if (!uncached_mem)
		vglUseCachedMem(GL_TRUE);
	if (double_buffering)
		vglUseTripleBuffering(GL_FALSE);
	if (maximizeMem)
		vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, 0, 0, 0, SCE_GXM_MULTISAMPLE_NONE);
	else
		vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	vgl_booted = 1;
	if (!tune_vsync)
		vglWaitVblankStart(GL_FALSE);
	
	// Applying extra patches to the runner
	patch_runner_post_init();
	Function_Add = (void *)so_symbol(&yoyoloader_mod, "_Z12Function_AddPKcPFvR6RValueP9CInstanceS4_iPS1_Eib");
	if (Function_Add == NULL)
		Function_Add = (void *)so_symbol(&yoyoloader_mod, "_Z12Function_AddPcPFvR6RValueP9CInstanceS3_iPS0_Eib");
	
	Function_Add("game_end", (intptr_t)game_end, 1, 1);
	Function_Add("audio_sound_get_track_position", (intptr_t)audio_sound_get_track_position, 1, 1);
	
	//uint8_t *g_fSuppressErrors = (uint8_t *)so_symbol(&yoyoloader_mod, "g_fSuppressErrors");
	//*g_fSuppressErrors = 1;
	
	patch_gamepad(game_name);
#ifdef STANDALONE_MODE
	int has_trophies = trophies_init();
	if (has_trophies > 0) {
		patch_trophies();
	}
#endif
	so_flush_caches(&yoyoloader_mod);

	// Initializing Java VM and JNI Interface
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)GetEnv;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;
	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)PushLocalFrame;
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)PopLocalFrame;
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x68) = (uintptr_t)ret0; // EnsureLocalCapacity
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x80) = (uintptr_t)ret1; // IsInstanceOf
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xEC) = (uintptr_t)CallDoubleMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x140) = (uintptr_t)CallNonVirtualIntMethod;
	*(uintptr_t *)(fake_env + 0x170) = (uintptr_t)CallNonVirtualVoidMethod;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x1E0) = (uintptr_t)CallStaticByteMethod;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x22C) = (uintptr_t)CallStaticDoubleMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x28C) = (uintptr_t)NewString;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B0) = (uintptr_t)NewObjectArray;
	*(uintptr_t *)(fake_env + 0x2B8) = (uintptr_t)SetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x2C0) = (uintptr_t)NewCharArray;
	*(uintptr_t *)(fake_env + 0x2CC) = (uintptr_t)NewIntArray;
	*(uintptr_t *)(fake_env + 0x2D8) = (uintptr_t)NewDoubleArray;
	*(uintptr_t *)(fake_env + 0x320) = (uintptr_t)GetByteArrayRegion;
	*(uintptr_t *)(fake_env + 0x34C) = (uintptr_t)SetIntArrayRegion;
	*(uintptr_t *)(fake_env + 0x358) = (uintptr_t)SetDoubleArrayRegion;
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x378) = (uintptr_t)GetPrimitiveArrayCritical;
	*(uintptr_t *)(fake_env + 0x37C) = (uintptr_t)ReleasePrimitiveArrayCritical;
	*(uintptr_t *)(fake_env + 0x394) = (uintptr_t)NewWeakGlobalRef;
	
	void (*Java_com_yoyogames_runner_RunnerJNILib_Startup) (void *env, int a2, char *apk_path, char *save_dir, char *pkg_dir, int sleep_margin) = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_Startup");
	Java_com_yoyogames_runner_RunnerJNILib_CreateVersionDSMap = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_CreateVersionDSMap");
	Java_com_yoyogames_runner_RunnerJNILib_TouchEvent  = (void *)so_symbol(&yoyoloader_mod, "Java_com_yoyogames_runner_RunnerJNILib_TouchEvent");
	
	// Displaying splash screen
	if (splash_buf) {
		int w, h;
		uint8_t *bg_data = stbi_load_from_memory(splash_buf, splash_size, &w, &h, NULL, 4);
		GLuint bg_image;
		glGenTextures(1, &bg_image);
		glBindTexture(GL_TEXTURE_2D, bg_image);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bg_data);
		vglFree(bg_data);
		free(splash_buf);
		glEnable(GL_TEXTURE_2D);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glViewport(0, 0, SCREEN_W, SCREEN_H);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		float splash_vertices[] = {
			       0,        0, 0,
			SCREEN_W,        0, 0,
			SCREEN_W, SCREEN_H, 0,
			       0, SCREEN_H, 0
		};
		float splash_texcoords[] = {0, 0, 1, 0, 1, 1, 0, 1};
		glVertexPointer(3, GL_FLOAT, 0, splash_vertices);
		glTexCoordPointer(2, GL_FLOAT, 0, splash_texcoords);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		vglSwapBuffers(GL_FALSE);
		glDeleteTextures(1, &bg_image);
	}
	
	// Extracting Game ID
	char game_id[256];
	void *tmp_buf = malloc(32 * 1024 * 1024);
	unzLocateFile(apk_file, "assets/game.droid", NULL);
	unzOpenCurrentFile(apk_file);
	unzReadCurrentFile(apk_file, tmp_buf, 20);
	uint32_t offs;
	unzReadCurrentFile(apk_file, &offs, 4);
	uint32_t target = offs - 28;
	while (target > 32 * 1024 * 1024) {
		unzReadCurrentFile(apk_file, tmp_buf, 32 * 1024 * 1024);
		target -= 32 * 1024 * 1024;
	}
	unzReadCurrentFile(apk_file, tmp_buf, target);
	unzReadCurrentFile(apk_file, &offs, 4);
	unzReadCurrentFile(apk_file, game_id, offs + 1);
	unzClose(apk_file);
	free(tmp_buf);
	debugPrintf("Detected %s as Game ID\n", game_id);
	
	// Enabling game specific gamehacks
	if (!strcmp(game_id, "DELTARUNE")) {
		debugPrintf("Enabling Deltarune specific gamehack!\n");
		deltarune_hack = 1;
	} else if (!strcmp(game_id, "void_stranger")) {
		debugPrintf("Enabling Void Stranger specific gamehack!\n");
		voidstranger_hack = 1;
	}
	
	// Starting the Runner
	Java_com_yoyogames_runner_RunnerJNILib_Startup(fake_env, 0, apk_path, data_path, pkg_name, 0);
	
	// Entering main loop
	debugPrintf("Startup ended\n");
	main_loop();
	
	return 0;
}
