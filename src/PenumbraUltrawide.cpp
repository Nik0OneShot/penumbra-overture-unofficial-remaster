// PenumbraUltrawide - widescreen/ultrawide fix for Penumbra: Overture (Steam build 459398).
//
// Ships as a stand-in SDL_image.dll that forwards every export to the real library
// (renamed SDL_image_orig.dll). At the game's first SDL_Init call, after the Steam stub has
// decrypted the executable, it patches the game in memory:
//
//   * Menus, HUD, text and the inventory keep their 4:3 shape, centred (pillarboxed),
//     instead of being stretched across the screen.
//   * Full-screen overlays (fades, flashes, damage, noise, letterbox bars) still fill
//     the whole screen.
//   * The in-world cursor stays on the object it points at.
//   * Horizontal mouse speed matches vertical again (it was ~1.8x slower at 21:9).
//   * The options menu offers modern resolutions, including the desktop resolution.
//   * Game logic runs at the monitor's refresh rate (HighFramerate.cpp).
//
// Every patch site is checked against the expected original bytes first; if anything
// differs (another game version), nothing is patched and the reason is logged.

#include "Patch.h"
#include <intrin.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdarg.h>

#pragma intrinsic(_ReturnAddress)

//////////////////////////////////////////////////////////////////////////
// Game addresses (Penumbra.exe, image base 0x400000, no ASLR)

namespace A {
	// cLowLevelGraphicsSDL: mvScreenSize (int x,y) at +4, mvVirtualSize (float x,y) at +0xC
	const DWORD SetOrtho        = 0x0052FFE0;	// vtable index 31
	const DWORD SetOrthoSlot    = 0x00650A3C;
	const DWORD RetDrawAll      = 0x004CD534;	// cGraphicsDrawer::DrawAll
	const DWORD RetDrawTexture  = 0x0043E64C;	// cGraphicsHelper::DrawTexture
	const DWORD RetIntroDraw    = 0x0044CB86;	// cIntroStory::OnDraw

	// cMouseSDL::Update: mvMouseRelPos at +0x2C, mpLowLevelGraphics at +0x64
	const DWORD MouseUpdate     = 0x0056E270;
	const DWORD MouseUpdateSlot = 0x0065B910;

	// cGraphicsDrawer (mpLowLevelGraphics at +0)
	const DWORD DrawLong        = 0x004CE1A0;	// DrawGfxObject(obj, pos, size, col, flipH, flipV, angle)
	const DWORD DrawShort       = 0x004CE270;	// DrawGfxObject(obj, pos)
	const DWORD DrawTexture     = 0x0043E600;	// cGraphicsHelper::DrawTexture(tex, pos, size, col)

	// Call sites
	const DWORD CursorShortCall    = 0x004843D2;	// cPlayer crosshair (esi = cPlayer)
	const DWORD CrossHairStateRead = 0x0048413F;	// mov eax, [esi+2F0h] in the same function
	const DWORD CrossHairDrawerLoad = 0x004843CC;	// mov ecx, [esi+84h] right before the call
	const DWORD PlayerCrossHairState = 0x2F0;	// cPlayer::mCrossHairState
	const int   CrossHairPointer   = 5;	// eCrossHairState_Pointer: menus reuse the crosshair as their mouse pointer
	const DWORD CursorLongCalls[]  = { 0x0048421A, 0x00484295, 0x004842D4 };	// held-item cursor
	const DWORD NoiseTileCall      = 0x0048C6C1;	// cPlayerNoiseFilter::Draw
	const DWORD LetterboxCalls[]   = { 0x0044CC02, 0x0044CC62 };	// intro black bars

	// fmul dword ptr [800.0f] feeding AddCrossHairPos(afVal * 800, 0)
	const DWORD CursorMul800[]  = { 0x0048E388, 0x00490D88, 0x00493A07, 0x00494C68 };
	const DWORD Const800        = 0x0063DF04;

	// gvResolutions[17] (cVector2l) used by the options menu
	const DWORD Resolutions     = 0x00692CC0;
	const int   ResolutionNum   = 17;
}

