// gbahandoff desktop: SDL3 front-end over gbah_core.
// usage: gbahandoff [--software] [--frames N] [--port N] [--adb PATH] [--no-link]
//                   [--exit-after-sessions N] <rom.gba>
//   cable link: TCP server on 127.0.0.1:27183 (default) + adb watcher; see link.h
//   keys: arrows, Z=A X=B A=L S=R Enter=Start Backspace=Select
//   F5 save state (<rom>.state, also writes <rom>.sav), F8 load state, F11 fullscreen, Esc quit
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "link.h"

extern "C" {
#include "gbah_core.h"
}

namespace {

struct Pad {
	SDL_Gamepad *gp = nullptr;
};

uint32_t read_keyboard()
{
	const bool *k = SDL_GetKeyboardState(nullptr);
	uint32_t m = 0;
	if (k[SDL_SCANCODE_Z]) m |= GBAH_KEY_A;
	if (k[SDL_SCANCODE_X]) m |= GBAH_KEY_B;
	if (k[SDL_SCANCODE_A]) m |= GBAH_KEY_L;
	if (k[SDL_SCANCODE_S]) m |= GBAH_KEY_R;
	if (k[SDL_SCANCODE_RETURN]) m |= GBAH_KEY_START;
	if (k[SDL_SCANCODE_BACKSPACE]) m |= GBAH_KEY_SELECT;
	if (k[SDL_SCANCODE_UP]) m |= GBAH_KEY_UP;
	if (k[SDL_SCANCODE_DOWN]) m |= GBAH_KEY_DOWN;
	if (k[SDL_SCANCODE_LEFT]) m |= GBAH_KEY_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) m |= GBAH_KEY_RIGHT;
	return m;
}

uint32_t read_gamepad(SDL_Gamepad *g)
{
	uint32_t m = 0;
	auto b = [&](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
	// Nintendo layout: east = A, south = B.
	if (b(SDL_GAMEPAD_BUTTON_EAST)) m |= GBAH_KEY_A;
	if (b(SDL_GAMEPAD_BUTTON_SOUTH)) m |= GBAH_KEY_B;
	if (b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) m |= GBAH_KEY_L;
	if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) m |= GBAH_KEY_R;
	if (b(SDL_GAMEPAD_BUTTON_START)) m |= GBAH_KEY_START;
	if (b(SDL_GAMEPAD_BUTTON_BACK)) m |= GBAH_KEY_SELECT;
	if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= GBAH_KEY_UP;
	if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= GBAH_KEY_DOWN;
	if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= GBAH_KEY_LEFT;
	if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= GBAH_KEY_RIGHT;
	const int dead = 16000;
	int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
	int y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
	if (x < -dead) m |= GBAH_KEY_LEFT;
	if (x > dead) m |= GBAH_KEY_RIGHT;
	if (y < -dead) m |= GBAH_KEY_UP;
	if (y > dead) m |= GBAH_KEY_DOWN;
	return m;
}

void fatal(const char *msg)
{
	SDL_Log("%s", msg);
	// With -mwindows there is no console on Windows: make the error visible.
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "gbahandoff", msg, nullptr);
}

bool save_sram(gbah_core *c, const std::string &path)
{
	void *s = nullptr;
	size_t n = gbah_sram_export(c, &s);
	if (!n) return false;
	bool ok = gbah_write_file(path.c_str(), s, n) == 0;
	gbah_free(s);
	return ok;
}

std::string host_name()
{
#ifdef _WIN32
	const char *h = getenv("COMPUTERNAME");
	return h && *h ? h : "PC";
#else
	char b[128] = {};
	if (gethostname(b, sizeof b - 1) != 0 || !*b) return "PC";
	return b;
#endif
}

