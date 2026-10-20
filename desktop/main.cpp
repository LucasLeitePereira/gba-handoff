// gbahandoff desktop: SDL3 front-end over gbah_core + the handoff hub (handoff.h).
// usage: gbahandoff [--software] [--frames N] [--port N] [--adb PATH] [--apk PATH] [--no-link] [--data-dir DIR]
//                   [--exit-after-sessions N] [--mirror-ms N] [--sav-min-ms N] [rom.gba]
//   With a ROM argument it is imported into the library and started; without one the menu opens
//   (arrows + Enter; drop a .gba on the window to import it).
//   Library: %APPDATA%/GBAHandoff (Windows) or $XDG_DATA_HOME/gbahandoff (see library.h).
//   Cable link: TCP server on 127.0.0.1:27183 (default) + adb watcher; see link.h.
//   keys (default map, remappable in the Controls screen): arrows, Z=A X=B A=L S=R Enter=Start Backspace=Select
//   F2 hand the game to the phone, F5 save, F8 reload the saved point, F11 fullscreen,
//   M mute, -/+ volume, Esc = pause menu in game (quit in the main menu), I = install/update the phone app.
//
// Drawing: no SDL logical presentation. Every frame is cleared with the menu colour over the WHOLE
// target, then the content is drawn through a render scale + origin offset that this file computes
// (menu: letterbox 400x240; game: 28 px bar on top + 240x160 at an integer scale centred below).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
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
#include "version.h"

extern "C" {
#include "gbah_core.h"
}

