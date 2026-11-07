// Offline test for PenumbraUltrawide: maps a memory dump of the running game at its real
// address, loads the proxy SDL_image.dll so it patches that image, then drives each hook
// with fake game objects and checks the results. Never runs real game logic.
//
// Usage (from a folder holding SDL_image.dll, SDL_image_orig.dll and its dependencies):
//   patch_test.exe <penumbra_mem.bin> <some.jpg>

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <math.h>
#include <share.h>
#include <string>

typedef unsigned char u8;
static int g_fail;

#define CHECK(cond, ...) do { if(!(cond)) { ++g_fail; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                              else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while(0)
static bool Near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void PutJmp(DWORD at, const void* to)
{
	DWORD old; VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
	*(u8*)at = 0xE9; *(DWORD*)(at + 1) = (DWORD)to - (at + 5);
}
static DWORD CallTarget(DWORD site) { return site + 5 + *(int*)(site + 1); }

// Fake cLowLevelGraphicsSDL at 3440x1440 with the 800x600 virtual size
static u8 g_gfx[0x20];
static void InitGfx() { *(int*)(g_gfx + 4) = 3440; *(int*)(g_gfx + 8) = 1440; *(float*)(g_gfx + 0xC) = 800; *(float*)(g_gfx + 0x10) = 600; }
static const float K = (600.0f * 3440 / 1440) / 800, OFF = (600.0f * 3440 / 1440 - 800) / 2;

// Captures of what reached the "original" game functions
static float cPos[3], cSize[3]; static int cCalls;
static void __fastcall CapLong(void*, void*, void*, const float* p, const float* s, const float*, int, int, float)
{ memcpy(cPos, p, 12); memcpy(cSize, s, 8); ++cCalls; }
static void __fastcall CapShort(void*, void*, void*, const float* p) { memcpy(cPos, p, 12); ++cCalls; }
static void __fastcall CapTex(void*, void*, void*, const float* p, const float* s, const float*) { memcpy(cPos, p, 12); memcpy(cSize, s, 12); ++cCalls; }
static void __fastcall CapOrtho(void*, void*, const float*, float, float) { ++cCalls; }
static void __fastcall CapMouse(void* m, void*) { *(float*)((u8*)m + 0x2C) = 10; *(float*)((u8*)m + 0x30) = 10; ++cCalls; }
static int g_capFrames;
static void __cdecl CapFreeze(void*, float, float, int frames) { g_capFrames = frames; }
// Stand-in for cSectorVisibilityContainer::IntersectionBV: "visible" once the frustum's left
// plane has been pushed out to at least g_fakeNeedLeftD
static int g_fakeCalls; static float g_fakeNeedLeftD;
static bool __fastcall FakePortalTest(void* container, void*, void*, void*)
{
	++g_fakeCalls;
	return *(float*)((u8*)container + 0x26C + 0x2D0 + 0xC) >= g_fakeNeedLeftD;
}
// A null write inside this exe (in the "game folder" here), caught right away
static void TriggerHandledFault()
{
	__try { *(volatile int*)0 = 1; } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
static int g_capPid; static void* g_capPidThis;
static void __fastcall CapPidSize(void* self, void*, int n) { g_capPidThis = self; g_capPid = n; }
static int CountLogLines(const char* needle)
{
	FILE* f = _fsopen("PenumbraUltrawide.log", "r", _SH_DENYNO);	// the DLL still has it open
	if(!f) return -1;
	char line[512]; int n = 0;
	while(fgets(line, sizeof(line), f)) if(strstr(line, needle)) ++n;
	fclose(f);
	return n;
}
static float g_capVec[3]; static void* g_capThis;
static void __fastcall CapVec(void* self, void*, const float* v) { g_capThis = self; memcpy(g_capVec, v, 12); }

// Stand-in for cSqScript::LoadCharBuffer: reads g_fakeFile into a buffer from the game's new[]
static char g_gameDir[MAX_PATH];
static const char* g_fakeFile;
static size_t g_fakeOrigLen;
static char* g_fakeLastBuf;
static char* __fastcall FakeLoadCharBuffer(void*, void*, const void*, int* len)
{
	FILE* f; fopen_s(&f, g_fakeFile, "rb");
	if(!f) return NULL;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	typedef void* (__cdecl *New_t)(size_t);
	char* buf = (char*)((New_t)GetProcAddress(GetModuleHandleA("msvcr71.dll"), "??_U@YAPAXI@Z"))(n);
	fread(buf, 1, n, f); fclose(f);
	*len = n; g_fakeOrigLen = n; g_fakeLastBuf = buf;
	return buf;
}

typedef void (__fastcall *Long_t)(void*, void*, void*, const float*, const float*, const float*, int, int, float);
typedef void (__fastcall *Short_t)(void*, void*, void*, const float*);
typedef void (__fastcall *Tex_t)(void*, void*, void*, const float*, const float*, const float*);
typedef void (__fastcall *Mouse_t)(void*, void*);

// The redirected crosshair call expects the cPlayer in esi, like the real call site.
static void CallCrosshair(void* player, void* drawer, void* obj, float* pos)
{
	void* fn = (void*)CallTarget(0x4843D2);
	__asm {
		push esi
		mov esi, player
		push pos
		push obj
		mov ecx, drawer
		call fn
		pop esi
	}
}

// The fog-culling stub at its call site: edi = camera, st0 = fov*aspect, st1 = fogEnd*2
static float CallFogStub(float fov, float aspect, float* fogEnd2Out)
{
	void* fn = (void*)CallTarget(0x4F6D83);
	float cam[8] = {}; cam[4] = fov; cam[5] = aspect;	// mfFOV +10h, mfAspect +14h
	float* pCam = cam;
	float prod = fov * aspect, two = 40.0f, r = 0, f2 = 0;
	__asm {
		push edi
		mov edi, pCam
		fld two
		fld prod
		call fn
		fstp r
		fstp f2
		pop edi
	}
	*fogEnd2Out = f2;
	return r;
}

static HGLRC g_rc;
static void DropGLContext() { if(g_rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_rc); g_rc = NULL; } }