// Stable per-installation id (16 random bytes kept in the user's pref dir).
void load_device_id(uint8_t id[16])
{
	std::string path;
	if (char *pref = SDL_GetPrefPath("gbahandoff", "gbahandoff")) {
		path = std::string(pref) + "device_id";
		SDL_free(pref);
	}
	void *buf = nullptr;
	size_t n = 0;
	if (!path.empty() && gbah_read_file(path.c_str(), &buf, &n) == 0 && n == 16) {
		memcpy(id, buf, 16);
		gbah_free(buf);
		return;
	}
	if (buf) gbah_free(buf);
	std::random_device rd;
	for (int i = 0; i < 16; i++) id[i] = (uint8_t)rd();
	if (!path.empty()) gbah_write_file(path.c_str(), id, 16);
}

} // namespace

int main(int argc, char **argv)
{
	const char *rom_path = nullptr;
	bool software = false;
	long max_frames = -1;
	long port = GBAHP_DEFAULT_PORT;
	std::string adb_path;
	bool no_link = false;
	long exit_after_sessions = 0;   // test hook: quit once N connections have ended
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--software")) software = true;
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atol(argv[++i]);
		else if (!strcmp(argv[i], "--adb") && i + 1 < argc) adb_path = argv[++i];
		else if (!strcmp(argv[i], "--no-link")) no_link = true;
		else if (!strcmp(argv[i], "--exit-after-sessions") && i + 1 < argc) exit_after_sessions = atol(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
		else rom_path = argv[i];
	}
	if (!rom_path) {
		fatal("usage: gbahandoff [--software] [--frames N] [--port N] [--adb PATH] [--no-link] <rom.gba>");
		return 2;
	}
	const std::string state_path = std::string(rom_path) + ".state";
	const std::string sav_path = std::string(rom_path) + ".sav";

	gbah_core *core = gbah_create(0);
	if (!core || gbah_load_rom_file(core, rom_path)) {
		fatal("Could not load ROM (file missing or not a GBA ROM).");
		return 1;
	}
	{
		void *sav = nullptr;
		size_t n = 0;
		if (gbah_read_file(sav_path.c_str(), &sav, &n) == 0) {
			if (gbah_load_sav_buffer(core, sav, n)) SDL_Log("warning: could not import %s", sav_path.c_str());
			gbah_free(sav);
		}
	}

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
		fatal(SDL_GetError());
		return 1;
	}
	SDL_Window *win = nullptr;
	SDL_Renderer *ren = nullptr;
	if (!SDL_CreateWindowAndRenderer("GBA Handoff", GBAH_WIDTH * 3, GBAH_HEIGHT * 3, SDL_WINDOW_RESIZABLE, &win, &ren)) {
		fatal(SDL_GetError());
		return 1;
	}
	if (software) { // re-create with the software renderer (lightest option on old GPUs)
		SDL_DestroyRenderer(ren);
		ren = SDL_CreateRenderer(win, "software");
		if (!ren) { fatal(SDL_GetError()); return 1; }
	}
	SDL_SetRenderVSync(ren, 1);
	SDL_SetRenderLogicalPresentation(ren, GBAH_WIDTH, GBAH_HEIGHT, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, GBAH_WIDTH, GBAH_HEIGHT);
	if (!tex) { fatal(SDL_GetError()); return 1; }
	SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

	const unsigned rate = gbah_audio_rate(core);
	SDL_AudioSpec spec{};
	spec.format = SDL_AUDIO_S16;
	spec.channels = 2;
	spec.freq = (int)rate;
	SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (audio) SDL_ResumeAudioStreamDevice(audio);
	else SDL_Log("audio disabled: %s", SDL_GetError());
	const int bytes_per_frame = 4; // s16 stereo
	const int target_queue = (int)(rate / 1000 * 60) * bytes_per_frame; // keep ~60 ms queued

	// Cable link: server thread + adb watcher; they never touch the render loop.
	cable::Status link_status;
	cable::Server server(link_status);
	cable::AdbWatcher adb(link_status, (uint16_t)port, adb_path);
	if (!no_link) {
		cable::ServerOptions lo;
		lo.port = (uint16_t)port;
		load_device_id(lo.device_id);
		lo.device_name = host_name();
		lo.core_version = gbah_core_version();
		gbahp_lib_entry e;
		SDL_PathInfo info;
		if (gbah_rom_sha1(core, e.sha1) == 0 && SDL_GetPathInfo(rom_path, &info)) {
			e.size = (uint64_t)info.size;
			lo.library.push_back(e);
		}
		if (server.start(lo)) adb.start();
		else link_status.set_adb_note("porta " + std::to_string(port) + " ocupada (outra instância aberta?)");
	}
	unsigned seen_gen = 0;

	std::vector<SDL_Gamepad *> pads;
	std::vector<int16_t> abuf(4096 * 2);

	const Uint64 frame_ns = 16742706; // 1 / 59.7275 Hz
	Uint64 next_ns = SDL_GetTicksNS();
	bool running = true;
	long frames = 0;

	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_EVENT_QUIT: running = false; break;
			case SDL_EVENT_GAMEPAD_ADDED: {
				SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which);
				if (g) pads.push_back(g);
				break;
			}
			case SDL_EVENT_GAMEPAD_REMOVED:
				for (size_t i = 0; i < pads.size(); i++)
					if (SDL_GetGamepadID(pads[i]) == e.gdevice.which) {
						SDL_CloseGamepad(pads[i]);
						pads.erase(pads.begin() + (long)i);
						break;
					}
				break;
			case SDL_EVENT_KEY_DOWN:
				if (e.key.repeat) break;
				if (e.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
				else if (e.key.scancode == SDL_SCANCODE_F11)
					SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
				else if (e.key.scancode == SDL_SCANCODE_F5) {
					void *st = nullptr;
					size_t n = gbah_state_save(core, &st);
					bool ok = n && gbah_write_file(state_path.c_str(), st, n) == 0;
					gbah_free(st);
					save_sram(core, sav_path);
					SDL_SetWindowTitle(win, ok ? "GBA Handoff - state saved" : "GBA Handoff - SAVE FAILED");
				} else if (e.key.scancode == SDL_SCANCODE_F8) {
					void *st = nullptr;
					size_t n = 0;
					bool ok = gbah_read_file(state_path.c_str(), &st, &n) == 0 && gbah_state_load(core, st, n) == 0;
					gbah_free(st);
					SDL_SetWindowTitle(win, ok ? "GBA Handoff - state loaded" : "GBA Handoff - LOAD FAILED");
				}
				break;
			default: break;
			}
		}

		if (unsigned g = link_status.generation(); g != seen_gen) {
			seen_gen = g;
			SDL_SetWindowTitle(win, link_status.title().c_str());
		}
		if (exit_after_sessions > 0 && (long)server.sessions_ended() >= exit_after_sessions) running = false;

		uint32_t keys = read_keyboard();
		for (SDL_Gamepad *g : pads) keys |= read_gamepad(g);
		gbah_set_input(core, keys);

		// Pacing: the audio device is the clock. Without audio, fall back to a timer.
		int todo = 0;
		if (audio) {
			int queued = SDL_GetAudioStreamQueued(audio);
			if (queued < target_queue) todo = queued < target_queue / 4 ? 2 : 1;
		} else {
			Uint64 now = SDL_GetTicksNS();
			if (now >= next_ns) { todo = 1; next_ns += frame_ns; if (now > next_ns + 5 * frame_ns) next_ns = now; }
		}
		if (!todo) { SDL_Delay(1); continue; }

		for (int i = 0; i < todo; i++) {
			gbah_run_frame(core);
			frames++;
			size_t n = gbah_audio_read(core, abuf.data(), 4096);
			if (audio && n) SDL_PutAudioStreamData(audio, abuf.data(), (int)(n * bytes_per_frame));
		}
		SDL_UpdateTexture(tex, nullptr, gbah_framebuffer(core), GBAH_WIDTH * 4);
		SDL_RenderClear(ren);
		SDL_RenderTexture(ren, tex, nullptr, nullptr);
		SDL_RenderPresent(ren);
		if (max_frames >= 0 && frames >= max_frames) running = false;
	}

	adb.stop();
	server.stop();
	save_sram(core, sav_path);
	for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
	if (audio) SDL_DestroyAudioStream(audio);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	gbah_destroy(core);
	return 0;
}