namespace {

using cable::Hub;
using cable::PhoneState;

const int kMenuW = 400, kMenuH = 240;   // menu virtual size
const int kBarH = 28;                   // bar above the game
const Uint8 kBgR = 16, kBgG = 16, kBgB = 28;   // menu colour = background everywhere

// GBA button bits in the order of cable::Settings::keys.
const uint32_t kBtnBits[cable::kNumPcButtons] = {
	GBAH_KEY_A, GBAH_KEY_B, GBAH_KEY_L, GBAH_KEY_R, GBAH_KEY_START,
	GBAH_KEY_SELECT, GBAH_KEY_UP, GBAH_KEY_DOWN, GBAH_KEY_LEFT, GBAH_KEY_RIGHT};

uint32_t read_keyboard(const cable::Settings &cfg)
{
	const bool *k = SDL_GetKeyboardState(nullptr);
	uint32_t m = 0;
	for (int i = 0; i < cable::kNumPcButtons; i++)
		if (k[cfg.keys[i]]) m |= kBtnBits[i];
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

// ---- drawing helpers -------------------------------------------------------

// Origin of the virtual canvas, in virtual units (see begin_view). NOT done with SDL_SetRenderViewport:
// SDL3 keeps the viewport in scaled units, so with a render scale s its position is multiplied by s and
// cannot be placed at an arbitrary pixel. Every helper below adds this offset instead.
float g_ox = 0, g_oy = 0;

void text(SDL_Renderer *r, float x, float y, const std::string &s, Uint8 R = 220, Uint8 G = 220, Uint8 B = 220)
{
	SDL_SetRenderDrawColor(r, R, G, B, 255);
	SDL_RenderDebugText(r, x + g_ox, y + g_oy, s.c_str());
}

void ctext(SDL_Renderer *r, float cx, float y, const std::string &s, Uint8 R = 220, Uint8 G = 220, Uint8 B = 220)
{
	text(r, cx - (float)s.size() * 4, y, s, R, G, B);
}

void fill(SDL_Renderer *r, float x, float y, float w, float h, Uint8 R, Uint8 G, Uint8 B, Uint8 a = 255)
{
	SDL_SetRenderDrawBlendMode(r, a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, R, G, B, a);
	SDL_FRect f{x + g_ox, y + g_oy, w, h};
	SDL_RenderFillRect(r, &f);
}

void box(SDL_Renderer *r, float x, float y, float w, float h, Uint8 a) { fill(r, x, y, w, h, 0, 0, 0, a); }

std::vector<std::string> wrap(const std::string &s, size_t width)
{
	std::vector<std::string> out;
	std::string line;
	size_t i = 0;
	while (i < s.size()) {
		size_t sp = s.find(' ', i);
		std::string w = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
		i = sp == std::string::npos ? s.size() : sp + 1;
		while (w.size() > width) { out.push_back(w.substr(0, width)); w = w.substr(width); }
		if (!line.empty() && line.size() + 1 + w.size() > width) { out.push_back(line); line.clear(); }
		line += (line.empty() ? "" : " ") + w;
	}
	if (!line.empty()) out.push_back(line);
	return out;
}

// Content area inside the render target: viewport rectangle + render scale. (vw, vh) = virtual size.
struct View {
	float s = 1;
	int ox = 0, oy = 0, vw = kMenuW, vh = kMenuH;
};

View menu_view(int pw, int ph)
{
	View v;
	v.vw = kMenuW; v.vh = kMenuH;
	v.s = std::max(0.25f, std::min((float)pw / kMenuW, (float)ph / kMenuH));
	v.ox = (int)((pw - kMenuW * v.s) / 2);
	v.oy = (int)((ph - kMenuH * v.s) / 2);
	return v;
}

View game_view(int pw, int ph)
{
	View v;
	v.vw = GBAH_WIDTH; v.vh = GBAH_HEIGHT;
	const int avail_h = std::max(1, ph - kBarH);
	float sc = std::min((float)pw / GBAH_WIDTH, (float)avail_h / GBAH_HEIGHT);
	v.s = sc >= 1 ? (float)(int)sc : std::max(0.25f, sc);   // integer scale; below 1x the only choice is to shrink
	v.ox = (int)((pw - GBAH_WIDTH * v.s) / 2);
	v.oy = kBarH + (int)((avail_h - GBAH_HEIGHT * v.s) / 2);
	return v;
}

void begin_view(SDL_Renderer *r, const View &v)
{
	SDL_SetRenderScale(r, v.s, v.s);
	g_ox = (float)v.ox / v.s;
	g_oy = (float)v.oy / v.s;
}

void end_view(SDL_Renderer *r)
{
	SDL_SetRenderScale(r, 1, 1);
	g_ox = g_oy = 0;
}

// ---- UI state --------------------------------------------------------------

enum class Screen { None, Esc, Remap };
enum class EscItem { Continue, Save, Load, Volume, Controls, Fullscreen, Return, ToMenu };

struct Ui {
	Screen screen = Screen::None;
	int sel = 0;                // Esc menu / remap list selection
	bool capture = false;       // remap: waiting for the new key
	std::string remap_msg;
	float mx = -1, my = -1;     // mouse, render pixels
	int menu_sel = 0, menu_top = 0;   // main menu
};

std::vector<EscItem> esc_items(const Hub &hub)
{
	std::vector<EscItem> v{EscItem::Continue};
	if (hub.mode() == Hub::Mode::Playing) { v.push_back(EscItem::Save); v.push_back(EscItem::Load); }
	v.push_back(EscItem::Volume);
	v.push_back(EscItem::Controls);
	v.push_back(EscItem::Fullscreen);
	if (hub.connected() && (hub.mode() == Hub::Mode::Playing || hub.mode() == Hub::Mode::AwaitPlace)) v.push_back(EscItem::Return);
	v.push_back(EscItem::ToMenu);
	return v;
}

std::string esc_label(EscItem it, const cable::Settings &cfg)
{
	switch (it) {
	case EscItem::Continue: return "Continuar";
	case EscItem::Save: return "Salvar (F5)";
	case EscItem::Load: return "Carregar (F8)";
	case EscItem::Volume: return cfg.muted ? "Volume: MUDO (Enter)" : "Volume: " + std::to_string(cfg.volume) + "% (</>)";
	case EscItem::Controls: return "Controles";
	case EscItem::Fullscreen: return "Tela cheia (F11)";
	case EscItem::Return: return "Devolver ao celular (F2)";
	case EscItem::ToMenu: return "Voltar ao menu";
	}
	return "";
}

// Esc menu / remap content block: always 240x160 virtual, centred in the view.
const float kBlockW = 240, kBlockH = 160;
const float kEscRowY0 = 22, kEscRowH = 14;
const int kRemapRows = cable::kNumPcButtons + 2;   // buttons, "Restaurar padrao", "Voltar"
const float kRemapRowY0 = 16, kRemapRowH = 11;

bool inside(float x, float y, float rx, float ry, float rw, float rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

void draw_esc(SDL_Renderer *ren, const View &v, const Hub &hub, const Ui &ui)
{
	const float bx = (v.vw - kBlockW) / 2, by = (v.vh - kBlockH) / 2;
	box(ren, bx, by, kBlockW, kBlockH, 190);
	ctext(ren, bx + kBlockW / 2, by + 7, "PAUSADO", 255, 255, 255);
	auto items = esc_items(hub);
	for (size_t i = 0; i < items.size(); i++) {
		const float y = by + kEscRowY0 + (float)i * kEscRowH;
		const bool s = (int)i == ui.sel;
		if (s) fill(ren, bx + 20, y - 1, kBlockW - 40, kEscRowH - 1, 40, 60, 110);
		text(ren, bx + 26, y + 2, std::string(s ? "> " : "  ") + esc_label(items[i], hub.settings()), s ? 255 : 210, s ? 255 : 210, s ? 255 : 210);
	}
	ctext(ren, bx + kBlockW / 2, by + kBlockH - 10, "Esc: continuar", 140, 140, 160);
}

void draw_remap(SDL_Renderer *ren, const View &v, const cable::Settings &cfg, const Ui &ui, bool opaque)
{
	const float bx = (v.vw - kBlockW) / 2, by = (v.vh - kBlockH) / 2;
	if (opaque) box(ren, bx, by, kBlockW, kBlockH, 215);
	ctext(ren, bx + kBlockW / 2, by + 4, "CONTROLES (teclado)", 255, 255, 255);
	for (int i = 0; i < kRemapRows; i++) {
		const float y = by + kRemapRowY0 + (float)i * kRemapRowH;
		const bool s = i == ui.sel;
		if (s) fill(ren, bx + 14, y - 1, kBlockW - 28, kRemapRowH - 1, 40, 60, 110);
		std::string line;
		Uint8 R = s ? 255 : 210, G = R, B = R;
		if (i < cable::kNumPcButtons) {
			std::string lab = cable::button_label(i);
			lab.resize(9, ' ');
			if (s && ui.capture) { line = lab + "Aperte a nova tecla..."; B = 100; }
			else {
				const char *n = SDL_GetScancodeName(cfg.keys[i]);
				line = lab + (n ? n : "?");
			}
		} else {
			line = i == cable::kNumPcButtons ? "Restaurar padrao" : "Voltar";
		}
		text(ren, bx + 20, y + 1, std::string(s ? "> " : "  ") + line, R, G, B);
	}
	const float hy = by + kRemapRowY0 + (float)kRemapRows * kRemapRowH + 2;
	if (!ui.remap_msg.empty()) ctext(ren, bx + kBlockW / 2, hy, ui.remap_msg.substr(0, 29), 255, 200, 100);
	else ctext(ren, bx + kBlockW / 2, hy, ui.capture ? "Esc: cancelar" : "Enter: trocar  Esc: voltar", 140, 140, 160);
}

// ---- bar (game mode) ---------------------------------------------------------

struct BarLayout { SDL_FRect mute, minus, label, plus, pad, menu; };

BarLayout bar_layout(int pw)
{
	const float bw = 20, gap = 4, y = 4;
	float x = (float)pw - 4;
	BarLayout b;
	b.menu = {x - bw, y, bw, bw};   x -= bw + gap;
	b.pad = {x - bw, y, bw, bw};    x -= bw + gap;
	b.plus = {x - bw, y, bw, bw};   x -= bw + 2;
	b.label = {x - 36, y, 36, bw};  x -= 36 + 2;
	b.minus = {x - bw, y, bw, bw};  x -= bw + gap;
	b.mute = {x - bw, y, bw, bw};
	return b;
}

bool hit(const SDL_FRect &r, float x, float y) { return inside(x, y, r.x, r.y, r.w, r.h); }

void icon_bg(SDL_Renderer *ren, const SDL_FRect &r, bool hover)
{
	if (hover) fill(ren, r.x, r.y, r.w, r.h, 70, 85, 140);
	else fill(ren, r.x, r.y, r.w, r.h, 30, 30, 52);
}

void line(SDL_Renderer *ren, float x1, float y1, float x2, float y2)
{
	SDL_RenderLine(ren, x1, y1, x2, y2);
}

void draw_bar(SDL_Renderer *ren, int pw, const Ui &ui, const cable::Settings &cfg, const std::string &status)
{
	fill(ren, 0, 0, (float)pw, (float)kBarH, kBgR, kBgG, kBgB);
	const BarLayout b = bar_layout(pw);
	const float mx = ui.mx, my = ui.my;
	const Uint8 W = 225;

	icon_bg(ren, b.mute, hit(b.mute, mx, my));
	{   // speaker: box + cone, then waves (on) or a red cross (muted)
		const float x = b.mute.x, y = b.mute.y;
		SDL_SetRenderDrawColor(ren, W, W, W, 255);
		SDL_FRect body{x + 4, y + 7, 4, 6};
		SDL_RenderFillRect(ren, &body);
		for (int i = 0; i < 5; i++) line(ren, x + 8 + (float)i, y + 10 - (3 + (float)i), x + 8 + (float)i, y + 10 + (3 + (float)i));
		if (cfg.muted || cfg.volume == 0) {
			SDL_SetRenderDrawColor(ren, 255, 90, 90, 255);
			line(ren, x + 14, y + 7, x + 19, y + 13);
			line(ren, x + 14, y + 13, x + 19, y + 7);
		} else {
			line(ren, x + 15, y + 7, x + 15, y + 13);
			line(ren, x + 18, y + 5, x + 18, y + 15);
		}
	}
	icon_bg(ren, b.minus, hit(b.minus, mx, my));
	fill(ren, b.minus.x + 5, b.minus.y + 9, 10, 2, W, W, W);
	{
		char buf[16];
		if (cfg.muted) snprintf(buf, sizeof buf, "MUDO");
		else snprintf(buf, sizeof buf, "%d%%", cfg.volume);
		ctext(ren, b.label.x + b.label.w / 2, b.label.y + 6, buf, cfg.muted ? 255 : 220, cfg.muted ? 120 : 220, cfg.muted ? 120 : 220);
	}
	icon_bg(ren, b.plus, hit(b.plus, mx, my));
	fill(ren, b.plus.x + 5, b.plus.y + 9, 10, 2, W, W, W);
	fill(ren, b.plus.x + 9, b.plus.y + 5, 2, 10, W, W, W);
	icon_bg(ren, b.pad, hit(b.pad, mx, my));
	{   // controller: outline, d-pad, two buttons
		const float x = b.pad.x, y = b.pad.y;
		SDL_SetRenderDrawColor(ren, W, W, W, 255);
		SDL_FRect o{x + 2, y + 5, 16, 10};
		SDL_RenderRect(ren, &o);
		line(ren, x + 4, y + 10, x + 8, y + 10);
		line(ren, x + 6, y + 8, x + 6, y + 12);
		fill(ren, x + 11, y + 9, 2, 2, W, W, W);
		fill(ren, x + 14, y + 8, 2, 2, W, W, W);
	}
	icon_bg(ren, b.menu, hit(b.menu, mx, my));
	for (int i = 0; i < 3; i++) fill(ren, b.menu.x + 4, b.menu.y + 5 + (float)i * 5, 12, 2, W, W, W);

	// status text on the left, as much as fits before the icons
	const int room = (int)((b.mute.x - 12) / 8);
	if (room > 3) text(ren, 6, 10, status.substr(0, (size_t)room), 170, 175, 200);
}

// ---- main menu ---------------------------------------------------------------

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

const int kMenuRows = 13;                       // visible game rows
const float kRowsY0 = 38, kRowH = 9;
const float kSettingsY = 158, kControlsY = 169;
const float kCardY = 184;
const SDL_FRect kInstallBtn{296, 185, 96, 13};

// Menu rows: games..., [prefer newest], [controls], [install button: only when available]
int menu_row_count(int n_games, bool can_install) { return n_games + 2 + (can_install ? 1 : 0); }

void draw_menu(SDL_Renderer *ren, cable::Library &lib, Hub &hub, cable::Status &st, cable::AdbWatcher &adb,
               const Ui &ui, bool link_on)
{
	text(ren, 8, 8, std::string("GBA Handoff v") + GBAH_APP_VERSION_NAME, 255, 255, 255);
	text(ren, 8, 20, "Setas+Enter: jogar  Esc: sair  Solte um .gba aqui", 140, 140, 160);
	auto &games = lib.games();
	const int n = (int)games.size();
	const bool can_install = link_on && adb.can_install();
	if (games.empty()) {
		text(ren, 16, 48, "Biblioteca vazia.");
		text(ren, 16, 60, "Arraste uma ROM .gba para esta janela, ou", 160, 160, 160);
		text(ren, 16, 72, "conecte o celular e entregue um jogo a ele.", 160, 160, 160);
	}
	for (int i = 0; i < kMenuRows && ui.menu_top + i < n; i++) {
		const cable::GameInfo &g = games[(size_t)(ui.menu_top + i)];
		bool s = ui.menu_top + i == ui.menu_sel;
		std::string ln = std::string(s ? "> " : "  ") + g.title;
		if (ln.size() < 22) ln.append(22 - ln.size(), ' ');
		ln += " " + game_status(g);
		if (s) fill(ren, 6, kRowsY0 + (float)i * kRowH, kMenuW - 12.f, 9, 40, 60, 110);
		text(ren, 8, kRowsY0 + 1 + (float)i * kRowH, ln, g.conflict ? 255 : 220, g.conflict ? 120 : 220, g.conflict ? 120 : 220);
	}
	{   // setting row + controls row
		const bool s = ui.menu_sel == n;
		if (s) fill(ren, 6, kSettingsY - 1, kMenuW - 12.f, 10, 40, 60, 110);
		text(ren, 8, kSettingsY, std::string(s ? "> " : "  ") + "Sempre manter a versao mais recente: " +
		     (hub.settings().prefer_newest ? "SIM" : "NAO"), 220, 220, 150);
		const bool c = ui.menu_sel == n + 1;
		if (c) fill(ren, 6, kControlsY - 1, kMenuW - 12.f, 10, 40, 60, 110);
		text(ren, 8, kControlsY, std::string(c ? "> " : "  ") + "Controles do teclado e volume...", 220, 220, 150);
	}

	// ---- status card: the phone (cable / app installed / up to date)
	fill(ren, 0, kCardY - 2, kMenuW, kMenuH - kCardY + 2, 8, 8, 16);
	const cable::PhoneInfo ph = adb.phone();
	std::string l1, l2, l3;
	Uint8 c1[3] = {200, 200, 200};
	auto col = [&](Uint8 r, Uint8 g, Uint8 b) { c1[0] = r; c1[1] = g; c1[2] = b; };
	bool refusal_shown = false;
	if (!link_on) { l1 = "Cabo desativado (--no-link)"; col(200, 200, 120); }
	else if (st.port_busy()) { l1 = "PORTA OCUPADA: outra instancia ja esta aberta"; col(255, 100, 100); }
	else if (st.connected()) { l1 = "Conectado: " + st.device(); col(120, 230, 120); }
	else switch (ph.state) {
	case PhoneState::None:
		l1 = "Nenhum celular"; l2 = "Ative a Depuracao USB no celular e plugue o cabo."; l3 = st.note();
		break;
	case PhoneState::Unauthorized:
		l1 = "Aceite a depuracao no celular"; col(255, 210, 100);
		l2 = "Toque em Permitir na janela que apareceu no aparelho.";
		break;
	case PhoneState::NoApp:
		l1 = "App nao instalado no celular"; col(255, 210, 100);
		l2 = ph.apk_found ? "Clique em Instalar ou tecle I." : "GBAHandoff.apk nao encontrado ao lado do .exe";
		break;
	case PhoneState::Outdated:
		l1 = "App desatualizado no celular"; col(255, 210, 100);
		l2 = ph.apk_found ? "Clique em Atualizar ou tecle I. Saves ficam." : "GBAHandoff.apk nao encontrado ao lado do .exe";
		break;
	case PhoneState::Installing:
		l1 = "Instalando..."; col(255, 210, 100); l2 = "Nao desconecte o cabo.";
		break;
	case PhoneState::Ready:
		l1 = "Pronto: abra o app no celular"; col(120, 230, 120); l2 = st.note();
		break;
	}
	text(ren, 8, kCardY + 3, l1, c1[0], c1[1], c1[2]);
	float y = kCardY + 14;
	if (!st.refusal().empty() && !st.connected()) {
		for (auto &w : wrap(st.refusal(), 48)) { text(ren, 8, y, w, 255, 100, 100); y += 10; if (y > kMenuH - 20) break; }
		refusal_shown = true;
	} else if (!ph.message.empty()) {
		for (auto &w : wrap(ph.message, 48)) { text(ren, 8, y, w, 255, 100, 100); y += 10; if (y > kMenuH - 20) break; }
	} else {
		const bool red = !ph.apk_found && (ph.state == PhoneState::NoApp || ph.state == PhoneState::Outdated);
		if (!l2.empty()) { text(ren, 8, y, l2, red ? 255 : 150, red ? 100 : 150, red ? 100 : 170); y += 10; }
		if (!l3.empty()) text(ren, 8, y, l3, 150, 150, 170);
	}
	(void)refusal_shown;
	if (can_install) {
		const bool s = ui.menu_sel == n + 2;
		const bool hover = hit(kInstallBtn, ui.mx, ui.my);
		fill(ren, kInstallBtn.x, kInstallBtn.y, kInstallBtn.w, kInstallBtn.h, s || hover ? 70 : 40, s || hover ? 100 : 60, s || hover ? 170 : 110);
		ctext(ren, kInstallBtn.x + kInstallBtn.w / 2, kInstallBtn.y + 3, ph.state == PhoneState::Outdated ? "Atualizar" : "Instalar", 255, 255, 255);
	}
	if (hub.osd_active()) text(ren, 8, kMenuH - 11, hub.osd(), 255, 230, 120);
	else if (!hub.pending_text().empty()) text(ren, 8, kMenuH - 11, hub.pending_text(), 255, 200, 100);
}

void draw_game_overlay(SDL_Renderer *ren, Hub &hub, cable::Status &st)
{
	std::string mt = hub.mode_text();
	if (!mt.empty()) {
		box(ren, 0, 56, GBAH_WIDTH, 48, 200);
		text(ren, (GBAH_WIDTH - (float)mt.size() * 8) / 2, 64, mt, 255, 255, 255);
		if (hub.mode() == Hub::Mode::AwaitPlace) {
			text(ren, 8, 80, st.connected() ? "Escolha no celular onde continuar" : "Enter: continuar aqui", 200, 200, 200);
			text(ren, 8, 90, "Esc: menu", 160, 160, 160);
		} else if (hub.mode() == Hub::Mode::WaitPhone) {
			text(ren, 8, 80, st.connected() ? "Devolva o jogo pelo celular" : "Enter: continuar aqui", 200, 200, 200);
			text(ren, 8, 90, "Esc: menu", 160, 160, 160);
		} else if (hub.mode() == Hub::Mode::Conflict) {
			text(ren, 8, 80, "Mostrando as duas versoes no", 200, 200, 200);
			text(ren, 8, 90, "celular. Esperando a escolha.", 200, 200, 200);
		}
	}
	if (hub.osd_active()) {
		box(ren, 0, GBAH_HEIGHT - 12, GBAH_WIDTH, 12, 170);
		text(ren, 4, GBAH_HEIGHT - 10, hub.osd().substr(0, 29), 255, 235, 150);
	}
}

float gain_of(const cable::Settings &s)
{
	if (s.muted) return 0;
	const float v = (float)s.volume / 100.f;
	return v * v;   // perceptual-ish curve: 50% is clearly half as loud, not barely quieter
}

} // namespace

int main(int argc, char **argv)
{
	const char *rom_path = nullptr;
	std::string data_dir;
	bool software = false;
	long max_frames = -1;
	long port = GBAHP_DEFAULT_PORT;
	std::string adb_path, apk_path;
	bool no_link = false;
	long exit_after_sessions = 0;   // test hook: quit once N connections have ended
	cable::HubOptions ho;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--software")) software = true;
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atol(argv[++i]);
		else if (!strcmp(argv[i], "--adb") && i + 1 < argc) adb_path = argv[++i];
		else if (!strcmp(argv[i], "--apk") && i + 1 < argc) apk_path = argv[++i];
		else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) data_dir = argv[++i];
		else if (!strcmp(argv[i], "--no-link")) no_link = true;
		else if (!strcmp(argv[i], "--exit-after-sessions") && i + 1 < argc) exit_after_sessions = atol(argv[++i]);
		else if (!strcmp(argv[i], "--mirror-ms") && i + 1 < argc) ho.mirror_ms = (uint32_t)atol(argv[++i]);
		else if (!strcmp(argv[i], "--sav-min-ms") && i + 1 < argc) ho.sav_min_ms = (uint32_t)atol(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
		else if (argv[i][0] == '-' && argv[i][1] == '-') {
			fatal("usage: gbahandoff [--software] [--frames N] [--port N] [--adb PATH] [--apk PATH] [--no-link] [--data-dir DIR] [rom.gba]");
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
	if (!SDL_CreateWindowAndRenderer("GBA Handoff", GBAH_WIDTH * 3, GBAH_HEIGHT * 3 + kBarH, SDL_WINDOW_RESIZABLE, &win, &ren)) {
		fatal(SDL_GetError());
		return 1;
	}
	if (software) { // re-create with the software renderer (lightest option on old GPUs)
		SDL_DestroyRenderer(ren);
		ren = SDL_CreateRenderer(win, "software");
		if (!ren) { fatal(SDL_GetError()); return 1; }
	}
	SDL_SetWindowMinimumSize(win, GBAH_WIDTH, GBAH_HEIGHT + kBarH);
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
	if (!apk_path.empty()) adb.set_apk_path(apk_path);
	cable::Hub hub(core, lib, disk, server, link_status, ho);
	if (audio) SDL_SetAudioStreamGain(audio, gain_of(hub.settings()));
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
	Ui ui;
	bool audio_paused = false;
	std::vector<SDL_Gamepad *> pads;
	std::vector<int16_t> abuf(4096 * 2);

	const Uint64 frame_ns = 16742706; // 1 / 59.7275 Hz
	Uint64 next_ns = SDL_GetTicksNS();
	bool running = true;
	long frames = 0;

	// ---- actions shared by keyboard, mouse and gamepad -------------------------------------
	auto in_menu_now = [&] { return hub.mode() == Hub::Mode::Menu; };
	auto set_local = [&](const cable::Settings &s) {
		hub.set_local_settings(s);
		if (audio) SDL_SetAudioStreamGain(audio, gain_of(hub.settings()));
	};
	auto toggle_mute = [&] { cable::Settings s = hub.settings(); s.muted = !s.muted; set_local(s); };
	auto change_volume = [&](int d) {
		cable::Settings s = hub.settings();
		s.volume = std::max(0, std::min(100, s.volume + d));
		if (d > 0) s.muted = false;   // turning it up is an explicit "I want to hear it"
		set_local(s);
	};
	auto toggle_fullscreen = [&] { SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); };
	auto open_screen = [&](Screen sc) { ui.screen = sc; ui.sel = 0; ui.capture = false; ui.remap_msg.clear(); };
	auto close_screen = [&] { ui.screen = Screen::None; ui.capture = false; };
	auto remap_back = [&] {
		if (in_menu_now()) close_screen();
		else open_screen(Screen::Esc);
	};
	auto menu_activate = [&](int row) {
		const int n = (int)lib.games().size();
		if (row < n) hub.open_game(lib.games()[(size_t)row].sha1);
		else if (row == n) hub.set_prefer_newest(!hub.settings().prefer_newest);
		else if (row == n + 1) open_screen(Screen::Remap);
		else if (row == n + 2) adb.install();
	};
	auto esc_activate = [&](EscItem it) {
		switch (it) {
		case EscItem::Continue: close_screen(); break;
		case EscItem::Save: hub.quick_save(); close_screen(); break;
		case EscItem::Load: close_screen(); hub.quick_load(); break;
		case EscItem::Volume: toggle_mute(); break;
		case EscItem::Controls: open_screen(Screen::Remap); break;
		case EscItem::Fullscreen: toggle_fullscreen(); break;
		case EscItem::Return: close_screen(); hub.request_return(); break;
		case EscItem::ToMenu: close_screen(); hub.close_game(); break;
		}
	};
	auto remap_apply = [&](int btn, SDL_Scancode k) {
		cable::Settings s = hub.settings();
		if (cable::key_reserved(k)) {
			const char *n = SDL_GetScancodeName(k);
			ui.remap_msg = std::string("Tecla reservada: ") + (n && *n ? n : "?");
			return;   // stay in capture mode
		}
		ui.remap_msg.clear();
		for (int j = 0; j < cable::kNumPcButtons; j++)
			if (j != btn && s.keys[j] == k) {   // key already used: the two buttons swap
				s.keys[j] = s.keys[btn];
				ui.remap_msg = std::string("Trocou com ") + cable::button_label(j);
			}
		s.keys[btn] = k;
		set_local(s);
		ui.capture = false;
	};
	auto remap_activate = [&](int row) {
		if (row < cable::kNumPcButtons) { ui.capture = true; ui.remap_msg.clear(); }
		else if (row == cable::kNumPcButtons) {
			cable::Settings s = hub.settings();
			for (int j = 0; j < cable::kNumPcButtons; j++) s.keys[j] = cable::default_key(j);
			set_local(s);
			ui.remap_msg = "Padrao restaurado";
		} else remap_back();
	};

	while (running) {
		SDL_Event e;
		int pw = 0, ph_ = 0;
		SDL_GetCurrentRenderOutputSize(ren, &pw, &ph_);
		const View mv = menu_view(pw, ph_), gv = game_view(pw, ph_);
		const int n_games = (int)lib.games().size();
		const bool can_install = !no_link && adb.can_install();
		const int menu_rows = menu_row_count(n_games, can_install);
		// Virtual coordinates of the content under a render-pixel point, for the current screen kind.
		auto virt = [&](float x, float y, float &vx, float &vy) {
			const View &v = in_menu_now() ? mv : gv;
			vx = (x - (float)v.ox) / v.s;
			vy = (y - (float)v.oy) / v.s;
		};

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
				if (ui.screen != Screen::None) break;   // pop-up screens are keyboard / mouse only
				if (in_menu_now()) {
					if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) ui.menu_sel = (ui.menu_sel + menu_rows - 1) % menu_rows;
					else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) ui.menu_sel = (ui.menu_sel + 1) % menu_rows;
					else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) menu_activate(std::min(ui.menu_sel, menu_rows - 1));
				} else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && hub.mode() != Hub::Mode::Playing) {
					hub.resume_local();
				}
				break;
			case SDL_EVENT_MOUSE_MOTION:
				SDL_ConvertEventToRenderCoordinates(ren, &e);   // render scale is 1 here (see end_view)
				ui.mx = e.motion.x; ui.my = e.motion.y;
				break;
			case SDL_EVENT_WINDOW_MOUSE_LEAVE: ui.mx = ui.my = -1; break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN: {
				if (e.button.button != SDL_BUTTON_LEFT) break;
				SDL_ConvertEventToRenderCoordinates(ren, &e);
				const float x = e.button.x, y = e.button.y;
				ui.mx = x; ui.my = y;
				if (ui.capture) { ui.capture = false; break; }   // a click cancels the key capture
				float vx, vy;
				virt(x, y, vx, vy);
				if (in_menu_now()) {
					if (ui.screen == Screen::Remap) {
						const float bx = (kMenuW - kBlockW) / 2, by = (kMenuH - kBlockH) / 2;
						for (int i = 0; i < kRemapRows; i++)
							if (inside(vx, vy, bx + 14, by + kRemapRowY0 + (float)i * kRemapRowH - 1, kBlockW - 28, kRemapRowH)) {
								ui.sel = i; remap_activate(i);
								break;
							}
						break;
					}
					if (can_install && hit(kInstallBtn, vx, vy)) { adb.install(); break; }
					for (int i = 0; i < kMenuRows && ui.menu_top + i < n_games; i++)
						if (inside(vx, vy, 6, kRowsY0 + (float)i * kRowH, kMenuW - 12.f, kRowH)) {
							ui.menu_sel = ui.menu_top + i;
							menu_activate(ui.menu_sel);
							break;
						}
					if (inside(vx, vy, 6, kSettingsY - 1, kMenuW - 12.f, 10)) { ui.menu_sel = n_games; menu_activate(n_games); }
					else if (inside(vx, vy, 6, kControlsY - 1, kMenuW - 12.f, 10)) { ui.menu_sel = n_games + 1; menu_activate(n_games + 1); }
					break;
				}
				// ---- in a game: the bar first
				if (y < kBarH) {
					const BarLayout b = bar_layout(pw);
					if (hit(b.mute, x, y)) toggle_mute();
					else if (hit(b.minus, x, y)) change_volume(-10);
					else if (hit(b.plus, x, y)) change_volume(10);
					else if (hit(b.pad, x, y)) open_screen(Screen::Remap);
					else if (hit(b.menu, x, y)) { if (ui.screen == Screen::Esc) close_screen(); else open_screen(Screen::Esc); }
					break;
				}
				const float bx = (gv.vw - kBlockW) / 2, by = (gv.vh - kBlockH) / 2;
				if (ui.screen == Screen::Esc) {
					auto items = esc_items(hub);
					for (size_t i = 0; i < items.size(); i++)
						if (inside(vx, vy, bx + 20, by + kEscRowY0 + (float)i * kEscRowH - 1, kBlockW - 40, kEscRowH)) {
							ui.sel = (int)i; esc_activate(items[i]);
							break;
						}
				} else if (ui.screen == Screen::Remap) {
					for (int i = 0; i < kRemapRows; i++)
						if (inside(vx, vy, bx + 14, by + kRemapRowY0 + (float)i * kRemapRowH - 1, kBlockW - 28, kRemapRowH)) {
							ui.sel = i; remap_activate(i);
							break;
						}
				}
				break;
			}
			case SDL_EVENT_KEY_DOWN: {
				const SDL_Scancode k = e.key.scancode;
				const bool enter = k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER;
				// Repeats: only list navigation (menus) uses them; in a running game they would double-fire F5/F8.
				if (e.key.repeat && !(in_menu_now() || ui.screen != Screen::None)) break;
				if (ui.capture) {   // the next key is the new binding (Esc cancels); nothing else may react to it
					if (e.key.repeat) break;
					if (k == SDL_SCANCODE_ESCAPE) { ui.capture = false; ui.remap_msg.clear(); }
					else remap_apply(ui.sel, k);
					break;
				}
				if (k == SDL_SCANCODE_F11) { toggle_fullscreen(); break; }
				if (!e.key.repeat) {
					if (k == SDL_SCANCODE_M) { toggle_mute(); break; }
					if (k == SDL_SCANCODE_MINUS || k == SDL_SCANCODE_KP_MINUS) { change_volume(-10); break; }
					if (k == SDL_SCANCODE_EQUALS || k == SDL_SCANCODE_KP_PLUS) { change_volume(10); break; }
				}
				if (ui.screen == Screen::Remap) {
					if (k == SDL_SCANCODE_ESCAPE) remap_back();
					else if (k == SDL_SCANCODE_UP) ui.sel = (ui.sel + kRemapRows - 1) % kRemapRows;
					else if (k == SDL_SCANCODE_DOWN) ui.sel = (ui.sel + 1) % kRemapRows;
					else if (enter && !e.key.repeat) remap_activate(ui.sel);
				} else if (ui.screen == Screen::Esc) {
					auto items = esc_items(hub);
					const int cnt = (int)items.size();
					if (ui.sel >= cnt) ui.sel = cnt - 1;
					if (k == SDL_SCANCODE_ESCAPE) close_screen();
					else if (k == SDL_SCANCODE_UP) ui.sel = (ui.sel + cnt - 1) % cnt;
					else if (k == SDL_SCANCODE_DOWN) ui.sel = (ui.sel + 1) % cnt;
					else if ((k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) && items[(size_t)ui.sel] == EscItem::Volume)
						change_volume(k == SDL_SCANCODE_LEFT ? -10 : 10);
					else if (enter && !e.key.repeat) esc_activate(items[(size_t)ui.sel]);
				} else if (in_menu_now()) {
					if (k == SDL_SCANCODE_ESCAPE) running = false;
					else if (k == SDL_SCANCODE_UP) ui.menu_sel = (ui.menu_sel + menu_rows - 1) % menu_rows;
					else if (k == SDL_SCANCODE_DOWN) ui.menu_sel = (ui.menu_sel + 1) % menu_rows;
					else if (k == SDL_SCANCODE_I) adb.install();
					else if (enter) menu_activate(std::min(ui.menu_sel, menu_rows - 1));
				} else if (k == SDL_SCANCODE_ESCAPE) {
					open_screen(Screen::Esc);
				} else if (k == SDL_SCANCODE_F2) {
					hub.request_return();
				} else if (k == SDL_SCANCODE_F5) {
					hub.quick_save();
				} else if (k == SDL_SCANCODE_F8) {
					hub.quick_load();
				} else if (enter && (hub.mode() == Hub::Mode::WaitPhone || hub.mode() == Hub::Mode::AwaitPlace)) {
					hub.resume_local();
				}
				break;
			}
			default: break;
			}
		}
		{   // keep selections valid (the library and the install button come and go)
			const int rows = menu_row_count((int)lib.games().size(), !no_link && adb.can_install());
			if (ui.menu_sel >= rows) ui.menu_sel = rows - 1;
			if (ui.menu_sel < ui.menu_top) ui.menu_top = ui.menu_sel;
			if (ui.menu_sel >= ui.menu_top + kMenuRows) ui.menu_top = ui.menu_sel - kMenuRows + 1;
			if (ui.menu_top < 0 || ui.menu_top > (int)lib.games().size()) ui.menu_top = 0;
		}

		hub.tick();
		if (hub.take_audio_flush() && audio) SDL_ClearAudioStream(audio);

		// The Esc menu belongs to a game: when the game is gone (closed, phone took over and left) it goes too.
		if (in_menu_now() && ui.screen == Screen::Esc) close_screen();
		if (hub.mode() != Hub::Mode::Menu && ui.screen == Screen::Esc && ui.sel >= (int)esc_items(hub).size()) ui.sel = 0;
		// Paused = any pop-up screen over a game. The audio device is paused with it (silence, no stale queue).
		if (audio) {
			const bool want = ui.screen != Screen::None && !in_menu_now();
			if (want != audio_paused) {
				if (want) { SDL_PauseAudioStreamDevice(audio); SDL_ClearAudioStream(audio); }
				else { SDL_ClearAudioStream(audio); SDL_ResumeAudioStreamDevice(audio); }
				audio_paused = want;
			}
		}

		if (unsigned g = link_status.generation(); g != seen_gen || hub.mode() != seen_mode || !seen_mode_valid) {
			seen_gen = g;
			std::string t = link_status.title();
			std::string mt = hub.mode_text();
			if (!mt.empty()) t += " - " + mt;
			if (t != seen_title) { SDL_SetWindowTitle(win, t.c_str()); seen_title = t; }
		}
		if (!seen_mode_valid || hub.mode() != seen_mode) {
			// the keys (and the mouse hover) of the previous screen kind mean nothing now
			seen_mode = hub.mode();
			seen_mode_valid = true;
		}
		if (exit_after_sessions > 0 && (long)server.sessions_ended() >= exit_after_sessions) running = false;

		// Left text of the bar: the link, in few words.
		auto bar_status = [&]() -> std::string {
			if (no_link) return "Cabo desativado";
			if (link_status.port_busy()) return "Porta ocupada";
			if (link_status.connected()) return "Conectado: " + link_status.device();
			if (!link_status.refusal().empty()) return link_status.refusal();
			return link_status.note();
		};

		const bool paused = ui.screen != Screen::None;
		if (hub.mode() == Hub::Mode::Menu || !hub.emulating() || paused) {
			// Not emulating: draw at a relaxed pace (the audio device is not the clock now).
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
			SDL_SetRenderDrawColor(ren, kBgR, kBgG, kBgB, 255);
			SDL_RenderClear(ren);   // the whole target: borders = menu colour
			if (hub.mode() == Hub::Mode::Menu) {
				begin_view(ren, mv);
				if (ui.screen == Screen::Remap) draw_remap(ren, mv, hub.settings(), ui, false);
				else draw_menu(ren, lib, hub, link_status, adb, ui, !no_link);
				end_view(ren);
			} else {
				draw_bar(ren, pw, ui, hub.settings(), bar_status());
				SDL_UpdateTexture(tex, nullptr, gbah_framebuffer(core), GBAH_WIDTH * 4);
				begin_view(ren, gv);
				SDL_FRect dst{g_ox, g_oy, GBAH_WIDTH, GBAH_HEIGHT};
				SDL_RenderTexture(ren, tex, nullptr, &dst);
				draw_game_overlay(ren, hub, link_status);
				if (ui.screen == Screen::Esc) draw_esc(ren, gv, hub, ui);
				else if (ui.screen == Screen::Remap) draw_remap(ren, gv, hub.settings(), ui, true);
				end_view(ren);
			}
			SDL_RenderPresent(ren);
			SDL_Delay(16);
			next_ns = SDL_GetTicksNS();
			continue;
		}

		uint32_t keys = read_keyboard(hub.settings());
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
		SDL_SetRenderDrawColor(ren, kBgR, kBgG, kBgB, 255);
		SDL_RenderClear(ren);
		draw_bar(ren, pw, ui, hub.settings(), bar_status());
		begin_view(ren, gv);
		SDL_FRect dst{g_ox, g_oy, GBAH_WIDTH, GBAH_HEIGHT};
		SDL_RenderTexture(ren, tex, nullptr, &dst);
		if (hub.osd_active()) draw_game_overlay(ren, hub, link_status);
		end_view(ren);
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
