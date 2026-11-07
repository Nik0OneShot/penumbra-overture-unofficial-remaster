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
#include <math.h>
#include <share.h>
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

	// cFrustum::SetViewProjMatrix: mOriginBV.SetSize(near*2), the box around the camera
	const DWORD OriginBoxSize   = 0x005045CD;	// fld [esp+78h] (near); mov edx, [esp+74h]; fadd st0, st0
	// cRenderer3D::UpdateRenderList: fog-culling box = fogEnd*2 / cos(fov*aspect*0.5)
	const DWORD FogCornerCos    = 0x004F6D83;	// fmul [0.5]; fcos   (edi = cCamera3D: mfFOV +10h, mfAspect +14h)

	// cSectorVisibilityContainer::SearchSector: is this doorway (portal) in view?
	const DWORD PortalTestCall  = 0x00523E60;	// call IntersectionBV(&portal->mBV, parentSet); ebp = cPortal
	const DWORD IntersectionBV  = 0x00523310;
	const DWORD IntersectionFrustumTest = 0x00523395;	// add ecx, 26Ch; call cFrustum::CollideBoundingVolume
	const DWORD FrustumCollideBV = 0x00504170;
	const DWORD ContainerFrustum = 0x26C;	// cSectorVisibilityContainer::mFrustum
	const DWORD FrustumPlanes    = 0x2D0;	// cPlanef[6] (a, b, c, d, normal): left, right, bottom, top, near, far
	const DWORD FrustumSphereRadius = 0x378;
	const DWORD PortalBV         = 0x90;	// cPortal::mBV; cPortal::mlId at +4

	// OALWrapper cOAL_EffectSlot constructor (environmental audio): creates its mutex only if
	// the EFX manager's "threaded" flag (+14h) is set, but the manager sets that flag after
	// creating the slots, so it is uninitialised here.
	const DWORD EffectSlotMutexCheck = 0x0057F91C;	// mov cl, [eax+14h]; test cl, cl; je +8; call SDL_CreateMutex
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

//////////////////////////////////////////////////////////////////////////
// Crash reporter: for a fatal fault in a module from the game folder, log where it happened,
// the registers, and the return addresses on the stack that point into game modules.

static volatile int g_probing;	// >0 while our own __try code reads memory that may not exist
static int g_crashReports;

