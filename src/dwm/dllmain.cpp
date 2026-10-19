// Fringeless DWM correction engine (injected into dwm.exe).
//
// Derived from dwm_lut by lauralex / ledoge and the Windows 11 25H2 compatibility fork
// by 0x401gg:
//   https://github.com/lauralex/dwm_lut              (GPL-3.0)
//   https://github.com/0x401gg/dwm_lut-windows-25h2  (GPL-3.0)
//
// This file is part of Fringeless and is distributed under the GNU General Public License
// version 3; see the LICENSE file at the repository root. It keeps dwm_lut's method of
// hooking COverlayContext::Present and force-disabling MPO/DirectFlip so the correction
// stays on the composited path.
//
// Changes from the original dwm_lut (2026):
//   * The per-pixel 3D-LUT shader is replaced by a per-channel sub-pixel shift that
//     cancels QD-OLED ClearType colour fringing.
//   * Correction parameters (enable flag + per-channel X/Y offsets) are read live from a
//     shared, memory-mapped file instead of from .cube LUT files.
//   * The DLL loads with no LUT files present; it corrects only the SDR desktop.
//   * Ported from MSVC to MinGW-w64 (logging macro, structured-exception guard removed,
//     intrinsics, viewport initialisation, min/max).
//
// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

#include <io.h>
#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#pragma comment (lib, "d3d11.lib") // Maybe un-useful
#pragma comment (lib, "d3dcompiler.lib")
#pragma comment (lib, "dxgi.lib") // Maybe un-useful
#pragma comment (lib, "uuid.lib") // Maybe un-useful
#pragma comment (lib, "dxguid.lib")


#pragma intrinsic(_ReturnAddress)

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifdef __GNUC__
#define _ReturnAddress() __builtin_return_address(0)
#endif

#define DITHER_GAMMA 2.2
#define LUT_FOLDER "%SYSTEMROOT%\\Temp\\luts"

#define RELEASE_IF_NOT_NULL(x) { if (x != NULL) { x->Release(); } }
#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)
#define RESIZE(x, y) realloc(x, (y) * sizeof(*x));
#define LOG_FILE_PATH R"(%TEMP%\dwm_lut.log)"
#define LOG_PATH_FILE R"(%SYSTEMROOT%\Temp\dwm_lut_log_path.txt)"
#define MAX_LOG_FILE_SIZE 20 * 1024 * 1024
#ifdef _DEBUG
#define DEBUG_MODE true
#else
#define DEBUG_MODE false
#endif

#if DEBUG_MODE == true
#define __LOG_ONLY_ONCE(x, y) do { static bool first_log_##y = true; if (first_log_##y) { log_to_file(x); first_log_##y = false; } } while (0);
#define _LOG_ONLY_ONCE(x, y) __LOG_ONLY_ONCE(x, y)
#define LOG_ONLY_ONCE(x) _LOG_ONLY_ONCE(x, __COUNTER__)
#define MESSAGE_BOX_DBG(x, y) MessageBoxA(NULL, x, "DEBUG HOOK DWM", y);

#define EXECUTE_WITH_LOG(winapi_func_hr) \
	do { \
		HRESULT hr = (winapi_func_hr); \
		if (FAILED(hr)) \
		{ \
			std::stringstream ss; \
			ss << "ERROR AT LINE: " << __LINE__ << " HR: " << hr << " - DETAILS: "; \
			LPSTR error_message = nullptr; \
			FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, \
				NULL, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&error_message, 0, NULL); \
			ss << error_message; \
			log_to_file(ss.str().c_str()); \
			LocalFree(error_message); \
			throw std::exception(ss.str().c_str()); \
		} \
	} while (false);

#define EXECUTE_D3DCOMPILE_WITH_LOG(winapi_func_hr, error_interface) \
	do { \
		HRESULT hr = (winapi_func_hr); \
		if (FAILED(hr)) \
		{ \
			std::stringstream ss; \
			ss << "ERROR AT LINE: " << __LINE__ << " HR: " << hr << " - DETAILS: "; \
			LPSTR error_message = nullptr; \
			FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, \
				NULL, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&error_message, 0, NULL); \
			ss << error_message << " - DX COMPILE ERROR: " << (char*)error_interface->GetBufferPointer(); \
			error_interface->Release(); \
			log_to_file(ss.str().c_str()); \
			LocalFree(error_message); \
			throw std::exception(ss.str().c_str()); \
		} \
	} while (false);

#define LOG_ADDRESS(prefix_message, address) \
	{ \
		std::stringstream ss; \
		ss << prefix_message << " 0x" << std::setw(sizeof(address) * 2) << std::setfill('0') << std::hex << (UINT_PTR)address; \
		log_to_file(ss.str().c_str()); \
	}

#else
#define __LOG_ONLY_ONCE(x, y) do { static bool first_log_##y = true; if (first_log_##y) { log_to_file(x); first_log_##y = false; } } while (0);
#define _LOG_ONLY_ONCE(x, y) __LOG_ONLY_ONCE(x, y)
#define LOG_ONLY_ONCE(x) _LOG_ONLY_ONCE(x, __COUNTER__)
#define MESSAGE_BOX_DBG(x, y) // NOP, not in debug mode
#define EXECUTE_WITH_LOG(winapi_func_hr) winapi_func_hr;
#define EXECUTE_D3DCOMPILE_WITH_LOG(winapi_func_hr, error_interface) winapi_func_hr;
#define LOG_ADDRESS(prefix_message, address) \
	{ \
		std::stringstream ss; \
		ss << prefix_message << " 0x" << std::setw(sizeof(address) * 2) << std::setfill('0') << std::hex << (UINT_PTR)address; \
		log_to_file(ss.str().c_str()); \
	}
#endif


void log_to_file(const char* log_buf)
{
	char logFilePath[MAX_PATH];
	ExpandEnvironmentStringsA(LOG_FILE_PATH, logFilePath, sizeof(logFilePath));

	char logPathFile[MAX_PATH];
	ExpandEnvironmentStringsA(LOG_PATH_FILE, logPathFile, sizeof(logPathFile));
	FILE* pPathFile = fopen(logPathFile, "r");
	if (pPathFile != NULL)
	{
		char configuredLogFilePath[MAX_PATH];
		if (fgets(configuredLogFilePath, sizeof(configuredLogFilePath), pPathFile) != NULL)
		{
			configuredLogFilePath[strcspn(configuredLogFilePath, "\r\n")] = '\0';
			if (configuredLogFilePath[0] != '\0')
			{
				strcpy_s(logFilePath, configuredLogFilePath);
			}
		}
		fclose(pPathFile);
	}

	FILE* pFile = fopen(logFilePath, "a");
	if (pFile == NULL)
	{
		return;
	}
	fseek(pFile, 0, SEEK_END);
	long size = ftell(pFile);
	if (size > MAX_LOG_FILE_SIZE)
	{
		if (_chsize(_fileno(pFile), 0) == -1)
		{
			fclose(pFile);
			return;
		}
	}
	fseek(pFile, 0, SEEK_END);
	fprintf(pFile, "%s\n", log_buf);
	fclose(pFile);
}

void print_error(const char* prefix_message)
{
	DWORD errorCode = GetLastError();
	LPSTR errorMessage = nullptr;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	               nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&errorMessage, 0, nullptr);

	char message_buf[100];
	sprintf(message_buf, "%s: %s - error code: %u", prefix_message, errorMessage, errorCode);
	log_to_file(message_buf);
	return;
}


unsigned int lut_index(const unsigned int b, const unsigned int g, const unsigned int r, const unsigned int c,
                       const unsigned int lut_size)
{
	return lut_size * lut_size * 4 * b + lut_size * 4 * g + 4 * r + c;
}

#define LUT_ACCESS_INDEX(lut, b, g, r, c, lut_size) (*((float*)(lut) + lut_index(b, g, r, c, lut_size)))

// Find relative address utility function
void* get_relative_address(void* instruction_address, int offset, int instruction_size)
{
	int relative_offset = *(int*)((unsigned char*)instruction_address + offset);

	return (unsigned char*)instruction_address + instruction_size + relative_offset;
}