//////////////////////////////////////////////////////////////////////////
// Logging and config

char g_dir[MAX_PATH];
static FILE* g_log;

void Log(const char* fmt, ...)
{
	if(!g_log) return;
	va_list va; va_start(va, fmt);
	vfprintf(g_log, fmt, va);
	va_end(va);
	fputc('\n', g_log);
	fflush(g_log);
}

int CfgInt(const char* key, int def)
{
	char path[MAX_PATH];
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%sPenumbraUltrawide.ini", g_dir);
	return GetPrivateProfileIntA("Fix", key, def, path);
}

//////////////////////////////////////////////////////////////////////////
// Aspect helpers

// k: how much wider the screen is than 4:3 (2.389/1.333 = 1.79 at 21:9).
// off: virtual units added on each side when the 800x600 layout is centred.
struct Fit { float k, off; };

static bool GetFit(const u8* gfx, Fit& f)
{
	if(!gfx) return false;
	int W = *(const int*)(gfx + 4), H = *(const int*)(gfx + 8);
	float vx = *(const float*)(gfx + 0xC), vy = *(const float*)(gfx + 0x10);
	if(W <= 0 || H <= 0 || vx <= 0 || vy <= 0) return false;
	float vw = vy * (float)W / (float)H;
	if(vw <= vx + 0.5f) return false;	// 4:3 or narrower: leave as is
	f.k = vw / vx;
	f.off = (vw - vx) * 0.5f;
	return true;
}

static float VirtualWidth(const u8* gfx) { return *(const float*)(gfx + 0xC); }

//////////////////////////////////////////////////////////////////////////
// Hooks

typedef void (__fastcall *SetOrtho_t)(void*, void*, const float*, float, float);
typedef void (__fastcall *MouseUpdate_t)(void*, void*);
typedef void (__fastcall *DrawLong_t)(void*, void*, void*, const float*, const float*, const float*, int, int, float);
typedef void (__fastcall *DrawShort_t)(void*, void*, void*, const float*);
typedef void (__fastcall *DrawTexture_t)(void*, void*, void*, const float*, const float*, const float*);

static DrawLong_t g_drawLong = (DrawLong_t)A::DrawLong;	// becomes the trampoline once detoured
static float g_cursorMulX = 800.0f;	// replaces the 800.0f in AddCrossHairPos(afVal * 800, 0)

// 2D projection: centre the 800x600 layout for the UI passes, leave post effects alone.
static void __fastcall Hook_SetOrtho(void* gfx, void* edx, const float* size, float afMin, float afMax)
{
	DWORD ret = (DWORD)_ReturnAddress();
	Fit f;
	if((ret == A::RetDrawAll || ret == A::RetDrawTexture || ret == A::RetIntroDraw) && GetFit((u8*)gfx, f))
	{
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-f.off, size[0] + f.off, size[1], 0, afMin, afMax);
		return;
	}
	((SetOrtho_t)A::SetOrtho)(gfx, edx, size, afMin, afMax);
}

// Relative mouse motion is scaled by virtual/screen size per axis, so at 21:9 horizontal
// movement came out ~1.8x slower than vertical. Make it match the 4:3 behaviour.
static void __fastcall Hook_MouseUpdate(void* mouse, void* edx)
{
	((MouseUpdate_t)A::MouseUpdate)(mouse, edx);

	u8* m = (u8*)mouse;
	Fit f;
	if(GetFit(*(u8**)(m + 0x64), f))
	{
		*(float*)(m + 0x2C) *= f.k;
		// The in-world cursor lives in stretched screen space, so undo the boost there.
		g_cursorMulX = 800.0f / f.k;
	}
	else
	{
		g_cursorMulX = 800.0f;
	}
}

