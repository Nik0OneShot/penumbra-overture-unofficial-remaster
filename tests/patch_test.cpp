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
	if(argc < 3) { printf("usage: patch_test <dump> <jpg>\n"); return 2; }
	InitGfx();

	// 1. Map the dump at the game's image base
	FILE* f; fopen_s(&f, argv[1], "rb");
	if(!f) { printf("cannot open dump\n"); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	u8* img = (u8*)VirtualAlloc((void*)0x400000, n, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if(img != (u8*)0x400000) { printf("could not map at 0x400000 (%p)\n", img); return 2; }
	fread(img, 1, n, f); fclose(f);

	// 2. Load the proxy and trigger patching through IMG_Load
	HMODULE dll = LoadLibraryA("SDL_image.dll");
	CHECK(dll != NULL, "proxy SDL_image.dll loads");
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