const unsigned char COverlayContext_Present_bytes[] = {
	0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xb1, 0x20,
	0x2c, 0x00, 0x00, 0x45, 0x8b, 0xd0, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xf6, 0x0f, 0x85
};
const int IOverlaySwapChain_IDXGISwapChain_offset = -0x118;

const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes[] = {
	0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83,
	0xec, 0x40
};
const unsigned char COverlayContext_OverlaysEnabled_bytes[] = {
	0x75, 0x04, 0x32, 0xc0, 0xc3, 0xcc, 0x83, 0x79, 0x30, 0x01, 0x0f, 0x97, 0xc0, 0xc3
};

const int COverlayContext_DeviceClipBox_offset = -0x120;

const int IOverlaySwapChain_HardwareProtected_offset = -0xbc;

/*
 * AOB for function: COverlayContext_Present_bytes_w11
 *
 * 40 53 55 56 57 41 56 41 57 48 81 EC 88 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 78 48
 *
 */
const unsigned char COverlayContext_Present_bytes_w11[] = {
	0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05,
	'?', '?', '?', '?', 0x48, 0x33, 0xC4, 0x48, 0x89, 0x44, 0x24, 0x78, 0x48
};
const int IOverlaySwapChain_IDXGISwapChain_offset_w11 = 0xE0;

/*
 * AOB for function: COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11
 *
 * 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 68 48
 */
const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11[] = {
	0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC,
	0x68, 0x48,
};

/*
 * AOB for function: COverlayContext_OverlaysEnabled_bytes_w11
 *
 * 83 3D ?? ?? ?? ?? ?? 75 04
 */
const unsigned char COverlayContext_OverlaysEnabled_bytes_w11[] = {
	0x83, 0x3D, '?', '?', '?', '?', '?', 0x75, 0x04
};

int COverlayContext_DeviceClipBox_offset_w11 = 0x466C;

const int IOverlaySwapChain_HardwareProtected_offset_w11 = -0x144;


/**
 * AOB for function COverlayContext_Present_bytes_w11_24h2
 * 
 * 4C 8B DC 56 41 56
 */
const unsigned char COverlayContext_Present_bytes_w11_24h2[] = {
	0x4C, 0x8B, 0xDC, 0x56, 0x41, 0x56
};

const int IOverlaySwapChain_IDXGISwapChain_offset_w11_24h2 = 0x108; // wrt OverlaySwapChain

const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2[] = {
	0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, '?', 0x48, 0x89, 0x68, '?', 0x48, 0x89, 0x70, '?', 0x48, 0x89, 0x78, '?', 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB
};

const unsigned char COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2[] = {
	0xE8, '?', '?', '?', '?', 0x84, 0xC0, 0xB8, 0x04, 0x00, 0x00, 0x00
};

int COverlayContext_DeviceClipBox_offset_w11_24h2 = 0x53E8;

const int IOverlaySwapChain_HardwareProtected_offset_w11_24h2 = 0x64;


/**
 * AOB for function COverlayContext_Present_bytes_w11_25h2
 *
 * 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC F8 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 EF 4C 8B 65 ?? 48 8B D9
 */
const unsigned char COverlayContext_Present_bytes_w11_25h2[] = {
	0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
	0x24, 0xF9, 0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05,
	'?', '?', '?', '?', 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0xEF, 0x4C, 0x8B, 0x65, '?', 0x48, 0x8B, 0xD9
};

/**
 * AOB for function COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2
 *
 * 48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 33 DB
 */
const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2[] = {
	0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
	0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB
};

/**
 * AOB for function COverlayContext_OverlaysEnabled_bytes_w11_25h2
 *
 * 83 3D ?? ?? ?? ?? 05 74 09 83 79 28 01 0F 97 C0 C3
 */
const unsigned char COverlayContext_OverlaysEnabled_bytes_w11_25h2[] = {
	0x83, 0x3D, '?', '?', '?', '?', 0x05, 0x74, 0x09, 0x83, 0x79, 0x28, 0x01, 0x0F, 0x97, 0xC0, 0xC3
};

// On 25H2: position stored as ints (not floats) at realObj+0xDC (left) and realObj+0xE0 (top)
int COverlayContext_DeviceClipBox_offset_w11_25h2 = 0x7698;

const int IOverlaySwapChain_HardwareProtected_offset_w11_25h2 = 0x4C;

// In 25H2, swap chain is obtained via vtable call at index 33 (offset 0x108)
const int IOverlaySwapChain_GetSwapChain_vtable_offset_w11_25h2 = 0x108;


bool isWindows11 = false;
bool isWindows11_24h2 = false;
bool isWindows11_25h2 = false;

static DWORD GetWindowsBuildNumber()
{
	typedef LONG (WINAPI* RtlGetVersion_t)(OSVERSIONINFOEXW*);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersion_t rtlGetVersion = ntdll == NULL ? NULL :
		reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion"));
	if (rtlGetVersion == NULL)
		return 0;

	OSVERSIONINFOEXW version = {};
	version.dwOSVersionInfoSize = sizeof(version);
	return rtlGetVersion(&version) == 0 ? version.dwBuildNumber : 0;
}

// Pointer to DWM's OverlayTestMode global variable (found from OverlaysEnabled AOB)
static int* g_pOverlayTestMode = NULL;
static int g_originalOverlayTestMode = 0;
static bool g_overlayTestModeSaved = false;

bool aob_match_inverse(const void* buf1, const void* mask, const int buf_len)
{
	for (int i = 0; i < buf_len; ++i)
	{
		if (((unsigned char*)buf1)[i] != ((unsigned char*)mask)[i] && ((unsigned char*)mask)[i] != '?')
		{
			return true;
		}
	}
	return false;
}

char shaders[] = R"(
struct VS_INPUT {
	float2 pos : POSITION;
	float2 tex : TEXCOORD;
};

struct VS_OUTPUT {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD;
};

Texture2D backBufferTex : register(t0);
SamplerState smp : register(s0);

cbuffer CB : register(b0) {
	float2 offR;
	float2 offG;
	float2 offB;
	float2 pad;
};

VS_OUTPUT VS(VS_INPUT input) {
	VS_OUTPUT output;
	output.pos = float4(input.pos, 0, 1);
	output.tex = input.tex;
	return output;
}

float4 PS(VS_OUTPUT input) : SV_TARGET {
	float r = backBufferTex.Sample(smp, input.tex + offR).r;
	float g = backBufferTex.Sample(smp, input.tex + offG).g;
	float b = backBufferTex.Sample(smp, input.tex + offB).b;
	return float4(r, g, b, 1);
}
)";

// Fringeless baked per-channel sub-pixel shift in pixels (R, G, B), from the tuned
// settings. Used only as a fallback when no live parameter file is present. Channel c is
// resampled at (pixel centre - shift_c), lining the colour channels up with the physical
// subpixels and cancelling the ClearType fringe.
static const float kShiftX[3] = { 0.000f, -0.146f, -0.011f };
static const float kShiftY[3] = { 0.000f,  0.494f,  0.000f };

// Live parameters, shared with the controlling app through a small memory-mapped file
// at %SYSTEMROOT%\Temp\fringeless_params.bin (given a NULL DACL by the injector so this
// DWM-hosted DLL can open it). The app writes; this DLL reads once per presented frame.
#pragma pack(push, 4)
struct FringeParams {
    unsigned int magic;   // kParamsMagic once the app has written a valid block
    int          enabled; // 0 = pass the frame through unchanged
    float        sx[3];   // per-channel X shift in pixels (R, G, B)
    float        sy[3];   // per-channel Y shift in pixels
    int          bicubic; // reserved; bilinear sampling for now
};
#pragma pack(pop)
static const unsigned int kParamsMagic = 0x46524E47; // 'FRNG'
static volatile FringeParams* g_params = NULL;
static HANDLE g_paramsFile = NULL;
static HANDLE g_paramsMap = NULL;
// Named marker (NULL DACL) the controlling app opens to tell that the engine is already
// loaded, so it does not inject a second time (and prompt for UAC again).
static HANDLE g_loadedMarker = NULL;

