// gbahandoff desktop: SDL3 front-end over gbah_core + the handoff hub (handoff.h).
// usage: gbahandoff [--software] [--frames N] [--port N] [--adb PATH] [--no-link] [--data-dir DIR]
//                   [--exit-after-sessions N] [--mirror-ms N] [--sav-min-ms N] [rom.gba]
//   With a ROM argument it is imported into the library and started; without one the menu opens
//   (arrows + Enter; drop a .gba on the window to import it).
//   Library: %APPDATA%/GBAHandoff (Windows) or $XDG_DATA_HOME/gbahandoff (see library.h).
//   Cable link: TCP server on 127.0.0.1:27183 (default) + adb watcher; see link.h.
//   keys: arrows, Z=A X=B A=L S=R Enter=Start Backspace=Select
//   F2 hand the game to the phone, F5 save, F8 reload the saved point, F11 fullscreen, Esc menu/quit
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "handoff.h"
#include "library.h"
#include "link.h"

extern "C" {
#include "gbah_core.h"
}

namespace {

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

// ---- menu ----------------------------------------------------------------

const int kMenuW = 400, kMenuH = 240;

void text(SDL_Renderer *r, float x, float y, const std::string &s, Uint8 R = 220, Uint8 G = 220, Uint8 B = 220)
{
	SDL_SetRenderDrawColor(r, R, G, B, 255);
	SDL_RenderDebugText(r, x, y, s.c_str());
}

void box(SDL_Renderer *r, float x, float y, float w, float h, Uint8 a)
{
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, a);
	SDL_FRect f{x, y, w, h};
	SDL_RenderFillRect(r, &f);
}

std::string when(uint64_t ms)
{
	if (!ms) return "";
	SDL_DateTime dt;
	if (!SDL_TimeToDateTime((SDL_Time)ms * 1000000, &dt, true)) return "";
	char b[32];
	snprintf(b, sizeof b, "%02d/%02d %02d:%02d", dt.day, dt.month, dt.hour, dt.minute);
	return b;
}

std::string game_status(const cable::GameInfo &g)
{
	if (g.conflict) return "CONFLITO";
	if (!g.meta.has_data) return "novo";
	std::string s = when(g.meta.timestamp_ms);
	if (gbah_ver_eq(&g.meta.base, &g.meta.version)) s += " sincronizado";
	return s;
}

void draw_menu(SDL_Renderer *ren, cable::Library &lib, cable::Hub &hub, cable::Status &st, int sel, int top, bool link_on)
{
	SDL_SetRenderDrawColor(ren, 16, 16, 28, 255);
	SDL_RenderClear(ren);
	text(ren, 8, 8, "GBA Handoff", 255, 255, 255);
	text(ren, 8, 20, "setas: escolher   Enter: jogar   Esc: sair   arraste um .gba aqui para importar", 140, 140, 160);
	const int rows = 22;
	auto &games = lib.games();
	if (games.empty()) {
		text(ren, 16, 48, "Biblioteca vazia.");
		text(ren, 16, 60, "Arraste uma ROM .gba para esta janela, ou", 160, 160, 160);
		text(ren, 16, 72, "conecte o celular e entregue um jogo a ele.", 160, 160, 160);
	}
	for (int i = 0; i < rows && top + i < (int)games.size(); i++) {
		const cable::GameInfo &g = games[(size_t)(top + i)];
		bool s = top + i == sel;
		std::string line = std::string(s ? "> " : "  ") + g.title;
		if (line.size() < 22) line.append(22 - line.size(), ' ');
		line += " " + game_status(g);
		if (s) {
			SDL_SetRenderDrawColor(ren, 40, 60, 110, 255);
			SDL_FRect f{6, 38 + i * 9.f, kMenuW - 12.f, 9};
			SDL_RenderFillRect(ren, &f);
		}
		text(ren, 8, 39 + i * 9.f, line, g.conflict ? 255 : 220, g.conflict ? 120 : 220, g.conflict ? 120 : 220);
	}
	// status
	box(ren, 0, kMenuH - 36, kMenuW, 36, 255);
	if (!link_on) text(ren, 8, kMenuH - 32, "Cabo desativado (--no-link)", 200, 200, 120);
	else if (st.port_busy()) text(ren, 8, kMenuH - 32, "PORTA OCUPADA: outra instancia ja esta aberta", 255, 100, 100);
	else if (st.connected()) text(ren, 8, kMenuH - 32, "Conectado: " + st.device(), 120, 230, 120);
	else text(ren, 8, kMenuH - 32, "Sem celular", 200, 200, 200);
	if (link_on) text(ren, 8, kMenuH - 22, st.note(), 150, 150, 170);
	if (hub.osd_active()) text(ren, 8, kMenuH - 11, hub.osd(), 255, 230, 120);
}

