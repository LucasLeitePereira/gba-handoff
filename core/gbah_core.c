#include "gbah_core.h"

#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba-util/vfs.h>

#include <stdarg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- SHA-1 */

typedef struct {
	uint32_t h[5];
	uint64_t len;
	uint8_t buf[64];
	size_t n;
} sha1_ctx;

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
	uint32_t w[80], a, b, d, e, f, k, t, cc;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
		       (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5A827999; }
		else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
		else             { f = b ^ cc ^ d;                   k = 0xCA62C1D6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx *c)
{
	c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
	c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
	c->len = 0;
	c->n = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t n)
{
	c->len += n;
	while (n) {
		size_t k = 64 - c->n;
		if (k > n) k = n;
		memcpy(c->buf + c->n, p, k);
		c->n += k; p += k; n -= k;
		if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
	}
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
	uint64_t bits = c->len * 8;
	uint8_t pad = 0x80, z = 0, l[8];
	int i;
	sha1_update(c, &pad, 1);
	while (c->n != 56) sha1_update(c, &z, 1);
	for (i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha1_update(c, l, 8);
	for (i = 0; i < 20; i++) out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void gbah_sha1(const void *data, size_t size, uint8_t out[20])
{
	sha1_ctx s;
	sha1_init(&s);
	sha1_update(&s, (const uint8_t *)data, size);
	sha1_final(&s, out);
}

/* ----------------------------------------------------------------- core */

struct gbah_core {
	struct mCore *core;
	uint32_t *video;       /* GBAH_WIDTH * GBAH_HEIGHT, XBGR8888 */
	unsigned audio_rate;
	int rom_loaded;
	uint8_t sha1[20];
};

#define AUDIO_BUF_SAMPLES 2048

/* mGBA's default logger printf()s everything to stdout (game-error spam such as
 * "Invalid DMA source address" appears on every state load). Keep errors only. */
static void quiet_log(struct mLogger *l, int category, enum mLogLevel level, const char *fmt, va_list args)
{
	(void)l; (void)category;
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		vfprintf(stderr, fmt, args);
		fputc('\n', stderr);
	}
}
static struct mLogger g_logger = { quiet_log, NULL };

gbah_core *gbah_create(unsigned audio_rate)
{
	gbah_core *c = calloc(1, sizeof(*c));
	if (!c) return NULL;
	mLogSetDefaultLogger(&g_logger); /* process-wide */
	c->audio_rate = audio_rate ? audio_rate : GBAH_AUDIO_RATE_DEFAULT;

	c->core = mCoreCreate(mPLATFORM_GBA);
	if (!c->core || !c->core->init(c->core)) {
		free(c);
		return NULL;
	}
	c->video = calloc((size_t)GBAH_WIDTH * GBAH_HEIGHT, sizeof(uint32_t));
	if (!c->video) {
		c->core->deinit(c->core);
		free(c);
		return NULL;
	}
	/* Config lives in memory only: never read/write files in the user's profile. */
	mCoreInitConfig(c->core, "gbahandoff");
	mCoreConfigSetDefaultIntValue(&c->core->config, "skipBios", 1);
	mCoreConfigSetDefaultIntValue(&c->core->config, "volume", 0x100);
	mCoreConfigSetDefaultIntValue(&c->core->config, "mute", 0);
	mCoreConfigSetDefaultIntValue(&c->core->config, "sampleRate", (int)c->audio_rate);
	mCoreConfigSetDefaultIntValue(&c->core->config, "audioBuffers", AUDIO_BUF_SAMPLES);
	mCoreLoadForeignConfig(c->core, &c->core->config);

	c->core->setVideoBuffer(c->core, c->video, GBAH_WIDTH);
	c->core->setAudioBufferSize(c->core, AUDIO_BUF_SAMPLES);
	return c;
}

void gbah_destroy(gbah_core *c)
{
	if (!c) return;
	if (c->rom_loaded) c->core->unloadROM(c->core);
	mCoreConfigDeinit(&c->core->config);
	c->core->deinit(c->core);
	free(c->video);
	free(c);
}

static void setup_audio(gbah_core *c)
{
	/* Must run after reset: blip rates are re-created with the core. */
	double clock = (double)c->core->frequency(c->core);
	blip_set_rates(c->core->getAudioChannel(c->core, 0), clock, c->audio_rate);
	blip_set_rates(c->core->getAudioChannel(c->core, 1), clock, c->audio_rate);
}

void gbah_reset(gbah_core *c)
{
	c->core->reset(c->core);
	setup_audio(c);
}

int gbah_load_rom_buffer(gbah_core *c, const void *data, size_t size)
{
	struct VFile *vf;
	sha1_ctx s;
	if (!c || !data || !size) return -1;

	/* VFileMemChunk copies the data, so the caller keeps ownership of theirs. */
	vf = VFileMemChunk(data, size);
	if (!vf) return -1;
	if (!c->core->isROM(vf)) {
		vf->close(vf);
		return -2;
	}
	if (c->rom_loaded) {
		c->core->unloadROM(c->core);
		c->rom_loaded = 0;
	}
	/* loadROM takes ownership of vf on success. */
	if (!c->core->loadROM(c->core, vf)) {
		vf->close(vf);
		return -3;
	}
	c->rom_loaded = 1;
	sha1_init(&s);
	sha1_update(&s, data, size);
	sha1_final(&s, c->sha1);
	gbah_reset(c);
	return 0;
}

int gbah_load_rom_file(gbah_core *c, const char *path)
{
	void *buf;
	size_t n;
	int r;
	if (gbah_read_file(path, &buf, &n)) return -1;
	r = gbah_load_rom_buffer(c, buf, n);
	free(buf);
	return r;
}

int gbah_rom_sha1(const gbah_core *c, uint8_t out[20])
{
	if (!c->rom_loaded) return -1;
	memcpy(out, c->sha1, 20);
	return 0;
}

int gbah_rom_sha1_hex(const gbah_core *c, char out[41])
{
	static const char hx[] = "0123456789abcdef";
	int i;
	if (!c->rom_loaded) return -1;
	for (i = 0; i < 20; i++) {
		out[i * 2] = hx[c->sha1[i] >> 4];
		out[i * 2 + 1] = hx[c->sha1[i] & 15];
	}
	out[40] = 0;
	return 0;
}

void gbah_set_input(gbah_core *c, uint32_t keys)
{
	c->core->setKeys(c->core, keys & GBAH_KEY_MASK);
}

void gbah_run_frame(gbah_core *c)
{
	if (c->rom_loaded) c->core->runFrame(c->core);
}

uint32_t gbah_frame_counter(const gbah_core *c)
{
	return c->core->frameCounter(c->core);
}

const uint32_t *gbah_framebuffer(const gbah_core *c)
{
	return c->video;
}

size_t gbah_audio_read(gbah_core *c, int16_t *out, size_t max_frames)
{
	blip_t *l = c->core->getAudioChannel(c->core, 0);
	blip_t *r = c->core->getAudioChannel(c->core, 1);
	size_t avail = (size_t)blip_samples_avail(l);
	if (avail > max_frames) avail = max_frames;
	if (!avail) return 0;
	blip_read_samples(l, out, (int)avail, 1);
	blip_read_samples(r, out + 1, (int)avail, 1);
	return avail;
}

unsigned gbah_audio_rate(const gbah_core *c)
{
	return c->audio_rate;
}

size_t gbah_state_save(gbah_core *c, void **out)
{
	size_t n;
	void *buf;
	*out = NULL;
	if (!c->rom_loaded) return 0;
	n = c->core->stateSize(c->core);
	buf = calloc(1, n);
	if (!buf) return 0;
	if (!c->core->saveState(c->core, buf)) {
		free(buf);
		return 0;
	}
	*out = buf;
	return n;
}

int gbah_state_load(gbah_core *c, const void *data, size_t size)
{
	if (!c->rom_loaded || size != c->core->stateSize(c->core)) return -1;
	/* loadState needs a mutable-looking pointer in some versions; it only reads. */
	if (!c->core->loadState(c->core, data)) return -2;
	/* Audio samples buffered before the load belong to a different timeline. */
	blip_clear(c->core->getAudioChannel(c->core, 0));
	blip_clear(c->core->getAudioChannel(c->core, 1));
	return 0;
}

size_t gbah_sram_export(gbah_core *c, void **out)
{
	*out = NULL;
	if (!c->rom_loaded) return 0;
	return c->core->savedataClone(c->core, out);
}

int gbah_sram_import(gbah_core *c, const void *data, size_t size)
{
	void *cur = NULL;
	size_t cur_n;
	if (!c->rom_loaded || !data || !size) return -1;
	/* mGBA's savedataRestore(writeback=true) = GBASavedataLoad(), which does
	 * nothing and returns true while the save type is still AUTODETECT.
	 * Detect that case (clone size 0) instead of reporting a fake success. */
	cur_n = c->core->savedataClone(c->core, &cur);
	free(cur);
	if (!cur_n) return GBAH_ERR_SAVE_TYPE_UNKNOWN;
	/* writeback=true copies into the live save memory (false would only mask it). */
	return c->core->savedataRestore(c->core, data, size, true) ? 0 : -2;
}

int gbah_load_sav_buffer(gbah_core *c, const void *data, size_t size)
{
	struct VFile *vf;
	if (!c->rom_loaded || !data || !size) return -1;
	/* VFileMemChunk copies; the core owns (and closes) the VFile on success. */
	vf = VFileMemChunk(data, size);
	if (!vf) return -1;
	if (!c->core->loadSave(c->core, vf)) {
		vf->close(vf);
		return -2;
	}
	return 0;
}

int gbah_handoff_restore(gbah_core *c, const void *state, size_t state_size,
                         const void *sav, size_t sav_size)
{
	int r = gbah_state_load(c, state, state_size);
	if (r) return r;
	/* Must come AFTER the state: the state load forces the save type it was
	 * taken with (re-initialising the save memory if it differs). */
	if (sav && sav_size) return gbah_sram_import(c, sav, sav_size);
	return 0;
}

const char *gbah_core_version(void)
{
	return projectVersion;
}

void gbah_free(void *p)
{
	free(p);
}

int gbah_read_file(const char *path, void **out, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;
	void *buf;
	*out = NULL;
	*size = 0;
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = buf;
	*size = (size_t)n;
	return 0;
}

int gbah_write_file(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	int ok;
	if (!f) return -1;
	ok = fwrite(data, 1, size, f) == size;
	return (fclose(f) == 0 && ok) ? 0 : -1;
}

uint64_t gbah_framebuffer_hash(const gbah_core *c)
{
	const uint8_t *p = (const uint8_t *)c->video;
	size_t i, n = (size_t)GBAH_WIDTH * GBAH_HEIGHT * 4;
	uint64_t h = 1469598103934665603ULL;
	for (i = 0; i < n; i++) {
		/* Ignore the X byte (alpha-like), mGBA may leave it as 0 or 0xFF. */
		if ((i & 3) == 3) continue;
		h = (h ^ p[i]) * 1099511628211ULL;
	}
	return h;
}
