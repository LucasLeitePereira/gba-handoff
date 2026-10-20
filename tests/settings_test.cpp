// settings.ini text codec: keymap, volume, mute (desktop/library.cpp). Invalid values fall back to defaults.
#include "library.h"

#include <cstdio>
#include <string>

using namespace cable;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static bool is_default_keymap(const Settings &s)
{
	for (int i = 0; i < kNumPcButtons; i++) if (s.keys[i] != default_key(i)) return false;
	return true;
}

int main()
{
	Settings d;
	CHECK(d.volume == 100 && !d.muted && !d.prefer_newest && is_default_keymap(d));
	CHECK(is_default_keymap(settings_from_text("")));                 // missing file = defaults
	CHECK(is_default_keymap(settings_from_text("prefer_newest=1\n"))); // old files (no keymap) keep working
	CHECK(settings_from_text("prefer_newest=1\n").prefer_newest);

	// round trip with a changed map, volume, mute
	Settings s;
	s.prefer_newest = true;
	s.keys[0] = SDL_SCANCODE_K;     // A -> K
	s.keys[1] = SDL_SCANCODE_J;     // B -> J
	s.keys[4] = SDL_SCANCODE_SPACE; // Start -> Space
	s.volume = 40;
	s.muted = true;
	std::string t = settings_to_text(s);
	CHECK(t.find("key_a=K") != std::string::npos);
	CHECK(t.find("volume=40") != std::string::npos);
	Settings r = settings_from_text(t);
	CHECK(r.prefer_newest && r.muted && r.volume == 40);
	for (int i = 0; i < kNumPcButtons; i++) CHECK(r.keys[i] == s.keys[i]);

	// invalid values -> defaults, never garbage
	r = settings_from_text("volume=55\n");  CHECK(r.volume == 100);
	r = settings_from_text("volume=150\n"); CHECK(r.volume == 100);
	r = settings_from_text("volume=-10\n"); CHECK(r.volume == 100);
	r = settings_from_text("volume=abc\n"); CHECK(r.volume == 100);
	r = settings_from_text("volume=\n");    CHECK(r.volume == 100);
	r = settings_from_text("volume=0\n");   CHECK(r.volume == 0);
	r = settings_from_text("muted=maybe\n"); CHECK(!r.muted);
	r = settings_from_text("key_a=NoSuchKey\n"); CHECK(r.keys[0] == SDL_SCANCODE_Z);
	r = settings_from_text("key_a=\n"); CHECK(r.keys[0] == SDL_SCANCODE_Z);
	// reserved shortcuts are refused
	r = settings_from_text("key_a=Escape\nkey_b=F5\nkey_l=F11\nkey_r=M\n");
	CHECK(r.keys[0] == SDL_SCANCODE_Z && r.keys[1] == SDL_SCANCODE_X && r.keys[2] == SDL_SCANCODE_A && r.keys[3] == SDL_SCANCODE_S);
	CHECK(key_reserved(SDL_SCANCODE_ESCAPE) && key_reserved(SDL_SCANCODE_F2) && key_reserved(SDL_SCANCODE_F5) &&
	      key_reserved(SDL_SCANCODE_F8) && key_reserved(SDL_SCANCODE_F11) && key_reserved(SDL_SCANCODE_UNKNOWN));
	CHECK(!key_reserved(SDL_SCANCODE_Z) && !key_reserved(SDL_SCANCODE_RETURN));
	// the same key on two buttons (hand-edited file): whole map back to defaults
	r = settings_from_text("key_a=K\nkey_b=K\n");
	CHECK(is_default_keymap(r));
	// CRLF and unknown keys
	r = settings_from_text("key_a=K\r\nfoo=bar\r\nvolume=20\r\n");
	CHECK(r.keys[0] == SDL_SCANCODE_K && r.volume == 20);

	if (fails) { printf("%d failure(s)\n", fails); return 1; }
	printf("settings_test: ok\n");
	return 0;
}