static void MapParams() {
    if (g_params) return;
    char path[MAX_PATH];
    ExpandEnvironmentStringsA("%SYSTEMROOT%\\Temp\\fringeless_params.bin", path, sizeof(path));
    g_paramsFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_paramsFile == INVALID_HANDLE_VALUE) { g_paramsFile = NULL; return; }
    g_paramsMap = CreateFileMappingA(g_paramsFile, NULL, PAGE_READONLY, 0, sizeof(FringeParams), NULL);
    if (!g_paramsMap) { CloseHandle(g_paramsFile); g_paramsFile = NULL; return; }
    g_params = (FringeParams*)MapViewOfFile(g_paramsMap, FILE_MAP_READ, 0, 0, sizeof(FringeParams));
}

ID3D11Device* device;
ID3D11DeviceContext* deviceContext;
ID3D11VertexShader* vertexShader;
ID3D11PixelShader* pixelShader;
ID3D11InputLayout* inputLayout;

ID3D11Buffer* vertexBuffer;
UINT numVerts;
UINT stride;
UINT offset;

D3D11_TEXTURE2D_DESC backBufferDesc;
D3D11_TEXTURE2D_DESC textureDesc[2];

ID3D11SamplerState* samplerState;
ID3D11Texture2D* texture[2];
ID3D11ShaderResourceView* textureView[2];

ID3D11SamplerState* noiseSamplerState;
ID3D11ShaderResourceView* noiseTextureView;

ID3D11Buffer* constantBuffer;

struct lutData
{
	int left;
	int top;
	int size;
	bool isHdr;
	ID3D11ShaderResourceView* textureView;
	float* rawLut;
};

void DrawRectangle(struct tagRECT* rect, int index)
{
	float width = backBufferDesc.Width;
	float height = backBufferDesc.Height;

	float screenLeft = rect->left / width;
	float screenTop = rect->top / height;
	float screenRight = rect->right / width;
	float screenBottom = rect->bottom / height;

	float left = screenLeft * 2 - 1;
	float top = screenTop * -2 + 1;
	float right = screenRight * 2 - 1;
	float bottom = screenBottom * -2 + 1;

	width = textureDesc[index].Width;
	height = textureDesc[index].Height;
	float texLeft = rect->left / width;
	float texTop = rect->top / height;
	float texRight = rect->right / width;
	float texBottom = rect->bottom / height;

	float vertexData[] = {
		left, bottom, texLeft, texBottom,
		left, top, texLeft, texTop,
		right, bottom, texRight, texBottom,
		right, top, texRight, texTop
	};

	D3D11_MAPPED_SUBRESOURCE resource;
	EXECUTE_WITH_LOG(deviceContext->Map(vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource))
	memcpy(resource.pData, vertexData, stride * numVerts);
	deviceContext->Unmap(vertexBuffer, 0);

	deviceContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

	deviceContext->Draw(numVerts, 0);
}

int numLuts;

lutData* luts;

bool ParseLUT(lutData* lut, char* filename)
{
	FILE* file = fopen(filename, "r");
	if (file == NULL) return false;

	char line[256];
	unsigned int lutSize;

	while (1)
	{
		if (!fgets(line, sizeof(line), file))
		{
			fclose(file);
			return false;
		}
		if (sscanf(line, "LUT_3D_SIZE%d", &lutSize) == 1)
		{
			break;
		}
	}
	// borgaccio
	float* rawLut = (float*)malloc(lutSize * lutSize * lutSize * 4 * sizeof(float));
	// lut_3d_vec rawLut(lutSize, { lutSize, {lutSize, RGBA_VEC} });

	for (int b = 0; b < lutSize; b++)
	{
		for (int g = 0; g < lutSize; g++)
		{
			for (int r = 0; r < lutSize; r++)
			{
				while (1)
				{
					if (!fgets(line, sizeof(line), file))
					{
						fclose(file);
						// free(rawLut);
						return false;
					}
					if (line[0] <= '9' && line[0] != '#' && line[0] != '\n')
					{
						float red, green, blue;

						if (sscanf(line, "%f%f%f", &red, &green, &blue) != 3)
						{
							fclose(file);
							// free(rawLut);
							return false;
						}
						LUT_ACCESS_INDEX(rawLut, b, g, r, 0, lutSize) = red;
						LUT_ACCESS_INDEX(rawLut, b, g, r, 1, lutSize) = green;
						LUT_ACCESS_INDEX(rawLut, b, g, r, 2, lutSize) = blue;
						LUT_ACCESS_INDEX(rawLut, b, g, r, 3, lutSize) = 1;

						break;
					}
				}
			}
		}
	}
	fclose(file);
	lut->size = lutSize;
	lut->rawLut = rawLut;
	return true;
}

bool AddLUTs(char* folder)
{
	WIN32_FIND_DATAA findData;

	char path[MAX_PATH];
	strcpy(path, folder);
	strcat(path, "\\*");
	HANDLE hFind = FindFirstFileA(path, &findData);
	if (hFind == INVALID_HANDLE_VALUE) return false;
	do
	{
		if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			char filePath[MAX_PATH];
			char* fileName = findData.cFileName;

			strcpy(filePath, folder);
			strcat(filePath, "\\");
			strcat(filePath, fileName);

			luts = (lutData*)RESIZE(luts, numLuts + 1)
			lutData* lut = &luts[numLuts];
			if (sscanf(findData.cFileName, "%d_%d", &lut->left, &lut->top) == 2)
			{
				lut->isHdr = strstr(fileName, "hdr") != NULL;
				lut->textureView = NULL;
				if (!ParseLUT(lut, filePath))
				{
					LOG_ONLY_ONCE("LUT could not be parsed")
					FindClose(hFind);
					return false;
				}
				numLuts++;
			}
		}
	}
	while (FindNextFileA(hFind, &findData) != 0);
	FindClose(hFind);
	return true;
}

int numLutTargets;
void** lutTargets;

// 25H2: Track which COverlayContext was initially HDR (main monitor)
// When a game disables HDR, this context switches to SDR but we still want to apply the LUT to it
static void* g_primaryHdrContext = NULL;

bool IsLUTActive(void* target)
{
	for (int i = 0; i < numLutTargets; i++)
	{
		if (lutTargets[i] == target)
		{
			return true;
		}
	}
	return false;
}

void SetLUTActive(void* target)
{
	if (!IsLUTActive(target))
	{
		lutTargets = (void**)RESIZE(lutTargets, numLutTargets + 1)
		lutTargets[numLutTargets++] = target;
	}
}

void UnsetLUTActive(void* target)
{
	for (int i = 0; i < numLutTargets; i++)
	{
		if (lutTargets[i] == target)
		{
			lutTargets[i] = lutTargets[--numLutTargets];
			lutTargets = (void**)RESIZE(lutTargets, numLutTargets)
			return;
		}
	}
}