static bool MakeGLContext()
{
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "uwtest";
	RegisterClassA(&wc);
	HWND w = CreateWindowA("uwtest", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	HDC dc = GetDC(w);
	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
	SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
	g_rc = wglCreateContext(dc);
	return g_rc && wglMakeCurrent(dc, g_rc);
}

int main(int argc, char** argv)
{
	if(argc < 4) { printf("usage: patch_test <dump> <jpg> <game dir>\n"); return 2; }
	strcpy_s(g_gameDir, argv[3]);
	InitGfx();

	// 1. Map the dump at the game's image base
	FILE* f; fopen_s(&f, argv[1], "rb");
	if(!f) { printf("cannot open dump\n"); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	u8* img = (u8*)VirtualAlloc((void*)0x400000, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if(img != (u8*)0x400000)
	{
		// Address-space randomisation sometimes puts a heap there first; a fresh process usually won't.
		char tries[8] = "0";
		GetEnvironmentVariableA("UWTEST_TRIES", tries, sizeof(tries));
		int t = atoi(tries);
		if(t >= 10) { printf("could not map at 0x400000 after %d tries\n", t); return 2; }
		sprintf_s(tries, "%d", t + 1);
		SetEnvironmentVariableA("UWTEST_TRIES", tries);
		STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
		if(!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return 2;
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD rc = 2; GetExitCodeProcess(pi.hProcess, &rc);
		return (int)rc;
	}
	fread(img, 1, n, f); fclose(f);

	// The game's CRT and Newton are loaded in the real process; the high-framerate checks
	// look for them, so load them and point the dump's Newton import at the real export.
	HMODULE crt = LoadLibraryA("msvcr71.dll");
	HMODULE newton = LoadLibraryA("Newton.dll");
	CHECK(crt && newton, "game CRT and Newton load");
	FARPROC setFreeze = newton ? GetProcAddress(newton, "NewtonBodySetFreezeTreshold") : NULL;
	*(FARPROC*)0x63D4CC = setFreeze;
	// Same for SDL_Init: the proxy hooks it at load time to patch before game objects exist
	HMODULE sdl = LoadLibraryA("SDL.dll");
	FARPROC sdlInit = sdl ? GetProcAddress(sdl, "SDL_Init") : NULL;
	*(FARPROC*)0x63D6F0 = sdlInit;

	// 2. Load the proxy; patching happens at the game's first SDL_Init
	HMODULE dll = LoadLibraryA("SDL_image.dll");
	CHECK(dll != NULL, "proxy SDL_image.dll loads");
	CHECK(dll && *(FARPROC*)0x63D6F0 != sdlInit, "SDL_Init import hooked at DLL load (code still encrypted then)");
	CHECK(*(u8*)0x4CE1A0 != 0xE9, "nothing patched before SDL_Init");
	typedef int (__cdecl *SDL_Init_t)(unsigned);
	(*(SDL_Init_t*)0x63D6F0)(0);
	CHECK(*(u8*)0x4CE1A0 == 0xE9, "first SDL_Init applies the patches");
	typedef void* (__cdecl *IMG_Load_t)(const char*);
	IMG_Load_t imgLoad = (IMG_Load_t)GetProcAddress(dll, "IMG_Load");
	void* surf = imgLoad ? imgLoad(argv[2]) : NULL;
	CHECK(surf != NULL, "IMG_Load forwards to the real SDL_image and returns a surface");
	typedef int (__cdecl *IMG_isJPG_t)(void*);
	CHECK(GetProcAddress(dll, "IMG_isJPG") != NULL, "forwarded export IMG_isJPG resolves");

	// 3. Patched sites
	DWORD dllLo = (DWORD)dll, dllHi = dllLo + 0x100000;
	auto inDll = [&](DWORD a) { return a >= dllLo && a < dllHi; };
	CHECK(inDll(*(DWORD*)0x650A3C), "SetOrthoProjection vtable slot hooked");
	CHECK(inDll(*(DWORD*)0x65B910), "cMouseSDL::Update vtable slot hooked");
	CHECK(*(u8*)0x4CE1A0 == 0xE9 && inDll(CallTarget(0x4CE1A0)), "DrawGfxObject detoured");
	DWORD sites[] = { 0x4843D2, 0x48421A, 0x484295, 0x4842D4, 0x48C6C1, 0x44CC02, 0x44CC62 };
	for(DWORD s : sites) CHECK(inDll(CallTarget(s)), "call site %08X redirected", s);
	float* mul = *(float**)(0x48E388 + 2);
	CHECK(inDll((DWORD)mul) && *(float**)(0x494C68 + 2) == mul, "cursor fmul operands point at the DLL's multiplier");
	int* res = (int*)0x692CC0;
	bool sorted = true;
	for(int i = 1; i < 17; ++i)
		if(res[i*2] < res[i*2-2] || (res[i*2] == res[i*2-2] && res[i*2+1] <= res[i*2-1])) sorted = false;
	bool hasDesk = false; for(int i = 0; i < 17; ++i) if(res[i*2] == GetSystemMetrics(SM_CXSCREEN) && res[i*2+1] == GetSystemMetrics(SM_CYSCREEN)) hasDesk = true;
	CHECK(hasDesk && sorted, "resolution list has the desktop resolution and is sorted (first %dx%d, last %dx%d)", res[0], res[1], res[32], res[33]);

	// 4. Overlay rule through the real detour + trampoline. Unwind the trampoline's stolen
	//    "sub esp,3Ch / push esi" at 0x4CE1A8 and hand the call to the capture function.
	{
		DWORD old; VirtualProtect((void*)0x4CE1A8, 8, PAGE_EXECUTE_READWRITE, &old);
		u8 unwind[] = { 0x5E, 0x83, 0xC4, 0x3C };
		memcpy((void*)0x4CE1A8, unwind, 4);
		PutJmp(0x4CE1AC, &CapLong);
	}
	u8* drawer[1] = { g_gfx };
	float col[4] = { 1, 1, 1, 1 };
	Long_t drawLong = (Long_t)0x4CE1A0;
	{
		float p[3] = { 0, 0, 150 }, s[2] = { 800, 600 };
		cCalls = 0; drawLong(drawer, 0, (void*)1, p, s, col, 0, 0, 0);
		CHECK(cCalls == 1 && Near(cPos[0], -OFF) && Near(cSize[0], 800 * K) && cSize[1] == 600 && cPos[2] == 150,
			  "full-screen fade stretched to screen edges (x=%.2f w=%.2f)", cPos[0], cSize[0]);
		float p2[3] = { 0, 525, 40 }, s2[2] = { 800, 75 };
		drawLong(drawer, 0, (void*)1, p2, s2, col, 0, 0, 0);
		CHECK(Near(cPos[0], -OFF) && Near(cSize[0], 800 * K) && cPos[1] == 525, "letterbox bar stretched");
		float p3[3] = { 120, 200, 5 }, s3[2] = { 64, 64 };
		drawLong(drawer, 0, (void*)1, p3, s3, col, 0, 0, 0);
		CHECK(cPos[0] == 120 && cSize[0] == 64, "ordinary HUD sprite untouched");
	}

	// 5. Held-item cursor (long) and noise tile wrappers
	{
		Long_t cursor = (Long_t)CallTarget(0x48421A);
		float p[3] = { 0 - 20, 300 - 20, 100 }, s[2] = { 40, 40 };	// centred on stretched x=0
		cursor(drawer, 0, (void*)1, p, s, col, 0, 0, 0);
		CHECK(Near(cPos[0] + 20, -OFF) && cSize[0] == 40, "item cursor at screen-left edge lands on the left edge (centre %.2f)", cPos[0] + 20);
		float pc[3] = { 400 - 20, 300 - 20, 100 };
		cursor(drawer, 0, (void*)1, pc, s, col, 0, 0, 0);
		CHECK(Near(cPos[0], 380), "item cursor at centre stays centred");

		Long_t noise = (Long_t)CallTarget(0x48C6C1);
		float pn[3] = { 760, 0, 0 }, sn[2] = { 40, 40 };
		noise(drawer, 0, (void*)1, pn, sn, col, 0, 0, 0);
		CHECK(Near(cPos[0] + cSize[0], 800 + OFF), "last noise tile reaches the right screen edge (%.2f)", cPos[0] + cSize[0]);
	}

	// 6. Crosshair (short overload): image width read through mpMat -> image -> size
	{
		PutJmp(0x4CE270, &CapShort);
		u8 image[0x80] = {}; *(int*)(image + 0x6C) = 32;
		u8* imgs[1] = { image };
		u8 mat[0xC0] = {}; *(u8***)(mat + 0xB0) = imgs;
		u8 obj[0x20] = {}; *(u8**)(obj + 0x10) = mat;
		u8 player[0x300] = {};
		float p[3] = { 800 - 16, 300 - 16, 100 };	// centred on stretched right edge
		*(int*)(player + 0x2F0) = 1;	// Active
		cCalls = 0; CallCrosshair(player, drawer, obj, p);
		CHECK(cCalls == 1 && Near(cPos[0] + 16, 800 + OFF), "in-world crosshair at screen-right edge lands on the right edge (centre %.2f)", cPos[0] + 16);
		float pm[3] = { 250 - 16, 200 - 16, 100 };	// inventory mouse at layout x=250
		*(int*)(player + 0x2F0) = 5;	// Pointer
		CallCrosshair(player, drawer, obj, pm);
		CHECK(cCalls == 2 && cPos[0] == 250 - 16, "menu pointer (inventory/notebook) drawn where its clicks land (x=%.2f)", cPos[0] + 16);
	}

	// 7. Intro letterbox via cGraphicsHelper::DrawTexture (helper->mpLowLevelGfx at +0x14)
	{
		PutJmp(0x43E600, &CapTex);
		u8 helper[0x20] = {}; *(u8**)(helper + 0x14) = g_gfx;
		Tex_t tex = (Tex_t)CallTarget(0x44CC02);
		float p[3] = { 0, 0, 140 }, s[3] = { 800, 75, 0 };
		cCalls = 0; tex(helper, 0, (void*)1, p, s, col);
		CHECK(cCalls == 1 && Near(cPos[0], -OFF) && Near(cSize[0], 800 * K) && cSize[1] == 75, "intro letterbox stretched");
	}

	// 8. Mouse: horizontal relative motion boosted, in-world cursor multiplier compensated
	{
		PutJmp(0x56E270, &CapMouse);
		u8 mouse[0x70] = {}; *(u8**)(mouse + 0x64) = g_gfx;
		cCalls = 0; ((Mouse_t)*(DWORD*)0x65B910)(mouse, 0);
		float rx = *(float*)(mouse + 0x2C), ry = *(float*)(mouse + 0x30);
		CHECK(cCalls == 1 && Near(rx, 10 * K) && ry == 10, "mouse rel x scaled by %.3f (x=%.2f y=%.2f)", K, rx, ry);
		CHECK(Near(*mul * K, 800), "in-world cursor multiplier = 800/k (%.2f)", *mul);
	}

	// 8b. Logic rate: the first GetStepSize call retunes the live timer to the monitor rate
	{
		DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
		EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
		int rate = (int)dm.dmDisplayFrequency; if(rate < 60) rate = 60; if(rate > 360) rate = 360;
		double timer[4] = { 12345.0, 1000.0 / 60, 0, 0 };
		float step = ((float (__fastcall*)(void*))0x50F1E0)(timer);
		CHECK(Near(step * rate, 1) && fabs(timer[1] - 1000.0 / rate) < 1e-9 && timer[0] == 12345.0,
			  "logic timer retuned to %d Hz (step %.5f s, tick %.3f ms)", rate, step, timer[1]);
		typedef float (__fastcall *GameStep_t)(void*);
		void* game[0x40] = {}; *(double**)((u8*)game + *(u8*)(0x4A4FD0 + 2)) = timer;
		CHECK(Near(((GameStep_t)0x4A4FD0)(game) * rate, 1), "cGame::GetStepSize (used by game code) returns the new step");
	}

	// 8c. Level scripts are rewritten as they load
	{
		DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
		EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
		int rate = (int)dm.dmDisplayFrequency; if(rate < 60) rate = 60; if(rate > 360) rate = 360;

		PutJmp(0x513730, &FakeLoadCharBuffer);
		typedef char* (__fastcall *Load_t)(void*, void*, const void*, int*);
		Load_t load = (Load_t)CallTarget(0x51396F);
		CHECK(inDll((DWORD)load), "script load call redirected");

		static const char* files[] = { "level01_17_worm_nest.hps", "level01_11_new_storage_room.hps",
									   "level01_04_old_storage.hps", "level01_15_icecave.hps", "level01_14_refinery.hps",
									   "level01_19_door_shaft.hps" };
		char expect[6][128];
		sprintf_s(expect[0], "if(fSpeed > %.6ff)", 0.3 * 60 / rate);
		sprintf_s(expect[1], "float fTimeStep = 1.0f/%d.0f;", rate);
		sprintf_s(expect[2], "GivePlayerDamage(2.3f*%.6ff, \"BloodSplash\");", 60.0 / rate);
		sprintf_s(expect[3], "lDuringWaterCount %% %d != 0", (int)(3 * rate / 60.0f + 0.5f));
		sprintf_s(expect[4], "GetLocalVar(\"breakfancount\") == %d)", rate);
		sprintf_s(expect[5], "fCarryMixTicks += %.6ff; if(fCarryMixTicks >= 1.0f) { fCarryMixTicks -= 1.0f; UpdateCarryMix(); }", 60.0 / rate);
		for(int i = 0; i < 6; ++i)
		{
			char path[MAX_PATH]; sprintf_s(path, "%s\\maps\\%s", g_gameDir, files[i]);
			g_fakeFile = path;
			u8 name[0x1C] = {}; char nameBuf[64]; sprintf_s(nameBuf, "maps/%s", files[i]);
			*(char**)(name + 4) = nameBuf; *(DWORD*)(name + 0x14) = (DWORD)strlen(nameBuf); *(DWORD*)(name + 0x18) = 63;
			int len = 0;
			char* text = load(NULL, NULL, name, &len);
			std::string s(text ? text : "", text ? len : 0);
			CHECK(text && s.find(expect[i]) != std::string::npos, "%s rewritten (contains '%s')", files[i], expect[i]);
			if(i == 5)
			{
				// The explosive mix check: declared once, called once (from OnUpdate), at 60 Hz.
				size_t calls = 0;
				for(size_t at = s.find("UpdateCarryMix();"); at != std::string::npos; at = s.find("UpdateCarryMix();", at + 1)) ++calls;
				CHECK(s.find("float fPrevSpeed=0; float fCarryMixTicks=0;") != std::string::npos && calls == 1,
					  "explosive mix counter declared, check called once per 60 Hz step");
				float step = (float)atof(expect[5] + strlen("fCarryMixTicks += ")), acc = 0; int checks = 0;
				for(int t = 0; t < rate; ++t) { acc += step; if(acc >= 1.0f) { acc -= 1.0f; ++checks; } }
				CHECK(checks >= 59 && checks <= 60, "explosive mix checked %d times per second at %d Hz", checks, rate);
			}
		}
		// Untouched script: same buffer back
		char path[MAX_PATH]; sprintf_s(path, "%s\\maps\\level01_01_outside.hps", g_gameDir);
		g_fakeFile = path;
		u8 name[0x1C] = {}; strcpy_s((char*)name + 4, 16, "outside.hps"); *(DWORD*)(name + 0x18) = 15;
		int len = 0;
		char* text = load(NULL, NULL, name, &len);
		CHECK(text == g_fakeLastBuf && len == (int)g_fakeOrigLen, "other scripts load unchanged");

		// Engine constants
		double slope = **(double**)(0x4B47DD + 2);
		CHECK(inDll(*(DWORD*)(0x4B47DD + 2)) && fabs(slope + 0.15 * 60 / rate) < 1e-9, "slope threshold scaled (%.4f)", slope);
		CHECK(*(u8*)(0x43DA27 + 1) == (u8)(20 * rate / 60.0f + 0.5f), "door PID window %d ticks", *(u8*)(0x43DA27 + 1));

		void* probe = (void*)CallTarget(0x4B3D9A);
		float sumSq = 4.0f, reach = 0;
		__asm {
			sub esp, 30h
			fld sumSq
			call probe
			fld dword ptr [esp+24h]
			fstp reach
			add esp, 30h
		}
		CHECK(inDll((DWORD)probe) && Near(reach, 2.0f * rate / 60.0f), "stair probe reach x%.2f (%.3f)", rate / 60.0f, reach);

		// Newton freeze frames: catch the call where it reaches Newton
		PutJmp((DWORD)setFreeze, &CapFreeze);
		typedef void (__cdecl *Freeze_t)(void*, float, float, int);
		CHECK(inDll(*(DWORD*)0x63D4CC), "Newton freeze import hooked");
		(*(Freeze_t*)0x63D4CC)((void*)1, 0.1f, 0.1f, 10);
		CHECK(g_capFrames == (int)(10 * rate / 60.0f + 0.5f), "prop freeze delay 10 -> %d physics steps", g_capFrames);
	}

	// 8d. Tick compensations
	{
		DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
		EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
		int rate = (int)dm.dmDisplayFrequency; if(rate < 60) rate = 60; if(rate > 360) rate = 360;
		float s = rate / 60.0f;
		auto ticks = [&](int n) { return (int)(n * s + 0.5f); };

		CHECK(Near(**(float**)(0x48E83B + 2), 100 * s), "door drag force x%.2f (%.1f)", s, **(float**)(0x48E83B + 2));
		CHECK(fabs(**(float**)(0x48DCCB + 2) - 0.01f / s) < 1e-7, "hide 'standing still' threshold /%.2f", s);
		CHECK(fabs(**(float**)(0x41D637 + 2) - 1e-5f / (s * s)) < 1e-10, "worm movement threshold /%.2f^2", s);
		CHECK(*(float*)0x63DA48 == 300.0f && *(float*)0x643558 == 0.01f, "shared game constants untouched");
		CHECK(*(int*)0x483C7B == ticks(25) && *(int*)0x488F54 == ticks(16) && *(int*)0x40717A == ticks(150) && *(int*)0x41DDC6 == ticks(20),
			  "tick counts scaled (ground grace %d, hand lag %d, path samples %d, worm %d)", *(int*)0x483C7B, *(int*)0x488F54, *(int*)0x40717A, *(int*)0x41DDC6);
		CHECK(Near(*(float*)0x480EE1 * rate, 1), "sound trigger life = 1 tick (%.5f s)", *(float*)0x480EE1);

		// Jump: stub replaces "mov edx,[edi+80h]" between a cmp and its jne
		void* jump = (void*)CallTarget(0x48097C);
		float state[0x30] = {}; state[0x80 / 4] = 350.0f;
		void* pState = state; DWORD edxOut; u8 zf;
		__asm {
			push edi
			mov edi, pState
			xor eax, eax
			cmp eax, eax			// ZF = 1
			call jump
			setz zf
			mov edxOut, edx
			pop edi
		}
		CHECK(Near(*(float*)&edxOut, 350 * s) && zf == 1, "jump force x%.2f (%.1f), flags kept", s, *(float*)&edxOut);

		// Knockback / spider leap: one-tick character force, original vector untouched
		PutJmp(0x4B1BF0, &CapVec);
		typedef void (__fastcall *Force_t)(void*, void*, const float*);
		float f[3] = { 1, 2, 3 };
		((Force_t)CallTarget(0x41A88E))((void*)0x55, 0, f);
		CHECK(g_capThis == (void*)0x55 && Near(g_capVec[0], s) && Near(g_capVec[2], 3 * s) && f[0] == 1,
			  "spider leap force x%.2f, game's vector untouched", s);

		// Torque stub at a "call [edx+80h]" site
		void* torque = (void*)CallTarget(0x441D9D);
		void* vtbl[0x30] = {}; vtbl[0x80 / 4] = (void*)&CapVec; vtbl[0x78 / 4] = (void*)&CapVec;
		void* pVtbl = vtbl; float tq[3] = { 2, 4, 6 }; float* pTq = tq;
		DWORD espBefore, espAfter;
		__asm {
			mov espBefore, esp
			push pTq
			mov ecx, 0x66
			mov edx, pVtbl
			call torque			// AddTorque pops the pointer, like the original call
			mov espAfter, esp
		}
		CHECK(espBefore == espAfter && g_capThis == (void*)0x66 && Near(g_capVec[1], 4 * s) && tq[1] == 4,
			  "hit/throw torque x%.2f, stack balanced", s);

		// Explosion stub: vector at [esp+40h..48h], pointer at [esp], y still on the FPU stack
		void* expl = (void*)CallTarget(0x402AFF);
		float y = 5; DWORD ecxOut; float ex[3];
		__asm {
			push esi
			sub esp, 50h
			lea eax, [esp+40h]
			mov [esp], eax
			mov dword ptr [esp+40h], 3F800000h	// x = 1
			mov dword ptr [esp+48h], 40400000h	// z = 3
			mov esi, 77h
			fld y
			call expl
			mov ecxOut, ecx
			mov eax, [esp]
			mov edx, [eax]
			mov ex, edx
			mov edx, [eax+4]
			mov ex+4, edx
			mov edx, [eax+8]
			mov ex+8, edx
			add esp, 50h
			pop esi
		}
		CHECK(ecxOut == 0x77 && Near(ex[0], s) && Near(ex[1], 5 * s) && Near(ex[2], 3 * s), "explosion push x%.2f", s);

		// Debris stub: vector at [esp+34h..3Ch], pointer at [esp], tail-calls [eax+78h]
		void* brk = (void*)CallTarget(0x430168);
		__asm {
			mov espBefore, esp
			sub esp, 40h
			lea eax, [esp+34h]
			mov [esp], eax
			mov dword ptr [esp+34h], 3F800000h	// x = 1
			mov dword ptr [esp+3Ch], 40400000h	// z = 3
			fld y
			mov ecx, 88h
			mov eax, pVtbl
			call brk			// AddForce pops the pointer
			add esp, 3Ch
			mov espAfter, esp
		}
		CHECK(espBefore == espAfter && g_capThis == (void*)0x88 && Near(g_capVec[1], 5 * s) && Near(g_capVec[2], 3 * s),
			  "debris force x%.2f, stack balanced", s);
	}

	// 8d2. Ported from the Black Plague remaster: drag brake delay, controller and grab PIDs, EFX
	{
		DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
		EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
		int rate = (int)dm.dmDisplayFrequency; if(rate < 60) rate = 60; if(rate > 360) rate = 360;
		auto ticks = [&](int n) { return (int)(n * (rate / 60.0f) + 0.5f); };
		bool brake = true;
		for(DWORD a : { 0x48E794u, 0x48E7F4u, 0x48E86Du, 0x491485u }) brake &= *(int*)(a + 3) == ticks(20);
		CHECK(brake, "drag brake delay 20 -> %d ticks at all four sites", ticks(20));
		bool grab = true;
		for(DWORD a : { 0x490851u, 0x490869u, 0x490878u, 0x49088Bu, 0x492DD3u, 0x492DEBu, 0x492DFAu, 0x492E0Du }) grab &= *(u8*)(a + 1) == ticks(10);
		CHECK(grab, "grab PID windows 10 -> %d", ticks(10));

		PutJmp(0x4F48C0, &CapPidSize);
		typedef void (__fastcall *Pid_t)(void*, void*, int);
		Pid_t loaderPid = (Pid_t)CallTarget(0x4E0E37);
		CHECK(inDll((DWORD)loaderPid), "entity controller PID size redirected");
		loaderPid((void*)0x99, 0, 6);
		int scaled = g_capPid;
		loaderPid((void*)0x99, 0, 0);
		CHECK(scaled == ticks(6) && g_capPid == 0 && g_capPidThis == (void*)0x99, "controller IntegralSize 6 -> %d, 0 stays 0", scaled);

		CHECK(*(u8*)0x57F921 == 0x90 && *(u8*)0x57F922 == 0x90, "environmental audio: effect slot mutex always created");
	}

	// 8d3. Crash reporter: logs faults in game-folder modules, not the DLL's own probes
	{
		u8 badObj[0x20] = {}; *(u8**)(badObj + 0x10) = (u8*)0x10;	// image lookup will fault inside the DLL
		u8 player[0x300] = {}; float p[3] = { 100, 100, 100 };
		CallCrosshair(player, drawer, badObj, p);	// ImageHalfWidth probes and recovers
		CHECK(CountLogLines("CRASH:") == 0, "the DLL's own guarded memory probes are not reported");
		TriggerHandledFault();
		CHECK(CountLogLines("CRASH: exception C0000005 at patch_test.exe+") == 1 && CountLogLines("writing address 00000000") == 1
			  && CountLogLines("  eax ") == 1, "fault in a game-folder module logged with module+offset, address and registers");
	}

	// 8e. Wide-view culling: camera near-plane box and fog-culling box
	{
		void* origin = (void*)CallTarget(0x5045CD);
		CHECK(inDll((DWORD)origin) && *(u8*)(0x5045CD + 5) == 0x90, "frustum origin box patched");
		float nearP = 0.05f, fov = 1.2217305f /* 70 deg */, aspect = 3440.0f / 1440, farP = 100.0f;
		float box = 0; DWORD eaxOut, edxOut;
		__asm {
			sub esp, 90h
			mov eax, nearP
			mov [esp+78h], eax
			mov eax, fov
			mov [esp+7Ch], eax
			mov eax, aspect
			mov [esp+80h], eax
			mov eax, farP
			mov [esp+74h], eax
			mov eax, 12345678h
			call origin
			fstp box
			mov eaxOut, eax
			mov edxOut, edx
			add esp, 90h
		}
		float t = tanf(fov * 0.5f), corner = nearP * sqrtf(1 + t * t * (1 + aspect * aspect));
		CHECK(Near(box * 1000, 2 * corner * 1000) && box > 2 * nearP * 2 && eaxOut == 0x12345678 && *(float*)&edxOut == farP,
			  "near-plane box %.4f covers 21:9 corners (vanilla %.4f), registers kept", box, 2 * nearP);

		CHECK(inDll(CallTarget(0x4F6D83)), "fog culling cosine patched");
		float f2 = 0;
		float c219 = CallFogStub(1.2217305f, 3440.0f / 1440, &f2);
		CHECK(Near(c219, cosf(1.2217305f * (3440.0f / 1440) * 0.5f)) && f2 == 40.0f, "fog box at 21:9 unchanged from vanilla (cos %.4f), x87 stack intact", c219);
		float c329 = CallFogStub(1.2217305f, 5120.0f / 1440, &f2);
		CHECK(c329 > 0.2f && c329 < 1.0f, "fog box at 32:9 valid (cos %.4f; vanilla would be %.4f)", c329, cosf(1.2217305f * (5120.0f / 1440) * 0.5f));
	}

	// 8f. Doorway (portal) test: retried with the view widened by the margin, then restored
	{
		CHECK(inDll(CallTarget(0x523E60)), "doorway visibility test redirected");
		PutJmp(0x523310, &FakePortalTest);
		static u8 container[0x700]; static u8 portal[0x120];
		*(int*)(portal + 4) = 7;
		float* leftD = (float*)(container + 0x26C + 0x2D0 + 0xC);
		float* radius = (float*)(container + 0x26C + 0x378);
		*leftD = 0; *radius = 10;
		typedef bool (__fastcall *Test_t)(void*, void*, void*, void*);
		Test_t portalTest = (Test_t)CallTarget(0x523E60);
		g_fakeCalls = 0; g_fakeNeedLeftD = 1.5f;
		bool vis = portalTest(container, 0, portal + 0x90, NULL);
		CHECK(vis && g_fakeCalls == 2 && *leftD == 0 && *radius == 10, "doorway just outside the view is drawn; view restored afterwards");
		g_fakeCalls = 0; g_fakeNeedLeftD = -1;
		vis = portalTest(container, 0, portal + 0x90, NULL);
		CHECK(vis && g_fakeCalls == 1, "doorway in view: exact test only");
		g_fakeCalls = 0; g_fakeNeedLeftD = 5.0f;
		vis = portalTest(container, 0, portal + 0x90, NULL);
		CHECK(!vis && g_fakeCalls == 2 && *leftD == 0, "doorway far outside the view stays hidden");
	}

	// 9. Ortho: post effects pass through; UI pass gets the centred projection
	{
		PutJmp(0x52FFE0, &CapOrtho);
		DWORD hook = *(DWORD*)0x650A3C;
		float vs[2] = { 800, 600 };
		cCalls = 0; ((void (__fastcall*)(void*, void*, const float*, float, float))hook)(g_gfx, 0, vs, -1000, 1000);
		CHECK(cCalls == 1, "non-UI caller (post effects) gets the original projection");

		if(getenv("UWTEST_NO_GL")) { printf("skipping projection check (UWTEST_NO_GL)\n"); goto done; }
		CHECK(MakeGLContext(), "GL context for projection check");
		DWORD old; VirtualProtect((void*)0x4CD534, 1, PAGE_EXECUTE_READWRITE, &old);
		*(u8*)0x4CD534 = 0xC3;	// ret, back to us
		void* gfx = g_gfx; float* pvs = vs; float mn = -1000, mx = 1000;
		cCalls = 0;
		__asm {
			lea eax, back
			push eax
			push mx
			push mn
			push pvs
			mov ecx, gfx
			push 0x4CD534		; return address of the DrawAll call site
			jmp hook
		back:
		}
		float m[16]; glGetFloatv(GL_PROJECTION_MATRIX, m);
		float l = (-1 - m[12]) / m[0], r = (1 - m[12]) / m[0];
		CHECK(cCalls == 0 && Near(l, -OFF) && Near(r, 800 + OFF), "DrawAll projection spans x %.2f..%.2f", l, r);
		DropGLContext();
	}

done:
	printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
	fflush(stdout);
	if(getenv("UWTEST_SKIP_UNLOAD")) ExitProcess(g_fail ? 1 : 0);
	FreeLibrary(dll);
	printf("proxy unloaded cleanly\n");
	fflush(stdout);
	return g_fail ? 1 : 0;
}
