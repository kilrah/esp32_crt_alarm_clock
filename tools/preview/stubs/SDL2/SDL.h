// Minimal SDL stub: lets LovyanGFX's desktop headers compile so sprites can be
// rendered off-screen on a PC without SDL installed. No window is ever opened.
#pragma once
#define SDL_h_
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_mutex;
enum SDL_KeyCode : int { SDLK_UNKNOWN = 0 };
enum SDL_Keymod : int { KMOD_NONE = 0 };