lutData* GetLUTDataFromCOverlayContext(void* context, bool hdr)
{
	int left, top;

	if (isWindows11_25h2)
	{
		void* realObj = *(void**)context;
		float* rect = (float*)((unsigned char*)realObj + 0x7698);
		// On current 25H2 builds rect[0]/rect[1] are the composed surface size.
		// The monitor position is stored in rect[2]/rect[3], matching the 24H2
		// object layout.
		left = (int)rect[2];
		top = (int)rect[3];

		char message_buf[256];
		sprintf(message_buf, "25H2 LUT lookup: context=0x%p realObj=0x%p rect={%.1f,%.1f,%.1f,%.1f} left=%d top=%d hdr=%d",
			context, realObj, rect[0], rect[1], rect[2], rect[3], left, top, hdr);
		LOG_ONLY_ONCE(message_buf)
	}
	else if (isWindows11_24h2)
	{
		float* rect = (float*)((unsigned char*)*(void**)context + COverlayContext_DeviceClipBox_offset_w11_24h2);
		left = (int)rect[2];
		top = (int)rect[3];
		char message_buf[100];
		sprintf(message_buf, "Left: %d, Top: %d", left, top);
		LOG_ONLY_ONCE(message_buf)

		// Put rect address in message
		sprintf(message_buf, "Rect address: 0x%p", rect);
		LOG_ONLY_ONCE(message_buf)
	}
	else if (isWindows11)
	{
		float* rect = (float*)((unsigned char*)*(void**)context + COverlayContext_DeviceClipBox_offset_w11);
		left = (int)rect[0];
		top = (int)rect[1];
		char message_buf[100];
		sprintf(message_buf, "Left: %d, Top: %d", left, top);
		LOG_ONLY_ONCE(message_buf)

		// Put rect address in message
		sprintf(message_buf, "Rect address: 0x%p", rect);
		LOG_ONLY_ONCE(message_buf)
	}
	else
	{
		int* rect = (int*)((unsigned char*)context + COverlayContext_DeviceClipBox_offset);
		left = rect[0];
		top = rect[1];
	}

	for (int i = 0; i < numLuts; i++)
	{
		if (luts[i].left == left && luts[i].top == top && luts[i].isHdr == hdr)
		{
			char message_buf[160];
			sprintf(message_buf, "Matched LUT: monitor left=%d top=%d hdr=%d lutSize=%u", left, top, hdr, luts[i].size);
			LOG_ONLY_ONCE(message_buf)
			return &luts[i];
		}
	}

	// 25H2 fallback: if this is the primary HDR context but no matching LUT found
	// (game disabled HDR, context switched to SDR), try matching with opposite HDR flag
	if (isWindows11_25h2 && g_primaryHdrContext == context)
	{
		for (int i = 0; i < numLuts; i++)
		{
			if (luts[i].left == left && luts[i].top == top && luts[i].isHdr != hdr)
			{
				char message_buf[160];
				sprintf(message_buf, "Matched 25H2 primary HDR fallback LUT: left=%d top=%d requestedHdr=%d lutHdr=%d", left, top, hdr, luts[i].isHdr);
				LOG_ONLY_ONCE(message_buf)
				return &luts[i];
			}
		}
	}

	// 25H2 single-monitor safety fallback: if the DWM coordinate layout changes
	// again but the user only configured one LUT for the current SDR/HDR mode,
	// apply that LUT instead of silently doing nothing. Avoid this on multi-LUT
	// setups so we do not apply the wrong display's calibration.
	if (isWindows11_25h2)
	{
		lutData* singleModeLut = NULL;
		int matchingModeLuts = 0;
		for (int i = 0; i < numLuts; i++)
		{
			if (luts[i].isHdr == hdr)
			{
				singleModeLut = &luts[i];
				matchingModeLuts++;
			}
		}

		if (matchingModeLuts == 1 && singleModeLut != NULL)
		{
			char message_buf[192];
			sprintf(message_buf, "25H2 single-LUT fallback: using configured LUT left=%d top=%d hdr=%d for detected left=%d top=%d",
				singleModeLut->left, singleModeLut->top, hdr, left, top);
			LOG_ONLY_ONCE(message_buf)
			return singleModeLut;
		}
	}

	if (isWindows11_25h2)
	{
		char message_buf[256];
		sprintf(message_buf, "No LUT match: left=%d top=%d hdr=%d numLuts=%d", left, top, hdr, numLuts);
		LOG_ONLY_ONCE(message_buf)
		for (int i = 0; i < numLuts; i++)
		{
			sprintf(message_buf, "Configured LUT[%d]: left=%d top=%d hdr=%d size=%u", i, luts[i].left, luts[i].top, luts[i].isHdr, luts[i].size);
			LOG_ONLY_ONCE(message_buf)
		}
	}

	return NULL;
}