// Anything covering the full layout width is an overlay: stretch it to the real screen edges.
static void __fastcall Hook_DrawLong(void* drawer, void* edx, void* obj, const float* pos, const float* size,
									 const float* col, int flipH, int flipV, float angle)
{
	const u8* gfx = *(u8**)drawer;
	Fit f;
	if(GetFit(gfx, f) && pos[0] <= 0.5f && pos[0] + size[0] >= VirtualWidth(gfx) - 0.5f)
	{
		float p[3] = { pos[0] * f.k - f.off, pos[1], pos[2] };
		float s[2] = { size[0] * f.k, size[1] };
		g_drawLong(drawer, edx, obj, p, s, col, flipH, flipV, angle);
		return;
	}
	g_drawLong(drawer, edx, obj, pos, size, col, flipH, flipV, angle);
}

// Tiles that make up a full-screen effect (sanity noise): stretch each tile.
static void __fastcall Call_DrawLongStretch(void* drawer, void* edx, void* obj, const float* pos, const float* size,
											const float* col, int flipH, int flipV, float angle)
{
	Fit f;
	if(GetFit(*(u8**)drawer, f))
	{
		float p[3] = { pos[0] * f.k - f.off, pos[1], pos[2] };
		float s[2] = { size[0] * f.k, size[1] };
		g_drawLong(drawer, edx, obj, p, s, col, flipH, flipV, angle);
		return;
	}
	g_drawLong(drawer, edx, obj, pos, size, col, flipH, flipV, angle);
}

// The in-world cursor position is in stretched screen space (it must match 3D picking),
// so move its centre into the centred layout space without changing its size.
static inline float CursorX(float x, float half, const Fit& f) { return (x + half) * f.k - f.off - half; }

static void __fastcall Call_DrawLongCursor(void* drawer, void* edx, void* obj, const float* pos, const float* size,
										   const float* col, int flipH, int flipV, float angle)
{
	Fit f;
	if(GetFit(*(u8**)drawer, f))
	{
		float p[3] = { CursorX(pos[0], size[0] * 0.5f, f), pos[1], pos[2] };
		g_drawLong(drawer, edx, obj, p, size, col, flipH, flipV, angle);
		return;
	}
	g_drawLong(drawer, edx, obj, pos, size, col, flipH, flipV, angle);
}