void draw_game_overlay(SDL_Renderer *ren, cable::Hub &hub, cable::Status &st)
{
	std::string mt = hub.mode_text();
	if (!mt.empty()) {
		box(ren, 0, 56, GBAH_WIDTH, 48, 200);
		text(ren, (GBAH_WIDTH - (float)mt.size() * 8) / 2, 64, mt, 255, 255, 255);
		if (hub.mode() == cable::Hub::Mode::WaitPhone) {
			text(ren, 8, 80, st.connected() ? "Devolva o jogo pelo celular" : "Enter: continuar aqui", 200, 200, 200);
			text(ren, 8, 90, "Esc: menu", 160, 160, 160);
		} else if (hub.mode() == cable::Hub::Mode::Conflict) {
			text(ren, 8, 80, "Mostrando as duas versoes no", 200, 200, 200);
			text(ren, 8, 90, "celular. Esperando a escolha.", 200, 200, 200);
		}
	}
	if (hub.osd_active()) {
		box(ren, 0, GBAH_HEIGHT - 12, GBAH_WIDTH, 12, 170);
		text(ren, 4, GBAH_HEIGHT - 10, hub.osd().substr(0, 29), 255, 235, 150);
	}
}

} // namespace

int main(int argc, char **argv)
{
	const char *rom_path = nullptr;
	std::string data_dir;
	bool software = false;
	long max_frames = -1;
	long port = GBAHP_DEFAULT_PORT;
	std::string adb_path;
	bool no_link = false;
	long exit_after_sessions = 0;   // test hook: quit once N connections have ended
	cable::HubOptions ho;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--software")) software = true;
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atol(argv[++i]);
		else if (!strcmp(argv[i], "--adb") && i + 1 < argc) adb_path = argv[++i];
		else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) data_dir = argv[++i];
		else if (!strcmp(argv[i], "--no-link")) no_link = true;
		else if (!strcmp(argv[i], "--exit-after-sessions") && i + 1 < argc) exit_after_sessions = atol(argv[++i]);
		else if (!strcmp(argv[i], "--mirror-ms") && i + 1 < argc) ho.mirror_ms = (uint32_t)atol(argv[++i]);
		else if (!strcmp(argv[i], "--sav-min-ms") && i + 1 < argc) ho.sav_min_ms = (uint32_t)atol(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
		else if (argv[i][0] == '-' && argv[i][1] == '-') {
			fatal("usage: gbahandoff [--software] [--frames N] [--port N] [--adb PATH] [--no-link] [--data-dir DIR] [rom.gba]");
			return 2;
		} else rom_path = argv[i];
	}

	cable::Library lib;
	if (!lib.open(data_dir)) {
		fatal("Could not open the game library folder (%APPDATA%/GBAHandoff).");
		return 1;
	}
	gbah_core *core = gbah_create(0);
	if (!core) {
		fatal("Could not create the emulator core.");
		return 1;
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

	// Disk worker, cable link (server thread + adb watcher) and the handoff logic. None of the
	// threads touches the core; the main loop only enqueues work for them.
	cable::DiskQueue disk;
	cable::Status link_status;
	cable::Server server(link_status);
	cable::AdbWatcher adb(link_status, (uint16_t)port, adb_path);
	cable::Hub hub(core, lib, disk, server, link_status, ho);
	if (!no_link) {
		cable::ServerOptions lo;
		lo.port = (uint16_t)port;
		memcpy(lo.device_id, lib.device_id(), 16);
		lo.device_name = host_name();
		lo.core_version = gbah_core_version();
		lo.roms_dir = lib.root() + "/roms";
		if (server.start(lo)) adb.start();
		else link_status.set_port_busy((uint16_t)port);
	}

	if (rom_path) {
		cable::GameInfo *g = hub.import_rom(rom_path);
		if (!g || !hub.open_game(g->sha1)) {
			fatal("Could not load ROM (file missing or not a GBA ROM).");
			return 1;
		}
	}

	unsigned seen_gen = 0;
	std::string seen_title;
	cable::Hub::Mode seen_mode = cable::Hub::Mode::Playing;
	bool seen_mode_valid = false;
	int sel = 0, top = 0;
	std::vector<SDL_Gamepad *> pads;
	std::vector<int16_t> abuf(4096 * 2);

	const Uint64 frame_ns = 16742706; // 1 / 59.7275 Hz
	Uint64 next_ns = SDL_GetTicksNS();
	bool running = true;
	long frames = 0;

	while (running) {
		SDL_Event e;
		const bool in_menu = hub.mode() == cable::Hub::Mode::Menu;
		auto n_games = (int)lib.games().size();
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_EVENT_QUIT: running = false; break;
			case SDL_EVENT_DROP_FILE:
				if (e.drop.data) {
					if (!hub.import_rom(e.drop.data)) SDL_Log("drop: not a ROM: %s", e.drop.data);
				}
				break;
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
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				if (in_menu && n_games) {
					if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) sel = (sel + n_games - 1) % n_games;
					else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) sel = (sel + 1) % n_games;
					else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) hub.open_game(lib.games()[(size_t)sel].sha1);
				}
				break;
			case SDL_EVENT_KEY_DOWN: {
				if (e.key.repeat && !in_menu) break;
				const SDL_Scancode k = e.key.scancode;
				if (k == SDL_SCANCODE_F11) {
					SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
				} else if (in_menu) {
					if (k == SDL_SCANCODE_ESCAPE) running = false;
					else if (n_games && k == SDL_SCANCODE_UP) sel = (sel + n_games - 1) % n_games;
					else if (n_games && k == SDL_SCANCODE_DOWN) sel = (sel + 1) % n_games;
					else if (n_games && (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER)) hub.open_game(lib.games()[(size_t)sel].sha1);
				} else if (k == SDL_SCANCODE_ESCAPE) {
					hub.close_game();
				} else if (k == SDL_SCANCODE_F2) {
					hub.request_return();
				} else if (k == SDL_SCANCODE_F5) {
					hub.quick_save();
				} else if (k == SDL_SCANCODE_F8) {
					hub.quick_load();
				} else if (k == SDL_SCANCODE_RETURN && hub.mode() == cable::Hub::Mode::WaitPhone) {
					hub.resume_local();
				}
				break;
			}
			default: break;
			}
		}
		if (n_games) { if (sel >= n_games) sel = n_games - 1; } else sel = 0;
		if (sel < top) top = sel;
		if (sel >= top + 22) top = sel - 21;

		hub.tick();
		if (hub.take_audio_flush() && audio) SDL_ClearAudioStream(audio);

		if (unsigned g = link_status.generation(); g != seen_gen || hub.mode() != seen_mode || !seen_mode_valid) {
			seen_gen = g;
			std::string t = link_status.title();
			std::string mt = hub.mode_text();
			if (!mt.empty()) t += " - " + mt;
			if (t != seen_title) { SDL_SetWindowTitle(win, t.c_str()); seen_title = t; }
		}
		if (!seen_mode_valid || hub.mode() != seen_mode) {
			const bool menu = hub.mode() == cable::Hub::Mode::Menu;
			if (menu) SDL_SetRenderLogicalPresentation(ren, kMenuW, kMenuH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
			else SDL_SetRenderLogicalPresentation(ren, GBAH_WIDTH, GBAH_HEIGHT, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
			seen_mode = hub.mode();
			seen_mode_valid = true;
		}
		if (exit_after_sessions > 0 && (long)server.sessions_ended() >= exit_after_sessions) running = false;

		if (hub.mode() == cable::Hub::Mode::Menu || !hub.emulating()) {
			// Not emulating: draw at a relaxed pace (the audio device is not the clock now).
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
			if (hub.mode() == cable::Hub::Mode::Menu) {
				draw_menu(ren, lib, hub, link_status, sel, top, !no_link);
			} else {
				SDL_RenderClear(ren);
				SDL_UpdateTexture(tex, nullptr, gbah_framebuffer(core), GBAH_WIDTH * 4);
				SDL_RenderTexture(ren, tex, nullptr, nullptr);
				draw_game_overlay(ren, hub, link_status);
			}
			SDL_RenderPresent(ren);
			SDL_Delay(16);
			next_ns = SDL_GetTicksNS();
			continue;
		}

		uint32_t keys = read_keyboard();
		for (SDL_Gamepad *g : pads) keys |= read_gamepad(g);
		// The phone as a second controller: only with a live session and while the game runs here
		// (this point is reached only when Playing). Dropped sessions zero keys_ in link.cpp.
		if (link_status.connected()) keys |= server.remote_keys() & GBAH_KEY_MASK;
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
		hub.on_frames(todo);
		SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
		SDL_UpdateTexture(tex, nullptr, gbah_framebuffer(core), GBAH_WIDTH * 4);
		SDL_RenderClear(ren);
		SDL_RenderTexture(ren, tex, nullptr, nullptr);
		if (hub.osd_active()) draw_game_overlay(ren, hub, link_status);
		SDL_RenderPresent(ren);
		if (max_frames >= 0 && frames >= max_frames) running = false;
	}

	// Final checkpoint (+ a last mirror to the phone), then let the disk worker finish before the
	// link goes away: its closures send the ACKs.
	hub.shutdown();
	disk.drain_and_stop();
	adb.stop();
	server.stop();
	for (SDL_Gamepad *g : pads) SDL_CloseGamepad(g);
	if (audio) SDL_DestroyAudioStream(audio);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	gbah_destroy(core);
	return 0;
}