void InitializeStuff(ID3D11Device* inputDevice)
{
	try
	{
		device = inputDevice;
		device->AddRef();
		LOG_ONLY_ONCE("Device successfully gathered")
		LOG_ADDRESS("The device address is: ", device)

		device->GetImmediateContext(&deviceContext);
		LOG_ONLY_ONCE("Got context after device")
		LOG_ADDRESS("The Device context is located at address: ", deviceContext)
		{
			ID3DBlob* vsBlob;
			ID3DBlob* compile_error_interface;
			LOG_ONLY_ONCE(("Trying to compile vshader with this code:\n" + std::string(shaders)).c_str())
			EXECUTE_D3DCOMPILE_WITH_LOG(
				D3DCompile(shaders, sizeof shaders, NULL, NULL, NULL, "VS", "vs_5_0", 0, 0, &vsBlob, &
					compile_error_interface), compile_error_interface)


			LOG_ONLY_ONCE("Vertex shader compiled successfully")
			EXECUTE_WITH_LOG(device->CreateVertexShader(vsBlob->GetBufferPointer(),
				vsBlob->GetBufferSize(), NULL, &vertexShader))


			LOG_ONLY_ONCE("Vertex shader created successfully")
			D3D11_INPUT_ELEMENT_DESC inputElementDesc[] =
			{
				{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
				{
					"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
					D3D11_INPUT_PER_VERTEX_DATA, 0
				}
			};
			EXECUTE_WITH_LOG(device->CreateInputLayout(inputElementDesc, ARRAYSIZE(inputElementDesc),
				vsBlob->GetBufferPointer(),
				vsBlob->GetBufferSize(), &inputLayout))

			vsBlob->Release();
		}
		{
			ID3DBlob* psBlob;
			ID3DBlob* compile_error_interface;
			EXECUTE_D3DCOMPILE_WITH_LOG(
				D3DCompile(shaders, sizeof shaders, NULL, NULL, NULL, "PS", "ps_5_0", 0, 0, &psBlob, &
					compile_error_interface), compile_error_interface)

			LOG_ONLY_ONCE("Pixel shader compiled successfully")
			device->CreatePixelShader(psBlob->GetBufferPointer(),
			                          psBlob->GetBufferSize(), NULL, &pixelShader);
			psBlob->Release();
		}
		{
			stride = 4 * sizeof(float);
			numVerts = 4;
			offset = 0;

			D3D11_BUFFER_DESC vertexBufferDesc = {};
			vertexBufferDesc.ByteWidth = stride * numVerts;
			vertexBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
			vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			vertexBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			EXECUTE_WITH_LOG(device->CreateBuffer(&vertexBufferDesc, NULL, &vertexBuffer))
		}
		{
			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;

			EXECUTE_WITH_LOG(device->CreateSamplerState(&samplerDesc, &samplerState))
		}
		for (int i = 0; i < numLuts; i++)
		{
			lutData* lut = &luts[i];

			D3D11_TEXTURE3D_DESC desc = {};
			desc.Width = lut->size;
			desc.Height = lut->size;
			desc.Depth = lut->size;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			D3D11_SUBRESOURCE_DATA initData;
			initData.pSysMem = lut->rawLut;
			initData.SysMemPitch = lut->size * 4 * sizeof(float);
			initData.SysMemSlicePitch = lut->size * lut->size * 4 * sizeof(float);

			ID3D11Texture3D* tex;
			EXECUTE_WITH_LOG(device->CreateTexture3D(&desc, &initData, &tex))
			EXECUTE_WITH_LOG(device->CreateShaderResourceView((ID3D11Resource*)tex, NULL, &luts[i].textureView))
			tex->Release();
			free(lut->rawLut);
			lut->rawLut = NULL;
		}
		{
			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;

			EXECUTE_WITH_LOG(device->CreateSamplerState(&samplerDesc, &noiseSamplerState))
		}
		{
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = NOISE_SIZE;
			desc.Height = NOISE_SIZE;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			float noise[NOISE_SIZE][NOISE_SIZE];

			for (int i = 0; i < NOISE_SIZE; i++)
			{
				for (int j = 0; j < NOISE_SIZE; j++)
				{
					noise[i][j] = (noiseBytes[i][j] + 0.5) / 256;
				}
			}

			D3D11_SUBRESOURCE_DATA initData;
			initData.pSysMem = noise;
			initData.SysMemPitch = sizeof(noise[0]);

			ID3D11Texture2D* tex;
			EXECUTE_WITH_LOG(device->CreateTexture2D(&desc, &initData, &tex))
			EXECUTE_WITH_LOG(device->CreateShaderResourceView((ID3D11Resource*)tex, NULL, &noiseTextureView))
			tex->Release();
		}
		{
			D3D11_BUFFER_DESC constantBufferDesc = {};
			constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			constantBufferDesc.ByteWidth = 32;
			constantBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
			constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			EXECUTE_WITH_LOG(device->CreateBuffer(&constantBufferDesc, NULL, &constantBuffer))
			LOG_ONLY_ONCE("Final buffer created in InitializeStuff")
		}
	}
	catch (std::exception& ex)
	{
		std::stringstream ex_message;
		ex_message << "Exception caught at line " << __LINE__ << ": " << ex.what() << std::endl;
		LOG_ONLY_ONCE(ex_message.str().c_str())
		throw;
	}
	catch (...)
	{
		std::stringstream ex_message;
		ex_message << "Exception caught at line " << __LINE__ << ": " << std::endl;
		LOG_ONLY_ONCE(ex_message.str().c_str())
		throw;
	}
}

void UninitializeStuff()
{
	RELEASE_IF_NOT_NULL(device)
	RELEASE_IF_NOT_NULL(deviceContext)
	RELEASE_IF_NOT_NULL(vertexShader)
	RELEASE_IF_NOT_NULL(pixelShader)
	RELEASE_IF_NOT_NULL(inputLayout)
	RELEASE_IF_NOT_NULL(vertexBuffer)
	RELEASE_IF_NOT_NULL(samplerState)
	for (int i = 0; i < 2; i++)
	{
		RELEASE_IF_NOT_NULL(texture[i])
		RELEASE_IF_NOT_NULL(textureView[i])
	}
	RELEASE_IF_NOT_NULL(noiseSamplerState)
	RELEASE_IF_NOT_NULL(noiseTextureView)
	RELEASE_IF_NOT_NULL(constantBuffer)
	for (int i = 0; i < numLuts; i++)
	{
		free(luts[i].rawLut);
		RELEASE_IF_NOT_NULL(luts[i].textureView)
	}
	free(luts);
	free(lutTargets);
}

// Shared rendering core. Does NOT release backBuffer - caller handles it.
bool RenderLUT(void* cOverlayContext, ID3D11Texture2D* backBuffer, struct tagRECT* rects, int numRects)
{
	ID3D11RenderTargetView* renderTargetView;

	D3D11_TEXTURE2D_DESC newBackBufferDesc;
	backBuffer->GetDesc(&newBackBufferDesc);

	char desc_message[256];
	sprintf(desc_message, "RenderLUT: format=%u width=%u height=%u numRects=%d", newBackBufferDesc.Format, newBackBufferDesc.Width, newBackBufferDesc.Height, numRects);
	LOG_ONLY_ONCE(desc_message)

	int index = -1;
	if (newBackBufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
	{
		index = 0;
	}
	else if (newBackBufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		index = 1;
		// 25H2: Remember which context uses HDR - this is the primary/main monitor
		if (isWindows11_25h2 && g_primaryHdrContext == NULL)
		{
			g_primaryHdrContext = cOverlayContext;
		}
	}

	// Fringeless: correct only the SDR (B8G8R8A8) desktop; leave HDR contexts untouched.
	if (index != 0)
	{
		LOG_ONLY_ONCE("Skipping non-SDR context")
		return false;
	}

	// Pick up live parameters from the controlling app (mapped lazily; retried cheaply
	// until the file appears). When the app has toggled correction off, pass the frame
	// through unchanged.
	if (!g_params) { static int t = 0; if ((t++ & 127) == 0) MapParams(); }
	if (g_params && g_params->magic == kParamsMagic && !g_params->enabled)
	{
		return false;
	}

	D3D11_TEXTURE2D_DESC oldTextureDesc = textureDesc[index];
	if (newBackBufferDesc.Width > oldTextureDesc.Width || newBackBufferDesc.Height > oldTextureDesc.Height)
	{
		if (texture[index] != NULL)
		{
			texture[index]->Release();
			textureView[index]->Release();
		}

		UINT newWidth = max(newBackBufferDesc.Width, oldTextureDesc.Width);
		UINT newHeight = max(newBackBufferDesc.Height, oldTextureDesc.Height);

		D3D11_TEXTURE2D_DESC newTextureDesc;

		newTextureDesc = newBackBufferDesc;
		newTextureDesc.Width = newWidth;
		newTextureDesc.Height = newHeight;
		newTextureDesc.Usage = D3D11_USAGE_DEFAULT;
		newTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		newTextureDesc.CPUAccessFlags = 0;
		newTextureDesc.MiscFlags = 0;

		textureDesc[index] = newTextureDesc;

		EXECUTE_WITH_LOG(device->CreateTexture2D(&textureDesc[index], NULL, &texture[index]))
		EXECUTE_WITH_LOG(
			device->CreateShaderResourceView((ID3D11Resource*)texture[index], NULL, &textureView[index]))
	}

	backBufferDesc = newBackBufferDesc;

	EXECUTE_WITH_LOG(device->CreateRenderTargetView((ID3D11Resource*)backBuffer, NULL, &renderTargetView))
	const D3D11_VIEWPORT d3d11_viewport = {0.0f, 0.0f, (FLOAT)backBufferDesc.Width, (FLOAT)backBufferDesc.Height, 0.0f, 1.0f};
	deviceContext->RSSetViewports(1, &d3d11_viewport);

	deviceContext->OMSetRenderTargets(1, &renderTargetView, NULL);
	renderTargetView->Release();

	deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	deviceContext->IASetInputLayout(inputLayout);

	deviceContext->VSSetShader(vertexShader, NULL, 0);
	deviceContext->PSSetShader(pixelShader, NULL, 0);

	deviceContext->PSSetShaderResources(0, 1, &textureView[index]);
	deviceContext->PSSetSamplers(0, 1, &samplerState);

	// Fringeless: per-channel sub-pixel offsets in UV. Shifts are centred about zero so
	// the whole image barely moves; the relative geometry is what cancels the fringe.
	// The sign (sample at centre - shift) is folded in here so the shader only adds.
	float sx[3], sy[3];
	for (int ch = 0; ch < 3; ch++) { sx[ch] = kShiftX[ch]; sy[ch] = kShiftY[ch]; }
	if (g_params && g_params->magic == kParamsMagic)
	{
		for (int ch = 0; ch < 3; ch++) { sx[ch] = g_params->sx[ch]; sy[ch] = g_params->sy[ch]; }
	}
	float texelW = 1.0f / (float)textureDesc[index].Width;
	float texelH = 1.0f / (float)textureDesc[index].Height;
	float ctrX = -(min(sx[0], min(sx[1], sx[2])) + max(sx[0], max(sx[1], sx[2]))) * 0.5f;
	float ctrY = -(min(sy[0], min(sy[1], sy[2])) + max(sy[0], max(sy[1], sy[2]))) * 0.5f;
	float cb[8];
	for (int ch = 0; ch < 3; ch++)
	{
		cb[ch * 2 + 0] = -(sx[ch] + ctrX) * texelW;
		cb[ch * 2 + 1] = -(sy[ch] + ctrY) * texelH;
	}
	cb[6] = 0.0f;
	cb[7] = 0.0f;

	D3D11_MAPPED_SUBRESOURCE resource;
	EXECUTE_WITH_LOG(deviceContext->Map((ID3D11Resource*)constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
		&resource))
	memcpy(resource.pData, cb, sizeof(cb));
	deviceContext->Unmap((ID3D11Resource*)constantBuffer, 0);

	deviceContext->PSSetConstantBuffers(0, 1, &constantBuffer);

	// Fringeless: shift ONLY the rectangles DWM recomposed this frame. They hold fresh,
	// unshifted content, so each region is shifted exactly once. Shifting the whole frame
	// instead re-shifts static areas every present and makes the image drift away.
	for (int i = 0; i < numRects; i++)
	{
		D3D11_BOX sourceRegion;
		sourceRegion.left = rects[i].left;
		sourceRegion.right = rects[i].right;
		sourceRegion.top = rects[i].top;
		sourceRegion.bottom = rects[i].bottom;
		sourceRegion.front = 0;
		sourceRegion.back = 1;

		deviceContext->CopySubresourceRegion((ID3D11Resource*)texture[index], 0, rects[i].left,
		                                     rects[i].top, 0, (ID3D11Resource*)backBuffer, 0, &sourceRegion);
		DrawRectangle(&rects[i], index);
	}

	return true;
}

bool ApplyLUT(void* cOverlayContext, IDXGISwapChain* swapChain, struct tagRECT* rects, int numRects)
{
	try
	{
		if (!device)
		{
			LOG_ONLY_ONCE("Initializing stuff in ApplyLUT")
			ID3D11Device* dev;
			EXECUTE_WITH_LOG(swapChain->GetDevice(IID_ID3D11Device, (void**)&dev))
			InitializeStuff(dev);
			dev->Release();
		}
		LOG_ONLY_ONCE("Init done, continuing with LUT application")

		ID3D11Texture2D* backBuffer;
		EXECUTE_WITH_LOG(swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&backBuffer))

		bool result = RenderLUT(cOverlayContext, backBuffer, rects, numRects);
		backBuffer->Release();
		return result;
	}
	catch (std::exception& ex)
	{
		std::stringstream ex_message;
		ex_message << "Exception caught at line " << __LINE__ << ": " << ex.what() << std::endl;
		LOG_ONLY_ONCE(ex_message.str().c_str())
		return false;
	}
	catch (...)
	{
		std::stringstream ex_message;
		ex_message << "Exception caught at line " << __LINE__ << std::endl;
		LOG_ONLY_ONCE(ex_message.str().c_str())
		return false;
	}
}

// Apply LUT directly from a back buffer texture (25H2 path - no swap chain needed)
bool ApplyLUTDirect(void* cOverlayContext, ID3D11Texture2D* backBuffer, struct tagRECT* rects, int numRects)
{
	try
	{
		if (!device)
		{
			LOG_ONLY_ONCE("Initializing from texture device (25H2)")
			ID3D11Device* dev;
			backBuffer->GetDevice(&dev);
			InitializeStuff(dev);
			dev->Release();
		}
		return RenderLUT(cOverlayContext, backBuffer, rects, numRects);
	}
	catch (std::exception& ex)
	{
		std::stringstream ex_message;
		ex_message << "Exception caught at line " << __LINE__ << ": " << ex.what() << std::endl;
		LOG_ONLY_ONCE(ex_message.str().c_str())
		return false;
	}
	catch (...)
	{
		std::stringstream ex_message;
		ex_message << "Exception caught at line " << __LINE__ << std::endl;
		LOG_ONLY_ONCE(ex_message.str().c_str())
		return false;
	}
}

typedef struct rectVec
{
	struct tagRECT* start;
	struct tagRECT* end;
	struct tagRECT* cap;
} rectVec;

typedef long (COverlayContext_Present_t)(void*, void*, unsigned int, rectVec*, unsigned int, bool);
typedef long long (COverlayContext_Present_24h2_t)(void*, void*, unsigned int, rectVec*, int, void*, bool);

// Get back buffer texture from overlaySwapChain on 25H2
// On 25H2, no IDXGISwapChain exists. Instead: overlaySwapChain->vt[24]() -> result->vt[19]() -> QI(ID3D11Texture2D)
static ID3D11Texture2D* GetBackBuffer_25H2(void* overlaySwapChain)
{
	{
		if (!overlaySwapChain)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: overlaySwapChain is null")
			return NULL;
		}

		void** vt = *(void***)overlaySwapChain;
		if (!vt)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: overlaySwapChain vtable is null")
			return NULL;
		}

		typedef void* (__fastcall *VirtFunc)(void*);

		VirtFunc func1 = (VirtFunc)vt[24];
		if (!func1)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: vt[24] is null")
			return NULL;
		}

		void* r1 = func1(overlaySwapChain);
		if (!r1)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: vt[24]() returned null")
			return NULL;
		}

		void** vt2 = *(void***)r1;
		if (!vt2)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: second vtable is null")
			return NULL;
		}

		VirtFunc func2 = (VirtFunc)vt2[19];
		if (!func2)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: vt2[19] is null")
			return NULL;
		}

		void* r2 = func2(r1);
		if (!r2)
		{
			LOG_ONLY_ONCE("25H2 back-buffer lookup failed: vt2[19]() returned null")
			return NULL;
		}

		ID3D11Texture2D* tex = NULL;
		HRESULT hr = ((IUnknown*)r2)->QueryInterface(IID_ID3D11Texture2D, (void**)&tex);
		if (FAILED(hr) || !tex)
		{
			char message_buf[160];
			sprintf(message_buf, "25H2 back-buffer lookup failed: QI(ID3D11Texture2D) hr=0x%08X tex=0x%p", hr, tex);
			LOG_ONLY_ONCE(message_buf)
			return NULL;
		}

		LOG_ONLY_ONCE("25H2: Got texture via overlaySwapChain->vt[24]()->vt2[19]()->QI")
		return tex;
	}
}