static float ImageHalfWidth(void* obj)
{
	__try
	{
		// cGfxObject::mpMat -> image[diffuse] -> size.x (same path the game uses)
		u8* mat = *(u8**)((u8*)obj + 0x10);
		u8* img = **(u8***)(mat + 0xB0);
		return (float)*(int*)(img + 0x6C) * 0.5f;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

// Inventory, notebook, keypad and death menu set the crosshair to their own mouse position,
// which is already in layout space, and show it as the pointer icon. No level uses that icon
// in the world, so it tells the two cases apart.
static u8* g_player;	// cPlayer making the crosshair draw call

static void __fastcall Call_DrawShortCursor(void* drawer, void* edx, void* obj, const float* pos)
{
	bool menuPointer = g_player && *(int*)(g_player + A::PlayerCrossHairState) == A::CrossHairPointer;
	Fit f;
	if(!menuPointer && GetFit(*(u8**)drawer, f))
	{
		float p[3] = { CursorX(pos[0], ImageHalfWidth(obj), f), pos[1], pos[2] };
		((DrawShort_t)A::DrawShort)(drawer, edx, obj, p);
		return;
	}
	((DrawShort_t)A::DrawShort)(drawer, edx, obj, pos);
}

// The crosshair call site holds the cPlayer in esi; note it, then continue as the draw call.
__declspec(naked) static void Thunk_DrawShortCursor()
{
	__asm
	{
		mov g_player, esi
		jmp Call_DrawShortCursor
	}
}

// Intro letterbox bars: full width like the in-game ones.
static void __fastcall Call_DrawTextureStretch(void* helper, void* edx, void* tex, const float* pos,
											   const float* size, const float* col)
{
	Fit f;
	if(GetFit(*(u8**)((u8*)helper + 0x14), f))
	{
		float p[3] = { pos[0] * f.k - f.off, pos[1], pos[2] };
		float s[3] = { size[0] * f.k, size[1], size[2] };
		((DrawTexture_t)A::DrawTexture)(helper, edx, tex, p, s, col);
		return;
	}
	((DrawTexture_t)A::DrawTexture)(helper, edx, tex, pos, size, col);
}

//////////////////////////////////////////////////////////////////////////
// Patching

static bool SameBytes(DWORD addr, const void* bytes, size_t n)
{
	__try { return memcmp((void*)addr, bytes, n) == 0; }
	__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Expect(DWORD addr, const void* bytes, size_t n, const char* what)
{
	if(SameBytes(addr, bytes, n)) return true;
	Log("  mismatch at %08X (%s)", addr, what);
	return false;
}

bool ExpectDword(DWORD addr, DWORD val, const char* what) { return Expect(addr, &val, 4, what); }

bool ExpectCall(DWORD site, DWORD target, const char* what)
{
	u8 b[5]; b[0] = 0xE8;
	*(DWORD*)(b + 1) = target - (site + 5);
	return Expect(site, b, 5, what);
}

void Write(DWORD addr, const void* bytes, size_t n)
{
	DWORD old;
	VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)addr, bytes, n);
	VirtualProtect((void*)addr, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
}

void WriteDword(DWORD addr, DWORD val) { Write(addr, &val, 4); }

void RedirectCall(DWORD site, const void* fn)
{
	DWORD rel = (DWORD)fn - (site + 5);
	Write(site + 1, &rel, 4);
}

// Overwrite the first n bytes of a function with a jump to fn; return a trampoline that
// runs those bytes and continues in the original.
static void* Detour(DWORD addr, const void* fn, size_t n)
{
	u8* tramp = (u8*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if(!tramp) return NULL;
	memcpy(tramp, (void*)addr, n);
	tramp[n] = 0xE9;
	*(DWORD*)(tramp + n + 1) = (addr + n) - ((DWORD)tramp + n + 5);

	u8 jmp[16];
	memset(jmp, 0x90, sizeof(jmp));
	jmp[0] = 0xE9;
	*(DWORD*)(jmp + 1) = (DWORD)fn - (addr + 5);
	Write(addr, jmp, n);
	return tramp;
}

static bool VerifyGame()
{
	static const u8 drawLongHead[] = { 0x83, 0xEC, 0x3C, 0x56, 0x8B, 0x74, 0x24, 0x44 };
	static const u8 orthoCallEdi[] = { 0xFF, 0x57, 0x7C };
	static const u8 orthoCallEbp[] = { 0xFF, 0x55, 0x7C };
	static const int firstRes[] = { 640, 480, 800, 600, 1024, 768 };
	const float f800 = 800.0f;

	bool ok = true;
	ok &= ExpectDword(A::SetOrthoSlot, A::SetOrtho, "SetOrthoProjection vtable");
	ok &= ExpectDword(A::MouseUpdateSlot, A::MouseUpdate, "cMouseSDL::Update vtable");
	ok &= Expect(A::RetDrawTexture - 3, orthoCallEdi, 3, "DrawTexture ortho call");
	ok &= Expect(A::RetIntroDraw - 3, orthoCallEdi, 3, "IntroStory ortho call");
	ok &= Expect(A::RetDrawAll - 3, orthoCallEbp, 3, "DrawAll ortho call");
	ok &= Expect(A::DrawLong, drawLongHead, sizeof(drawLongHead), "DrawGfxObject head");
	ok &= ExpectCall(A::CursorShortCall, A::DrawShort, "crosshair draw");
	static const u8 stateRead[] = { 0x8B, 0x86, 0xF0, 0x02, 0x00, 0x00 };
	static const u8 drawerLoad[] = { 0x8B, 0x8E, 0x84, 0x00, 0x00, 0x00 };
	ok &= Expect(A::CrossHairStateRead, stateRead, sizeof(stateRead), "crosshair state read via esi");
	ok &= Expect(A::CrossHairDrawerLoad, drawerLoad, sizeof(drawerLoad), "crosshair drawer load via esi");
	for(DWORD c : A::CursorLongCalls) ok &= ExpectCall(c, A::DrawLong, "item cursor draw");
	ok &= ExpectCall(A::NoiseTileCall, A::DrawLong, "noise tile draw");
	for(DWORD c : A::LetterboxCalls) ok &= ExpectCall(c, A::DrawTexture, "intro letterbox draw");
	for(DWORD c : A::CursorMul800)
	{
		const u8 fmul[] = { 0xD8, 0x0D };
		ok &= Expect(c, fmul, 2, "cursor fmul") && ExpectDword(c + 2, A::Const800, "cursor fmul operand");
	}
	ok &= Expect(A::Const800, &f800, 4, "800.0f constant");
	ok &= Expect(A::Resolutions, firstRes, sizeof(firstRes), "resolution table");
	return ok;
}

static void PatchResolutions()
{
	static const int common[][2] = {
		{1024,768}, {1280,720}, {1280,1024}, {1366,768}, {1440,900}, {1600,900}, {1680,1050},
		{1920,1080}, {1920,1200}, {2560,1080}, {2560,1440}, {2560,1600}, {3440,1440},
		{3840,1600}, {3840,2160}, {5120,1440}, {5120,2160},
	};
	const int N = A::ResolutionNum;
	int dw = GetSystemMetrics(SM_CXSCREEN), dh = GetSystemMetrics(SM_CYSCREEN);

	int list[N + 1][2];
	int n = 0;
	bool haveDesktop = false;
	for(auto& r : common)
	{
		if(r[0] == dw && r[1] == dh) haveDesktop = true;
		list[n][0] = r[0]; list[n][1] = r[1]; ++n;
	}
	if(!haveDesktop && dw > 0 && dh > 0)
	{
		// Keep the list at N entries: drop the largest entry that the desktop can't show.
		int drop = n - 1;
		for(int i = n - 1; i >= 0; --i)
			if(list[i][0] > dw || list[i][1] > dh) { drop = i; break; }
		for(int i = drop; i < n - 1; ++i) { list[i][0] = list[i + 1][0]; list[i][1] = list[i + 1][1]; }
		--n;
		int at = n;
		while(at > 0 && (list[at - 1][0] > dw || (list[at - 1][0] == dw && list[at - 1][1] > dh))) --at;
		for(int i = n; i > at; --i) { list[i][0] = list[i - 1][0]; list[i][1] = list[i - 1][1]; }
		list[at][0] = dw; list[at][1] = dh; ++n;
	}
	Write(A::Resolutions, list, sizeof(int) * 2 * N);
}

static void ApplyPatches()
{
	if(!VerifyGame())
	{
		Log("Unexpected game code: this fix targets the Steam build (459398). Nothing was patched.");
		return;
	}

	if(CfgInt("Pillarbox2D", 1))
	{
		WriteDword(A::SetOrthoSlot, (DWORD)&Hook_SetOrtho);
		void* tramp = Detour(A::DrawLong, &Hook_DrawLong, 8);
		if(tramp) g_drawLong = (DrawLong_t)tramp;
		RedirectCall(A::CursorShortCall, &Thunk_DrawShortCursor);
		for(DWORD c : A::CursorLongCalls) RedirectCall(c, &Call_DrawLongCursor);
		RedirectCall(A::NoiseTileCall, &Call_DrawLongStretch);
		for(DWORD c : A::LetterboxCalls) RedirectCall(c, &Call_DrawTextureStretch);
		Log("  2D layout pillarboxed, overlays full width%s", tramp ? "" : " (overlay detour FAILED)");
	}
	if(CfgInt("FixMouse", 1))
	{
		WriteDword(A::MouseUpdateSlot, (DWORD)&Hook_MouseUpdate);
		for(DWORD c : A::CursorMul800) WriteDword(c + 2, (DWORD)&g_cursorMulX);
		Log("  mouse speed fixed");
	}
	if(CfgInt("MoreResolutions", 1))
	{
		PatchResolutions();
		Log("  options menu resolutions updated");
	}
	int rate = ChooseLogicRate();
	if(rate != 60)
	{
		if(VerifyHighFramerate()) ApplyHighFramerate(rate);
		else Log("  high framerate skipped: unexpected code, game logic stays at 60 Hz");
	}
	Log("Done.");
}

//////////////////////////////////////////////////////////////////////////
// SDL_image proxy: everything but IMG_Load is forwarded to the real DLL.

#define FORWARD(name, ord) __pragma(comment(linker, "/EXPORT:" #name "=SDL_image_orig." #name ",@" #ord))
FORWARD(IMG_InvertAlpha, 1)
FORWARD(IMG_Linked_Version, 2)
FORWARD(IMG_LoadBMP_RW, 4)
FORWARD(IMG_LoadGIF_RW, 5)
FORWARD(IMG_LoadJPG_RW, 6)
FORWARD(IMG_LoadLBM_RW, 7)
FORWARD(IMG_LoadPCX_RW, 8)
FORWARD(IMG_LoadPNG_RW, 9)
FORWARD(IMG_LoadPNM_RW, 10)
FORWARD(IMG_LoadTGA_RW, 11)
FORWARD(IMG_LoadTIF_RW, 12)
FORWARD(IMG_LoadTyped_RW, 13)
FORWARD(IMG_LoadXCF_RW, 14)
FORWARD(IMG_LoadXPM_RW, 15)
FORWARD(IMG_LoadXV_RW, 16)
FORWARD(IMG_Load_RW, 17)
FORWARD(IMG_ReadXPMFromArray, 18)
FORWARD(IMG_isBMP, 19)
FORWARD(IMG_isGIF, 20)
FORWARD(IMG_isJPG, 21)
FORWARD(IMG_isLBM, 22)
FORWARD(IMG_isPCX, 23)
FORWARD(IMG_isPNG, 24)
FORWARD(IMG_isPNM, 25)
FORWARD(IMG_isTIF, 26)
FORWARD(IMG_isXCF, 27)
FORWARD(IMG_isXPM, 28)
FORWARD(IMG_isXV, 29)

// Patch once, as early as possible: some fixes change values the game stores while building
// its objects. The game's first SDL_Init call (engine start-up, before any game object
// exists) is the preferred moment; the first IMG_Load is the fallback.
static void EnsurePatched(const char* when)
{
	static bool done;
	if(done) return;
	done = true;
	Log("Patching at %s", when);
	if(CfgInt("Enabled", 1)) ApplyPatches();
	else Log("Disabled in PenumbraUltrawide.ini");
}

const DWORD IatSDLInit = 0x0063D6F0;	// Penumbra.exe import slot for SDL_Init
typedef int (__cdecl *SDL_Init_t)(unsigned);
static SDL_Init_t g_sdlInit;

static int __cdecl Hook_SDLInit(unsigned flags)
{
	EnsurePatched("engine start");
	return g_sdlInit(flags);
}

typedef void* (__cdecl *IMG_Load_t)(const char*);
static IMG_Load_t g_imgLoad;

extern "C" void* __cdecl IMG_Load(const char* file)
{
	if(!g_imgLoad)
	{
		char path[MAX_PATH];
		_snprintf_s(path, sizeof(path), _TRUNCATE, "%sSDL_image_orig.dll", g_dir);
		HMODULE orig = LoadLibraryA(path);
		if(orig) g_imgLoad = (IMG_Load_t)GetProcAddress(orig, "IMG_Load");
		if(!g_imgLoad)
		{
			Log("Could not load IMG_Load from %s", path);
			return NULL;
		}
	}
	EnsurePatched("first image load");
	return g_imgLoad(file);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(inst);
		GetModuleFileNameA(inst, g_dir, MAX_PATH);
		char* slash = strrchr(g_dir, '\\');
		if(slash) slash[1] = 0;

		char path[MAX_PATH];
		_snprintf_s(path, sizeof(path), _TRUNCATE, "%sPenumbraUltrawide.log", g_dir);
		fopen_s(&g_log, path, "w");

		// The game's code is still encrypted here, but its import table is already filled in.
		HMODULE sdl = GetModuleHandleA("SDL.dll");
		g_sdlInit = sdl ? (SDL_Init_t)GetProcAddress(sdl, "SDL_Init") : NULL;
		if(g_sdlInit && SameBytes(IatSDLInit, &g_sdlInit, 4)) WriteDword(IatSDLInit, (DWORD)&Hook_SDLInit);
	}
	return TRUE;
}