static bool GameFolderModule(DWORD addr, char* name, size_t n, DWORD* base)
{
	HMODULE mod;
	if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCSTR)addr, &mod)) return false;
	char path[MAX_PATH];
	if(!GetModuleFileNameA(mod, path, MAX_PATH)) return false;
	if(_strnicmp(path, g_dir, strlen(g_dir)) != 0) return false;
	const char* file = strrchr(path, '\\');
	strncpy_s(name, n, file ? file + 1 : path, _TRUNCATE);
	*base = (DWORD)mod;
	return true;
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep)
{
	const EXCEPTION_RECORD* er = ep->ExceptionRecord;
	DWORD code = er->ExceptionCode;
	bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
				 code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
				 code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
	if(!fatal || g_probing > 0 || g_crashReports >= 3) return EXCEPTION_CONTINUE_SEARCH;

	char mod[64]; DWORD base;
	DWORD eip = (DWORD)er->ExceptionAddress;
	if(!GameFolderModule(eip, mod, sizeof(mod), &base)) return EXCEPTION_CONTINUE_SEARCH;
	++g_crashReports;

	const CONTEXT* c = ep->ContextRecord;
	Log("CRASH: exception %08X at %s+%X (%08X)", code, mod, eip - base, eip);
	if(code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
	{
		ULONG_PTR kind = er->ExceptionInformation[0];
		Log("  %s address %08X", kind == 8 ? "executing" : kind == 1 ? "writing" : "reading", (DWORD)er->ExceptionInformation[1]);
	}
	Log("  eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
	Log("  ebp %08X esp %08X eip %08X eflags %08X", c->Ebp, c->Esp, c->Eip, c->EFlags);

	const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
	const DWORD* sp = (const DWORD*)c->Esp;
	int shown = 0;
	for(int i = 0; i < 512 && shown < 24 && (const void*)(sp + i + 1) <= tib->StackBase && (const void*)sp >= tib->StackLimit; ++i)
	{
		char m2[64]; DWORD b2;
		if(sp[i] > 0x10000 && GameFolderModule(sp[i], m2, sizeof(m2), &b2))
		{
			Log("  [esp+%03X] %s+%X", i * 4, m2, sp[i] - b2);
			++shown;
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
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
	float half = 0;
	++g_probing;
	__try
	{
		// cGfxObject::mpMat -> image[diffuse] -> size.x (same path the game uses)
		u8* mat = *(u8**)((u8*)obj + 0x10);
		u8* img = **(u8***)(mat + 0xB0);
		half = (float)*(int*)(img + 0x6C) * 0.5f;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	}
	--g_probing;
	return half;
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

// The engine keeps a box around the camera, sized near*2, and uses it to pick Z-fail stencil
// shadows when the near plane might be inside a shadow volume. That box only reaches the near
// plane's corners up to about 4:3. Wider, a side corner can sit in a wall's shadow unnoticed,
// and that strip of the screen renders black. Size the box to contain the whole near plane.
static float __cdecl NearPlaneBoxSize(float nearPlane, float fov, float aspect)
{
	float t = tanf(fov * 0.5f);
	return 2.0f * nearPlane * sqrtf(1.0f + t * t * (1.0f + aspect * aspect));
}

// Replaces "fld [esp+78h]; mov edx, [esp+74h]; fadd st0, st0" (10 bytes).
__declspec(naked) static void Stub_OriginBoxSize()
{
	__asm
	{
		push eax
		push ecx
		push dword ptr [esp+8Ch]	// aspect: the game's [esp+80h]
		push dword ptr [esp+8Ch]	// fov:    the game's [esp+7Ch]
		push dword ptr [esp+8Ch]	// near:   the game's [esp+78h]
		call NearPlaneBoxSize
		add esp, 0Ch
		pop ecx
		pop eax
		mov edx, [esp+78h]			// the replaced "mov edx, [esp+74h]"
		ret
	}
}

// Fog culling keeps what lies within fogEnd*2/cos(fov*aspect/2) of the camera. From about
// 2.6:1 the cosine goes negative and everything is culled. Use the vanilla value where it is
// valid and at least as generous as the real view corner; otherwise use the real corner.
static float __cdecl FogCornerCos(float fov, float aspect)
{
	float t = tanf(fov * 0.5f);
	float exact = 1.0f / sqrtf(1.0f + t * t * (1.0f + aspect * aspect));
	float vanilla = cosf(fov * aspect * 0.5f);
	return (vanilla > 0 && vanilla < exact) ? vanilla : exact;
}

// Replaces "fmul [0.5]; fcos" (8 bytes). On entry st0 = fov*aspect, st1 = fogEnd*2.
static float g_fogEnd2;
__declspec(naked) static void Stub_FogCornerCos()
{
	__asm
	{
		fstp st(0)
		fstp g_fogEnd2				// keep the x87 stack empty across the C call
		push eax
		push ecx
		push edx
		push dword ptr [edi+14h]	// mfAspect
		push dword ptr [edi+10h]	// mfFOV
		call FogCornerCos
		add esp, 8
		pop edx
		pop ecx
		pop eax
		fld g_fogEnd2
		fxch st(1)					// st0 = result, st1 = fogEnd*2, as after fcos
		ret
	}
}

// A room is only drawn if a doorway (portal quad) leading to it is in view. Some levels'
// room meshes reach a little past their doorway quad, so near a doorway you can see into the
// next room while the quad itself is just outside the view. At 4:3 that never happens on
// screen; at 21:9 the room shows up black at the edge. Count doorways within a margin outside
// the view as visible; everything inside the room is still culled with the exact view.
static float g_portalMargin;	// metres; 0 = off
static int g_portalRescueLogs;

typedef bool (__fastcall *IntersectionBV_t)(void*, void*, void*, void*);

static bool __fastcall Call_PortalTest(void* container, void* edx, void* bv, void* parentSet)
{
	IntersectionBV_t test = (IntersectionBV_t)A::IntersectionBV;
	bool exact = test(container, edx, bv, parentSet);
	if(exact || g_portalMargin <= 0) return exact;

	u8* frustum = (u8*)container + A::ContainerFrustum;
	float saved[6];
	for(int i = 0; i < 5; ++i)	// left, right, bottom, top, near
	{
		float* d = (float*)(frustum + A::FrustumPlanes + i * 0x1C + 0xC);
		saved[i] = *d;
		*d += g_portalMargin;
	}
	float* radius = (float*)(frustum + A::FrustumSphereRadius);
	saved[5] = *radius;
	*radius += g_portalMargin;

	bool visible = test(container, edx, bv, parentSet);

	for(int i = 0; i < 5; ++i) *(float*)(frustum + A::FrustumPlanes + i * 0x1C + 0xC) = saved[i];
	*radius = saved[5];

	if(visible && g_portalRescueLogs < 40)
	{
		++g_portalRescueLogs;
		const float* eye = (const float*)(frustum + 0x14);
		Log("  doorway %d drawn thanks to the margin (camera %.1f %.1f %.1f)",
			*(int*)((u8*)bv - A::PortalBV + 4), eye[0], eye[1], eye[2]);
	}
	return visible;
}

//////////////////////////////////////////////////////////////////////////
// Patching

static bool SameBytes(DWORD addr, const void* bytes, size_t n)
{
	bool same = false;
	++g_probing;
	__try { same = memcmp((void*)addr, bytes, n) == 0; }
	__except(EXCEPTION_EXECUTE_HANDLER) {}
	--g_probing;
	return same;
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
	static const u8 originBox[] = { 0xD9, 0x44, 0x24, 0x78, 0x8B, 0x54, 0x24, 0x74, 0xDC, 0xC0 };
	ok &= Expect(A::OriginBoxSize, originBox, sizeof(originBox), "frustum origin box");
	static const u8 fogCos[] = { 0xD8, 0x0D, 0x14, 0xDE, 0x63, 0x00, 0xD9, 0xFF };
	ok &= Expect(A::FogCornerCos, fogCos, sizeof(fogCos), "fog culling cosine");
	static const u8 portalBV[] = { 0x8D, 0x95, 0x90, 0x00, 0x00, 0x00, 0x52, 0x8B, 0xCE };	// lea edx,[ebp+90h]; push edx; mov ecx,esi
	ok &= Expect(A::PortalTestCall - 9, portalBV, sizeof(portalBV), "portal test arguments")
		&& ExpectCall(A::PortalTestCall, A::IntersectionBV, "portal test call");
	static const u8 frustumAdd[] = { 0x81, 0xC1, 0x6C, 0x02, 0x00, 0x00 };	// add ecx, 26Ch
	ok &= Expect(A::IntersectionFrustumTest, frustumAdd, sizeof(frustumAdd), "visibility frustum")
		&& ExpectCall(A::IntersectionFrustumTest + 6, A::FrustumCollideBV, "visibility frustum test");
	static const u8 slotMutex[] = { 0x8A, 0x48, 0x14, 0x84, 0xC9, 0x74, 0x08, 0xE8 };
	ok &= Expect(A::EffectSlotMutexCheck, slotMutex, sizeof(slotMutex), "effect slot mutex check");
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

	u8 call[10] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
	*(DWORD*)(call + 1) = (DWORD)&Stub_OriginBoxSize - (A::OriginBoxSize + 5);
	Write(A::OriginBoxSize, call, 10);
	*(DWORD*)(call + 1) = (DWORD)&Stub_FogCornerCos - (A::FogCornerCos + 5);
	Write(A::FogCornerCos, call, 8);
	g_portalMargin = (float)CfgInt("DoorwayMargin", 2);
	if(g_portalMargin > 0) RedirectCall(A::PortalTestCall, &Call_PortalTest);
	Log("  wide-view shadow and fog culling fixed; doorway margin %.0f m", g_portalMargin);

	// Environmental audio: always create the effect slot's mutex (the slot updater thread
	// locks it), instead of trusting a flag that isn't set yet.
	static const u8 nop2[] = { 0x90, 0x90 };
	Write(A::EffectSlotMutexCheck + 5, nop2, 2);
	Log("  environmental audio slot mutex fixed");
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
		g_log = _fsopen(path, "w", _SH_DENYNO);	// readable while the game runs
		AddVectoredExceptionHandler(1, CrashHandler);

		// The game's code is still encrypted here, but its import table is already filled in.
		HMODULE sdl = GetModuleHandleA("SDL.dll");
		g_sdlInit = sdl ? (SDL_Init_t)GetProcAddress(sdl, "SDL_Init") : NULL;
		if(g_sdlInit && SameBytes(IatSDLInit, &g_sdlInit, 4)) WriteDword(IatSDLInit, (DWORD)&Hook_SDLInit);
	}
	return TRUE;
}