COverlayContext_Present_t* COverlayContext_Present_orig = NULL;
COverlayContext_Present_t* COverlayContext_Present_real_orig = NULL;

COverlayContext_Present_24h2_t* COverlayContext_Present_orig_24h2 = NULL;
COverlayContext_Present_24h2_t* COverlayContext_Present_real_orig_24h2 = NULL;

long long COverlayContext_Present_hook_24h2(void* self, void* overlaySwapChain, unsigned int a3, rectVec* rectVec,
	int a5, void* a6, bool a7)
{
	if (_ReturnAddress() < (void*)COverlayContext_Present_real_orig_24h2 || isWindows11_24h2 || isWindows11_25h2)
	{
			LOG_ONLY_ONCE("I am inside COverlayContext::Present hook inside the main if condition")
			std::stringstream overlay_swapchain_message;
			overlay_swapchain_message << "OverlaySwapChain address: 0x" << std::hex << overlaySwapChain
				<< " -- windows 11 25h2: " << isWindows11_25h2
				<< " -- windows 11 24h2: " << isWindows11_24h2
				<< " -- " << "windows 11: " << isWindows11;
			LOG_ONLY_ONCE(overlay_swapchain_message.str().c_str())

			if (isWindows11_25h2)
			{
				// 25H2: Get texture directly from overlaySwapChain via vtable chain
				ID3D11Texture2D* backBuffer = GetBackBuffer_25H2(overlaySwapChain);
				if (backBuffer)
				{
					if (ApplyLUTDirect(self, backBuffer, rectVec->start, rectVec->end - rectVec->start))
						SetLUTActive(self);
					else
						UnsetLUTActive(self);
					backBuffer->Release();
				}
				else
				{
					UnsetLUTActive(self);
				}
			}
			else
			{

			bool hwProtected = false;
			if (isWindows11_24h2)
				hwProtected = *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset_w11_24h2);
			else if (isWindows11)
				hwProtected = *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset_w11);
			else
				hwProtected = *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset);

			if (hwProtected)
			{
				LOG_ONLY_ONCE("Hardware protected - unsetting LUT active")
				UnsetLUTActive(self);
			}
			else
			{
				IDXGISwapChain* swapChain = NULL;

				if (isWindows11_24h2)
				{
					LOG_ONLY_ONCE("Gathering IDXGISwapChain pointer")
					swapChain = *(IDXGISwapChain**)((unsigned char*)overlaySwapChain +
						IOverlaySwapChain_IDXGISwapChain_offset_w11_24h2);
				}
				else if (isWindows11)
				{
					LOG_ONLY_ONCE("Gathering IDXGISwapChain pointer")
					int sub_from_legacy_swapchain = *(int*)((unsigned char*)overlaySwapChain - 4);
					void* real_overlay_swap_chain = (unsigned char*)overlaySwapChain - sub_from_legacy_swapchain -
						0x1b0;
					swapChain = *(IDXGISwapChain**)((unsigned char*)real_overlay_swap_chain +
						IOverlaySwapChain_IDXGISwapChain_offset_w11);
				}
				else
				{
					swapChain = *(IDXGISwapChain**)((unsigned char*)overlaySwapChain +
						IOverlaySwapChain_IDXGISwapChain_offset);
				}

				if (swapChain != NULL && ApplyLUT(self, swapChain, rectVec->start, rectVec->end - rectVec->start))
				{
					LOG_ONLY_ONCE("Setting LUTactive")
					SetLUTActive(self);
				}
				else
				{
					LOG_ONLY_ONCE("Un-setting LUTactive")
					UnsetLUTActive(self);
				}
			}
			} // end else (non-25H2)
	}

	return COverlayContext_Present_orig_24h2(self, overlaySwapChain, a3, rectVec, a5, a6, a7);
}


long COverlayContext_Present_hook(void* self, void* overlaySwapChain, unsigned int a3, rectVec* rectVec,
                                  unsigned int a5, bool a6)
{
	if (_ReturnAddress() < (void*)COverlayContext_Present_real_orig)
	{
		LOG_ONLY_ONCE("I am inside COverlayContext::Present hook inside the main if condition")

		bool hwProtected = false;
		if (isWindows11)
			hwProtected = *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset_w11);
		else
			hwProtected = *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset);

		if (hwProtected)
		{
			LOG_ONLY_ONCE("Hardware protected - unsetting LUT active")
			UnsetLUTActive(self);
		}
		else
		{
			IDXGISwapChain* swapChain;

			if (isWindows11)
			{
				LOG_ONLY_ONCE("Gathering IDXGISwapChain pointer")
				int sub_from_legacy_swapchain = *(int*)((unsigned char*)overlaySwapChain - 4);
				void* real_overlay_swap_chain = (unsigned char*)overlaySwapChain - sub_from_legacy_swapchain -
					0x1b0;
				swapChain = *(IDXGISwapChain**)((unsigned char*)real_overlay_swap_chain +
					IOverlaySwapChain_IDXGISwapChain_offset_w11);
			}
			else
			{
				swapChain = *(IDXGISwapChain**)((unsigned char*)overlaySwapChain +
					IOverlaySwapChain_IDXGISwapChain_offset);
			}

			if (ApplyLUT(self, swapChain, rectVec->start, rectVec->end - rectVec->start))
			{
				LOG_ONLY_ONCE("Setting LUTactive")
				SetLUTActive(self);
			}
			else
			{
				LOG_ONLY_ONCE("Un-setting LUTactive")
				UnsetLUTActive(self);
			}
		}
	}

	return COverlayContext_Present_orig(self, overlaySwapChain, a3, rectVec, a5, a6);
}

typedef bool (COverlayContext_IsCandidateDirectFlipCompatbile_t)(void*, void*, void*, void*, int, unsigned int, bool,
                                                                 bool);
typedef bool (COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t)(void*, void*, void*, void*, unsigned int, bool);

COverlayContext_IsCandidateDirectFlipCompatbile_t* COverlayContext_IsCandidateDirectFlipCompatbile_orig;
COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t* COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2;

bool COverlayContext_IsCandidateDirectFlipCompatbile_hook_24h2(void* self, void* a2, void* a3, void* a4, unsigned int a5,
	bool a6)
{
	if (true)
	{
		return false;
	}
	return COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2(self, a2, a3, a4, a5, a6);
}

bool COverlayContext_IsCandidateDirectFlipCompatbile_hook(void* self, void* a2, void* a3, void* a4, int a5,
                                                          unsigned int a6, bool a7, bool a8)
{
	if (true)
	{
		return false;
	}
	return COverlayContext_IsCandidateDirectFlipCompatbile_orig(self, a2, a3, a4, a5, a6, a7, a8);
}

typedef bool (COverlayContext_OverlaysEnabled_t)(void*);

COverlayContext_OverlaysEnabled_t* COverlayContext_OverlaysEnabled_orig  = NULL;

bool COverlayContext_OverlaysEnabled_hook(void* self)
{
	if (true)
	{
		return false;
	}
	return COverlayContext_OverlaysEnabled_orig(self);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			HMODULE dwmcore = GetModuleHandleW(L"dwmcore.dll");
			MODULEINFO moduleInfo;
			GetModuleInformation(GetCurrentProcess(), dwmcore, &moduleInfo, sizeof moduleInfo);

			const DWORD buildNumber = GetWindowsBuildNumber();
			if (buildNumber == 0)
				return FALSE;

			char startupMessage[256];
			sprintf(startupMessage, "DWM LUT loading: build=%lu", buildNumber);
			LOG_ONLY_ONCE(startupMessage)

			// Match exact servicing families. Treating every future build as 25H2 is
			// dangerous because these offsets point into undocumented DWM objects.
			isWindows11_25h2 = buildNumber == 26200;
			isWindows11_24h2 = buildNumber == 26100;
			isWindows11 = buildNumber >= 22000 && buildNumber < 26100;

			if (buildNumber > 26200)
				return FALSE;

			if (isWindows11_25h2)
			{
				for (size_t i = 0; i <= moduleInfo.SizeOfImage - sizeof COverlayContext_OverlaysEnabled_bytes_w11_25h2; i++)
				{
					unsigned char* address = (unsigned char*)dwmcore + i;
					if (!COverlayContext_Present_orig_24h2 && sizeof COverlayContext_Present_bytes_w11_25h2 <= moduleInfo.
						SizeOfImage - i && !aob_match_inverse(address, COverlayContext_Present_bytes_w11_25h2,
							sizeof COverlayContext_Present_bytes_w11_25h2))
					{
						COverlayContext_Present_orig_24h2 = (COverlayContext_Present_24h2_t*)address;
						COverlayContext_Present_real_orig_24h2 = COverlayContext_Present_orig_24h2;
					}
					else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 && sizeof
						COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2 <= moduleInfo.SizeOfImage - i && !
						aob_match_inverse(
							address, COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2,
							sizeof COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2))
					{
						COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 = (
							COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t*)address;
					}
					else if (!COverlayContext_OverlaysEnabled_orig && sizeof COverlayContext_OverlaysEnabled_bytes_w11_25h2
						<= moduleInfo.SizeOfImage - i && !aob_match_inverse(
							address, COverlayContext_OverlaysEnabled_bytes_w11_25h2,
							sizeof COverlayContext_OverlaysEnabled_bytes_w11_25h2))
					{
						// 25H2 OverlaysEnabled is a direct function match (like W11)
						COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)address;

						// Extract the OverlayTestMode global variable address from the instruction:
						// 83 3D [rip_offset32] 05 = cmp dword ptr [rip+offset], 5
						// Global var addr = instruction_addr + 7 + *(int32_t*)(instruction_addr + 2)
						int rip_offset = *(int*)(address + 2);
						g_pOverlayTestMode = (int*)(address + 7 + rip_offset);
					}
					if (COverlayContext_Present_orig_24h2 && COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 &&
						COverlayContext_OverlaysEnabled_orig)
					{
						break;
					}
				}
			}
			else if (isWindows11_24h2)
			{
				for (size_t i = 0; i <= moduleInfo.SizeOfImage - sizeof COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2; i++)
				{
					unsigned char* address = (unsigned char*)dwmcore + i;
					if (!COverlayContext_Present_orig && sizeof COverlayContext_Present_bytes_w11_24h2 <= moduleInfo.
						SizeOfImage - i && !aob_match_inverse(address, COverlayContext_Present_bytes_w11_24h2,
							sizeof COverlayContext_Present_bytes_w11_24h2))
					{
						COverlayContext_Present_orig_24h2 = (COverlayContext_Present_24h2_t*)address;
						COverlayContext_Present_real_orig_24h2 = COverlayContext_Present_orig_24h2;
					}
					else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig && sizeof
						COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2 <= moduleInfo.SizeOfImage - i && !
						aob_match_inverse(
							address, COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2,
							sizeof COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2))
					{
						COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 = (
							COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t*)address;
					}
					else if (!COverlayContext_OverlaysEnabled_orig && sizeof COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2
						<= moduleInfo.SizeOfImage - i && !aob_match_inverse(
							address, COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2,
							sizeof COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2))
					{


						COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)get_relative_address(address, 1, 5);
					}
					if (COverlayContext_Present_orig && COverlayContext_IsCandidateDirectFlipCompatbile_orig &&
						COverlayContext_OverlaysEnabled_orig)
					{
						break;
					}
				}
			}
			else if (isWindows11)
			{
				for (size_t i = 0; i <= moduleInfo.SizeOfImage - sizeof COverlayContext_OverlaysEnabled_bytes_w11; i++)
				{
					unsigned char* address = (unsigned char*)dwmcore + i;
					if (!COverlayContext_Present_orig && sizeof COverlayContext_Present_bytes_w11 <= moduleInfo.
						SizeOfImage - i && !aob_match_inverse(address, COverlayContext_Present_bytes_w11,
						                                      sizeof COverlayContext_Present_bytes_w11))
					{
						COverlayContext_Present_orig = (COverlayContext_Present_t*)address;
						COverlayContext_Present_real_orig = COverlayContext_Present_orig;
					}
					else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig && sizeof
						COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11 <= moduleInfo.SizeOfImage - i && !
						aob_match_inverse(
							address, COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11,
							sizeof COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11))
					{
						COverlayContext_IsCandidateDirectFlipCompatbile_orig = (
							COverlayContext_IsCandidateDirectFlipCompatbile_t*)address;
					}
					else if (!COverlayContext_OverlaysEnabled_orig && sizeof COverlayContext_OverlaysEnabled_bytes_w11
						<= moduleInfo.SizeOfImage - i && !aob_match_inverse(
							address, COverlayContext_OverlaysEnabled_bytes_w11,
							sizeof COverlayContext_OverlaysEnabled_bytes_w11))
					{
						COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)address;
					}
					if (COverlayContext_Present_orig && COverlayContext_IsCandidateDirectFlipCompatbile_orig &&
						COverlayContext_OverlaysEnabled_orig)
					{
						break;
					}
				}

				DWORD rev;
				DWORD revSize = sizeof(rev);
				RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_DWORD,
				             NULL, &rev, &revSize);

				if (rev >= 706)
				{
					// COverlayContext_DeviceClipBox_offset_w11 += 8;
				}
			}
			else
			{
				for (size_t i = 0; i <= moduleInfo.SizeOfImage - sizeof(COverlayContext_Present_bytes); i++)
				{
					unsigned char* address = (unsigned char*)dwmcore + i;
					if (!COverlayContext_Present_orig && !memcmp(address, COverlayContext_Present_bytes,
					                                             sizeof(COverlayContext_Present_bytes)))
					{
						COverlayContext_Present_orig = (COverlayContext_Present_t*)address;
						COverlayContext_Present_real_orig = COverlayContext_Present_orig;
					}
					else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig && !memcmp(
						address, COverlayContext_IsCandidateDirectFlipCompatbile_bytes,
						sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes)))
					{
						static int found = 0;
						found++;
						if (found == 2)
						{
							COverlayContext_IsCandidateDirectFlipCompatbile_orig = (
								COverlayContext_IsCandidateDirectFlipCompatbile_t*)(address - 0xa);
						}
					}
					else if (!COverlayContext_OverlaysEnabled_orig && !memcmp(
						address, COverlayContext_OverlaysEnabled_bytes, sizeof(COverlayContext_OverlaysEnabled_bytes)))
					{
						COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)(address - 0x7);
					}
					if (COverlayContext_Present_orig && COverlayContext_IsCandidateDirectFlipCompatbile_orig &&
						COverlayContext_OverlaysEnabled_orig)
					{
						break;
					}
				}
			}

			char lutFolderPath[MAX_PATH];
			ExpandEnvironmentStringsA(LUT_FOLDER, lutFolderPath, sizeof(lutFolderPath));
			// Fringeless: LUT files are optional. We apply a baked per-channel sub-pixel
			// shift, so load whatever .cube files exist (usually none) and carry on.
			AddLUTs(lutFolderPath);
			// Present is the only hook required to apply a LUT. The DirectFlip and
			// OverlaysEnabled helpers are private DWM implementation details and have
			// changed independently in cumulative updates. Keep them optional on the
			// modern path so an unrelated signature change does not prevent the DLL
			// from loading at all.
			const bool foundPresent = isWindows11_24h2 || isWindows11_25h2
				? COverlayContext_Present_orig_24h2 != NULL
				: COverlayContext_Present_orig != NULL;

			sprintf(startupMessage, "DWM LUT signatures: foundPresent=%d present24=0x%p presentLegacy=0x%p directFlip24=0x%p overlays=0x%p overlayTestMode=0x%p numLuts=%d",
				foundPresent, COverlayContext_Present_orig_24h2, COverlayContext_Present_orig,
				COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2, COverlayContext_OverlaysEnabled_orig,
				g_pOverlayTestMode, numLuts);
			LOG_ONLY_ONCE(startupMessage)

			if (foundPresent)

			{
				if (MH_Initialize() != MH_OK)
				{
					return FALSE;
				}
				if (!isWindows11_24h2 && !isWindows11_25h2)
				{
					if (MH_CreateHook((PVOID)COverlayContext_Present_orig, (PVOID)COverlayContext_Present_hook,
						(PVOID*)&COverlayContext_Present_orig) != MH_OK)
					{
						MH_Uninitialize();
						return FALSE;
					}
				}
				else
				{
					if (MH_CreateHook((PVOID)COverlayContext_Present_orig_24h2, (PVOID)COverlayContext_Present_hook_24h2,
						(PVOID*)&COverlayContext_Present_orig_24h2) != MH_OK)
					{
						MH_Uninitialize();
						return FALSE;
					}
				}

				if (!isWindows11_24h2 && !isWindows11_25h2 &&
					COverlayContext_IsCandidateDirectFlipCompatbile_orig != NULL)
					MH_CreateHook((PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_orig,
								  (PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_hook,
								  (PVOID*)&COverlayContext_IsCandidateDirectFlipCompatbile_orig);
				else if ((isWindows11_24h2 || isWindows11_25h2) &&
					COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 != NULL)
					MH_CreateHook((PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2,
						(PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_hook_24h2,
						(PVOID*)&COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2);

				if (COverlayContext_OverlaysEnabled_orig != NULL)
					MH_CreateHook((PVOID)COverlayContext_OverlaysEnabled_orig, (PVOID)COverlayContext_OverlaysEnabled_hook,
						(PVOID*)&COverlayContext_OverlaysEnabled_orig);

				if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
				{
					MH_Uninitialize();
					return FALSE;
				}
				LOG_ONLY_ONCE("DWM HOOK DLL INITIALIZATION. START LOGGING")

				// Publish a NULL-DACL marker so the (non-elevated) app can tell the engine
				// is loaded without opening dwm.exe.
				{
					SECURITY_DESCRIPTOR sd;
					InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
					SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
					SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
					g_loadedMarker = CreateMutexW(&sa, FALSE, L"Global\\FringelessDwmEngine");
				}

				// Disable DirectFlip/MPO by setting DWM's internal OverlayTestMode to 5
				if (g_pOverlayTestMode != NULL)
				{
					g_originalOverlayTestMode = *g_pOverlayTestMode;
					g_overlayTestModeSaved = true;
					*g_pOverlayTestMode = 5;
					LOG_ONLY_ONCE("Set OverlayTestMode global to 5 in DWM memory")
				}

				break;
			}
			return FALSE;
		}
	case DLL_PROCESS_DETACH:
		if (g_loadedMarker) { CloseHandle(g_loadedMarker); g_loadedMarker = NULL; }
		// Restore OverlayTestMode
		if (g_pOverlayTestMode != NULL && g_overlayTestModeSaved)
		{
			*g_pOverlayTestMode = g_originalOverlayTestMode;
		}
		MH_Uninitialize();
		Sleep(100);
		UninitializeStuff();
		break;
	default:
		break;
	}
	return TRUE;
}
