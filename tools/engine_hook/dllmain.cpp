#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <vector>
#include "ddraw7proxy.hpp"
#include "detours.h"
#include "logger.hpp"
#include "types.hpp"
#include "oddlib/anim.hpp"
#include "oddlib/stream.hpp"
#include "sound_hooks.hpp"
#include "gamefilesystem.hpp"
#include "hook.hpp"
#include "types.hpp"

// Hack to access private parts of ResourceMapper
#define private public

#include "resourcemapper.hpp"

#undef private

#include "gridmap.hpp"

/*static*/ DirectSurface7Proxy* DirectSurface7Proxy::g_Primary;
/*static*/ DirectSurface7Proxy* DirectSurface7Proxy::g_BackBuffer;
/*static*/ DirectSurface7Proxy* DirectSurface7Proxy::g_FakePrimary;

bool gCollisionsEnabled = true;
bool gGridEnabled = false;

HINSTANCE gDllInstance = NULL;
HRESULT WINAPI NewDirectDrawCreate(GUID* lpGUID, IDirectDraw** lplpDD, IUnknown* pUnkOuter);
using TDirectDrawCreate = decltype(&NewDirectDrawCreate);

static HMODULE LoadRealDDrawDll()
{
    char infoBuf[MAX_PATH] = {};
    ::GetSystemDirectory(infoBuf, MAX_PATH);

    strcat_s(infoBuf, "\\ddraw.dll");

    const HMODULE hRealDll = ::LoadLibrary(infoBuf);
    if (!hRealDll)
    {
        FatalExit("Can't load or find real DDraw.dll");
    }
    return hRealDll;
}

template<class T>
inline T TGetProcAddress(HMODULE hDll, const char* func)
{
    #pragma warning(suppress: 4191)
    return reinterpret_cast<T>(::GetProcAddress(hDll, func));
}

static TDirectDrawCreate GetFunctionPointersToRealDDrawFunctions(HMODULE hRealDll)
{
    auto ptr = TGetProcAddress<TDirectDrawCreate>(hRealDll, "DirectDrawCreate");
    if (!ptr)
    {
        FatalExit("Can't find DirectDrawCreate function in real dll");
    }
    return ptr;
}


// =================
WNDPROC g_pOldProc = 0;

static LRESULT CALLBACK NewWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        abort();
        break;

    case WM_ERASEBKGND:
    {
        RECT rcWin;
        HDC hDC = GetDC(hwnd);
        GetClipBox((HDC)wParam, &rcWin);
        FillRect(hDC, &rcWin, GetSysColorBrush(COLOR_DESKTOP));  // hBrush can be obtained by calling GetWindowLong()
    }
    return TRUE;

    case WM_GETICON:
    case WM_MOUSEACTIVATE:
    case WM_NCLBUTTONDOWN:
    case WM_NCMOUSELEAVE:
    case WM_KILLFOCUS:
    case WM_SETFOCUS:
    case WM_ACTIVATEAPP:
    case WM_NCHITTEST:
    case WM_ACTIVATE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_NCCALCSIZE:
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
    case WM_WINDOWPOSCHANGING:
    case WM_NCMOUSEMOVE:
    case WM_MOUSEMOVE:
        return DefWindowProc(hwnd, message, wParam, lParam);
    case WM_KEYDOWN:
    {
        if (GetAsyncKeyState('G'))
            gGridEnabled = !gGridEnabled;
        if (GetAsyncKeyState('H'))
            gCollisionsEnabled = !gCollisionsEnabled;
        return g_pOldProc(hwnd, message, wParam, lParam);
    }
    case WM_SETCURSOR:
    {
        // Set the cursor so the resize cursor or whatever doesn't "stick"
        // when we move the mouse over the game window.
        static HCURSOR cur = LoadCursor(0, IDC_ARROW);
        if (cur)
        {
            SetCursor(cur);
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);

    }
    return FALSE;
    }

    return g_pOldProc(hwnd, message, wParam, lParam);
}


static void CenterWnd(HWND wnd)
{
    RECT r, r1;
    GetWindowRect(wnd, &r);
    GetWindowRect(GetDesktopWindow(), &r1);
    MoveWindow(wnd, ((r1.right - r1.left) - (r.right - r.left)) / 2,
        ((r1.bottom - r1.top) - (r.bottom - r.top)) / 2,
        (r.right - r.left), (r.bottom - r.top), 0);
}


static void SubClassWindow()
{
    HWND wnd = FindWindow("ABE_WINCLASS", NULL);
    g_pOldProc = (WNDPROC)SetWindowLong(wnd, GWL_WNDPROC, (LONG)NewWindowProc);

    for (int i = 0; i < 70; ++i)
    {
        ShowCursor(TRUE);
    }

    SetWindowLongA(wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);

    RECT rc;
    SetRect(&rc, 0, 0, 640, 460);
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW | WS_VISIBLE, TRUE, 0);
    SetWindowPos(wnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
        SWP_SHOWWINDOW);
        

    ShowWindow(wnd, SW_HIDE);

    CenterWnd(wnd);

    ShowWindow(wnd, SW_SHOW);

    InvalidateRect(GetDesktopWindow(), NULL, TRUE);
}

static void PatchWindowTitle()
{
    HWND wnd = FindWindow("ABE_WINCLASS", NULL);
    if (wnd)
    {
        const int length = GetWindowTextLength(wnd) + 1;
        std::vector< char > titleBuffer(length+1);
        if (GetWindowText(wnd, titleBuffer.data(), length))
        {
            std::string titleStr(titleBuffer.data());
            titleStr += " under ALIVE hook";
            SetWindowText(wnd, titleStr.c_str());
        }
    }
}


typedef int(__cdecl* gdi_draw_hook_type)(DWORD * hdc);
static int __cdecl gdi_draw_hook(DWORD * hdc);

static int __fastcall set_first_camera_hook(void *thisPtr, void*, __int16 a2, __int16 a3, __int16 a4, __int16 a5, __int16 a6, __int16 a7);
typedef int(__thiscall* set_first_camera_thiscall)(void *thisPtr, __int16 a2, __int16 a3, __int16 a4, __int16 a5, __int16 a6, __int16 a7);


#pragma pack(push)
#pragma pack(1)
struct anim_struct
{
    void* mVtbl;
    WORD field_4; // max w?
    WORD field_6; // max h?
    DWORD mFlags;
    WORD field_C;
    WORD field_E;
    WORD field_10;
    WORD field_12;
    DWORD field_14;
    DWORD mAnimationHeaderOffset; // offset to frame table from anim data header
    DWORD field_1C;
    BYTE** mAnimChunkPtrs; // pointer to a pointer which points to anim data?
    DWORD iDbufPtr;
    DWORD iAnimSize;
    DWORD field_2C;
    DWORD field_30;
    DWORD field_34;
    DWORD field_38;
    DWORD field_3C;
    DWORD field_40;
    DWORD field_44;
    DWORD field_48;
    DWORD field_4C;
    DWORD field_50;
    DWORD field_54;
    DWORD field_58;
    DWORD field_5C;
    DWORD field_60;
    DWORD field_64;
    DWORD field_68;
    DWORD field_6C;
    DWORD field_70;
    DWORD field_74;
    DWORD field_78;
    DWORD field_7C;
    DWORD field_80;
    WORD iRect;
    WORD field_86;
    WORD field_88;
    WORD field_8A;
    DWORD field_8C;
    WORD field_90;
    s16 mFrameNum;
};
#pragma pack(pop)
static_assert(sizeof(anim_struct) == 0x94, "Wrong size");

void __fastcall anim_decode_hook(anim_struct* thisPtr, void*);
typedef void (__thiscall* anim_decode_thiscall)(anim_struct* thisPtr);

int __fastcall get_anim_frame_hook(anim_struct* thisPtr, int a2, __int16 a3);

struct CollisionInfo;
typedef int(__thiscall* sub_418930_thiscall)(int thisPtr, const CollisionInfo* pCollisionInfo, u8* pPathBlock);
static int __fastcall sub_418930_hook(int thisPtr, void*, const CollisionInfo* pCollisionInfo, u8* pPathBlock);

namespace Hooks
{
    Hook<decltype(&::SetWindowLongA)> SetWindowLong(::SetWindowLongA);
    Hook<decltype(&::sub_418930_hook), sub_418930_thiscall> sub_418930(0x00418930);
    Hook<decltype(&::set_first_camera_hook), set_first_camera_thiscall> set_first_camera(0x00401415);
    Hook<decltype(&::gdi_draw_hook), gdi_draw_hook_type> gdi_draw(0x004F21F0);
    Hook<decltype(&::anim_decode_hook), anim_decode_thiscall> anim_decode(0x0040AC90);
    Hook<decltype(&::get_anim_frame_hook)> get_anim_frame(0x0040B730);
}

LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
    if (nIndex == GWL_STYLE)
    {
        dwNewLong = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    }
    return Hooks::SetWindowLong.Real()(hWnd, nIndex, dwNewLong);
}

static int __fastcall set_first_camera_hook(void *thisPtr, void* , __int16 levelNumber, __int16 pathNumber, __int16 cameraNumber, __int16 screenChangeEffect, __int16 a6, __int16 a7)
{
    TRACE_ENTRYEXIT;

    // AE Cheat screen, I think we have to fake cheat input or set a bool for this to work :(
    // cameraNumber = 31;

    // Setting to Feco lets us go directly in game, with the side effect that pausing will crash
    // and some other nasties, still its good enough for debugging animations
    //levelNumber = 5;

    // Abe "hello" screen when levelNumber is left as the intro level
    //cameraNumber = 1;

    //pathNumber = 1;

    // 5 = "flash" on
    // 4 = top to bottom
    // 8 = door effect/sound
    screenChangeEffect = 5;

    return Hooks::set_first_camera.Real()(thisPtr, levelNumber, pathNumber, cameraNumber, screenChangeEffect, a6, a7);
}

struct AnimLogger
{
    std::unique_ptr<IFileSystem> mFileSystem;
    std::unique_ptr<ResourceMapper> mResources;

    std::map<u32, std::unique_ptr<Oddlib::AnimSerializer>> mAnimCache;
    std::map<std::pair<u32, u32>, Oddlib::AnimSerializer*> mAnims;

    Oddlib::AnimSerializer* AddAnim(u32 id, u8* ptr, u32 size)
    {
        auto it = mAnimCache.find(id);
        if (it == std::end(mAnimCache))
        {
            std::vector<u8> data(
                reinterpret_cast<u8*>(ptr),
                reinterpret_cast<u8*>(ptr) + size);

            Oddlib::MemoryStream ms(std::move(data));
            mAnimCache[id] = std::make_unique<Oddlib::AnimSerializer>(ms, false);
            return AddAnim(id, ptr, size);
        }
        else
        {
            return it->second.get();
        }
    }

    void Add(u32 id, u32 idx, Oddlib::AnimSerializer* anim)
    {
        auto key = std::make_pair(id, idx);
        auto it = mAnims.find(key);
        if (it == std::end(mAnims))
        {
            mAnims[key] = anim;
        }

        LogAnim(id, idx);
    }

    std::string Find(u32 id, u32 idx)
    {
        for (const auto& mapping : mResources->mAnimMaps)
        {
            for (const auto& location : mapping.second.mLocations)
            {
                if (location.mDataSetName == "AePc")
                {
                    for (const auto& file : location.mFiles)
                    {
                        if (file.mAnimationIndex == idx && file.mId == id)
                        {
                            return mapping.first;
                        }
                    }
                }
            }
        }
        return "";
    }

    void ResourcesInit()
    {
        TRACE_ENTRYEXIT;
        mFileSystem = std::make_unique<GameFileSystem>();
        if (!mFileSystem->Init())
        {
            LOG_ERROR("File system init failure");
        }
        mResources = std::make_unique<ResourceMapper>(*mFileSystem, 
            "../../data/dataset_contents.json",
            "../../data/animations.json",
            "../../data/sounds.json",
            "../../data/paths.json",
            "../../data/fmvs.json");
    }

    std::string mLastResName;

    void LogAnim(u32 id, u32 idx)
    {
        if (GetAsyncKeyState(VK_F2))
        {
            std::cout << "RELOAD RESOURCES" << std::endl;
            mFileSystem = nullptr;
            mResources = nullptr;
        }

        if (!mFileSystem)
        {
            ResourcesInit();
        }

        std::string resName = Find(id, idx);
        if (resName != mLastResName)
        {
            std::string s = "id: " + std::to_string(id) + " idx: " + std::to_string(idx) + " resource name: " + resName;
            std::cout << "ANIM: " << s << std::endl; // use cout directly, don't want the function name etc here
            mLastResName = resName;
        }

        //::Sleep(128);
    }
};

AnimLogger gAnimLogger;

// First 16bits is the whole number, last 16bits is the remainder (??)
class HalfFloat
{
public:
    HalfFloat() = default;
    HalfFloat(s32 value) : mValue(value) { }
    f64 AsDouble() const { return static_cast<double>(mValue) / 65536.0; }
    HalfFloat& operator = (int value) { mValue = value; }
    bool operator != (const HalfFloat& r) const { return mValue != r.mValue; }
private:
    friend HalfFloat operator -(const HalfFloat& l, const HalfFloat& r);
    s32 mValue;
};

inline HalfFloat operator - (const HalfFloat& l, const HalfFloat& r) { return HalfFloat(l.mValue - r.mValue); }

#pragma pack(push)
#pragma pack(4)
struct abe
{

    void *vtable;
    __int16 type;
    char objectMode;
    char field_7;
    char gap8;
    int field_C;
    char *field_10;
    int field_14;
    BYTE gap18[8];
    int field_20;
    char field_24;
    __declspec(align(2)) char field_26;
    __declspec(align(2)) char field_28;
    char gap29;
    __int16 field_2A;
    __int16 field_2C;
    BYTE gap2E[6];
    __int16 field_34;
    BYTE gap36[6];
    int field_3C;
    BYTE gap40[20];
    int field_54;
    __int16 gap58;
    BYTE gap5A[88];
    char field_B2;
    BYTE gapB3[5];
    HalfFloat xpos;
    HalfFloat ypos;
    char field_C0;
    HalfFloat velocity_x;
    HalfFloat velocity_y;
    int scale;
    char color_r;
    __declspec(align(2)) char color_g;
    __declspec(align(2)) char color_b;
    __declspec(align(2)) char layer;
    __int16 sprite_offset_x;
    __int16 sprite_offset_y;
    BYTE gapDC[28];
    int field_F8;
    BYTE gapFC[4];
    int field_100;
    __int16 gap104;
    __int16 alive_state;
    BYTE gap108[4];
    int health;
    BYTE gap110[32];
    int field_130;
    int field_134;
    __int16 field_138;
    BYTE gap13A[48];
    char ring_ability;
    BYTE gap16B[55];
    char rock_count;
    BYTE gap1A3[148];
    __declspec(align(1)) __int16 gap10A;
};
#pragma pack(pop)

abe ** hero = reinterpret_cast<abe**>(0x005C1B68);

void DumpDeltas(anim_struct* thisPtr)
{
    static HalfFloat prevX = 0;
    static HalfFloat prevY = 0;
    static HalfFloat preVX = 0;
    static HalfFloat preVY = 0;

    if (prevX != (*hero)->xpos ||
        prevY != (*hero)->ypos ||
        preVX != (*hero)->velocity_x ||
        preVY != (*hero)->velocity_y)
    {

        printf("XD:%f YD:%f F:%d XV:%f YV:%f\n",
            ((*hero)->xpos - prevX).AsDouble(),
            ((*hero)->ypos - prevY).AsDouble(),
            thisPtr->mFrameNum,
            ((*hero)->velocity_x - preVX).AsDouble(),
            ((*hero)->velocity_y - preVY).AsDouble()
        );
        //system("PAUSE");
    }

    prevX = (*hero)->xpos;
    prevY = (*hero)->ypos;
    preVX = (*hero)->velocity_x;
    preVY = (*hero)->velocity_y;
}

void __fastcall anim_decode_hook(anim_struct* thisPtr, void*)
{
    static anim_struct* pTarget = nullptr;

    DumpDeltas(thisPtr);

    if (thisPtr->mAnimChunkPtrs)
    {

        DWORD* ptr = (DWORD*)*thisPtr->mAnimChunkPtrs;
        DWORD* id = ptr - 1;

        // Comment out pTarget checks to log anims for everything that comes in here
        // with pTarget its limited to abe anims only

        // 303 = dust
        if (*id == 55 && pTarget == nullptr)
        {
            pTarget = thisPtr;
        }

        // 55 index 5 = idle stand or walk
        if (pTarget && thisPtr == pTarget)
        {
            // Force anim index 0
            // thisPtr->mFrameTableOffset = *(ptr + 1);

            // Get anim from cache or add to cache
            Oddlib::AnimSerializer* as = gAnimLogger.AddAnim(
                *id, 
                reinterpret_cast<u8*>(ptr),
                *(ptr - 4) - (sizeof(DWORD) * 4));
           
            // Find out which index this anim is
            bool found = false;
            u32 idx = 0;
            for (const auto& anim : as->Animations())
            {
                if (anim->mOffset == thisPtr->mAnimationHeaderOffset)
                {
                    found = true;
                    break;
                }
                idx++;
            }

            if (found)
            {
                // Log the animation id/index against the parsed/cache animation
                gAnimLogger.Add(
                    *id,
                    idx,
                    as);
            }
        }

    }
  
    Hooks::anim_decode.Real()(thisPtr);
}

int __fastcall get_anim_frame_hook(anim_struct* thisPtr, int a2, __int16 a3)
{
    // loop flag?
    // a3 = 0;
    int ret = Hooks::get_anim_frame.Real()(thisPtr, a2, a3);
    return ret;
}

struct SoundBlockInfo
{
    char* mVabHeaderName;
    char* mVabBodyName;
    unsigned int mVabID;
    void* mVabHead;
};

struct PathRoot
{
    struct PathBlyRecHeader* mBlyArrayPtr;
    void* mFmvArrayPtr;
    SoundBlockInfo* mMusicPtr;
    char* mBsqFileNamePtr; // E.g "FESEQ.BSQ"

    unsigned short int mReverb;
    unsigned short int mBgMusicId; // BG Music seq?
    char* mName; // E.g "FD"

    unsigned short int mNumPaths;
    unsigned short int mUnused;

    unsigned int mUnknown3;
    char* mLvlName; // E.g "\\FD.LVL;1" 
    unsigned int mUnknown4;
    char* mOvlName; // E.g "\\FD.OVL;1" 
    unsigned int mUnknown5;
    char* mMovName; // E.g "\\FD.MOV;1"
    char* mIdxName; // E.g "FD.IDX"
    char* mBndName; // E.g "FDPATH.BND"
};

struct PathData
{
    unsigned short int mBLeft;	// for ao ??
    unsigned short int mBRight; // for ao ??
    unsigned short int mBTop;
    unsigned short int mBBottom;

    unsigned short int mGridWidth;
    unsigned short int mGridHeight;

    unsigned short int mWidth;
    unsigned short int mHeight;

    unsigned int object_offset;
    unsigned int object_indextable_offset;

    unsigned short int mUnknown3; // values offset for ao?
    unsigned short int mUnknown4; // part of the above for ao??

    void* (*mObjectFuncs[256])(void);
};

struct CollisionInfo;
CollisionInfo* CollisionInitFunc(const CollisionInfo *, u8 *);
using TCollisionFunc = decltype(&CollisionInitFunc);

struct CollisionInfo
{
    TCollisionFunc iFuncPtr;

    unsigned short int mBLeft;
    unsigned short int mBRight;
    unsigned short int mBTop;
    unsigned short int mBBottom;

    unsigned int mCollisionOffset;
    unsigned int mNumCollisionItems;
    unsigned int mGridWidth;
    unsigned int mGridHeight;
};

struct PathBlyRec
{
    char* mBlyName;
    PathData* mPathData;
    CollisionInfo* mCollisionData;
    unsigned int mUnused;
};

struct PathBlyRecHeader
{
    // Max number of paths is 99 due to the format string used in the cam loading
    struct PathBlyRec iBlyRecs[99];
};

const int kNumLvls = 17;
struct PathRootData
{
    PathRoot iLvls[17];
};
PathRootData* gPathData = nullptr;
std::unique_ptr<Oddlib::Path> gPath;

static int __fastcall sub_418930_hook(int thisPtr, void*, const CollisionInfo* pCollisionInfo, u8* pPathBlock)
{
    LOG_INFO("Load collision data");

    for (s32 i = 0; i < kNumLvls; i++)
    {
        PathRoot& data = gPathData->iLvls[i];
        for (s32 j = 1; j < data.mNumPaths + 1; j++)
        {
            if (data.mBlyArrayPtr->iBlyRecs[j].mBlyName && data.mBlyArrayPtr->iBlyRecs[j].mCollisionData == pCollisionInfo)
            {
                std::cout << data.mBlyArrayPtr->iBlyRecs[j].mBlyName << std::endl;

                DWORD size = *reinterpret_cast<DWORD*>(pPathBlock - 16); // Back by the size of res header which contains the chunk size
                Oddlib::MemoryStream pathDataStream(std::vector<u8>(pPathBlock, pPathBlock + size));


                PathData& pathData = *data.mBlyArrayPtr->iBlyRecs[j].mPathData;

                gPath = std::make_unique<Oddlib::Path>(
                    pathDataStream, 
                    pCollisionInfo->mCollisionOffset + 16, 
                    pathData.object_indextable_offset + 16,
                    pathData.object_offset + 16,
                    pathData.mBTop / pathData.mWidth,
                    pathData.mBBottom / pathData.mHeight,
                    false);

                return Hooks::sub_418930.Real()(thisPtr, pCollisionInfo, pPathBlock);
            }
        }
    }

    LOG_ERROR("Couldn't load path");
    return Hooks::sub_418930.Real()(thisPtr, pCollisionInfo, pPathBlock);
}


void GetPathArray()
{
    gPathData = reinterpret_cast<PathRootData*>(0x00559660);

    for (s32 i = 0; i < kNumLvls; i++)
    {
        PathRoot& data = gPathData->iLvls[i];
        for (s32 j = 1; j < data.mNumPaths+1; j++)
        {
            if (data.mBlyArrayPtr->iBlyRecs[j].mBlyName && data.mBlyArrayPtr->iBlyRecs[j].mCollisionData->iFuncPtr)
            {
                std::cout << data.mBlyArrayPtr->iBlyRecs[j].mBlyName << std::endl;
            }
        }
    }
}

typedef HDC(__cdecl * ConvertAbeHdcHandle)(DWORD * hdc);
ConvertAbeHdcHandle ddHdcToGdi = reinterpret_cast<ConvertAbeHdcHandle>(0x4F2150);
typedef DWORD(__cdecl * ConvertAbeHdcHandle2)(DWORD * hdc, int hdc2);
ConvertAbeHdcHandle2 gdiHdcToDd = reinterpret_cast<ConvertAbeHdcHandle2>(0x4F21A0);

void GdiLoop(HDC hdc)
{
    std::string text("Abe: X: " + 
        std::to_string((*hero)->xpos.AsDouble()) + 
        " Y: " + std::to_string((*hero)->ypos.AsDouble()) + 
        " Grid: " + std::to_string(gGridEnabled) + 
        " Collisions: " + std::to_string(gCollisionsEnabled) + "(G/H)");

    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, 0);
    SetTextColor(hdc, RGB(255, 0, 0));
    TextOut(hdc, 0, 0, text.c_str(), text.length());

    XFORM xForm;
    xForm.eM11 = (FLOAT) 1.73913;
    xForm.eM12 = (FLOAT) 0.0;
    xForm.eM21 = (FLOAT) 0.0;
    xForm.eM22 = (FLOAT) 1.0;
    xForm.eDx = (FLOAT) 0.0;
    xForm.eDy = (FLOAT) (240.0 + 32.0); // Moves to background buffer

    SetGraphicsMode(hdc, GM_ADVANCED);
    //SetMapMode(hdc, MM_LOENGLISH);
    SetWorldTransform(hdc, &xForm);
    HPEN hPenOld;
    HPEN hLinePen;
    COLORREF qLineColor;
    qLineColor = RGB(255, 255, 255);
    hLinePen = CreatePen(PS_SOLID, 1, qLineColor);
    hPenOld = (HPEN)SelectObject(hdc, hLinePen);

    if (gGridEnabled)
    {
        for (int i = 0; i < (368 / 25) + 1; i++)
        {
            MoveToEx(hdc, i * 25, 0, NULL);
            LineTo(hdc, i * 25, 240);
        }

        for (int i = 0; i < (240 / 20) + 1; i++)
        {
            MoveToEx(hdc, 0, i * 20, NULL);
            LineTo(hdc, 368, i * 20);
        }
    }

    if (gCollisionsEnabled)
    {
        char currentLevelId = *reinterpret_cast<char*>(0x5C3030);
        char * currentLevelName = gPathData->iLvls[currentLevelId].mName;
        char currentPath = *reinterpret_cast<char*>(0x5C3032);
        char currentCam = *reinterpret_cast<char*>(0x5C3034);
        char currentCamBuffer[24];
        sprintf(currentCamBuffer, "%sP%02dC%02d.CAM", currentLevelName, currentPath, currentCam);
        if (gPathData)
        {
            int camX = 0;
            int camY = 0;

            for (auto x = 0u; x < gPath->XSize(); x++)
            {
                for (auto y = 0u; y < gPath->YSize(); y++)
                {
                    const Oddlib::Path::Camera screen = gPath->CameraByPosition(x, y);


                    if (screen.mName == std::string(currentCamBuffer))
                    {
                        camX = x;
                        camY = y;
                        goto camPosFound;
                    }
                }
            }
        camPosFound:

            int camRoomSizeX = 375;
            int camRoomSizeY = 260;

            int renderOffsetX = camRoomSizeX * camX;
            int renderOffsetY = camRoomSizeY * camY;

            for (auto collision : gPath->mCollisionItems)
            {
                int p1x = collision.mP1.mX - renderOffsetX;
                int p1y = collision.mP1.mY - renderOffsetY;
                int p2x = collision.mP2.mX - renderOffsetX;
                int p2y = collision.mP2.mY - renderOffsetY;

                if (p1x < 0 && p1y < 0 && p2x < 0 && p2y < 0 && p1x > 368 && p1y > 240 && p2x > 368 && p2y > 240)
                    continue;

                p1x = glm::clamp(p1x, 0, 368);
                p2x = glm::clamp(p2x, 0, 368);

                p1y = glm::clamp(p1y, 0, 240);
                p2y = glm::clamp(p2y, 0, 240);

                DeleteObject(hLinePen);

                const auto type = CollisionLine::ToType(collision.mType);
                const auto it = CollisionLine::mData.find(type);

                qLineColor = RGB(it->second.mColour.r, it->second.mColour.g, it->second.mColour.b);

                hLinePen = CreatePen(PS_SOLID, 1, qLineColor);
                hPenOld = (HPEN)SelectObject(hdc, hLinePen);

                MoveToEx(hdc, p1x, p1y, NULL);
                LineTo(hdc, p2x, p2y);
            }
        }
    }

    SelectObject(hdc, hPenOld);
    DeleteObject(hLinePen);
}

static int __cdecl gdi_draw_hook(DWORD * hdcPtr)
{
    HDC hdc = ddHdcToGdi(hdcPtr);
    GdiLoop(hdc);
    gdiHdcToDd(hdcPtr, (int)hdc);
    
    return Hooks::gdi_draw.Real()(hdcPtr);
}

int __cdecl AbeSnap_sub_449930_hook(int scale, const signed int xpos)
{
    int result = 0;
    int v3 = 0;
    int v4 = 0;

    if (scale == 32768)
    {
        v4 = (xpos % 375 - 6) % 13;
        if (v4 >= 7)
            result = xpos - v4 + 13;
        else
            result = xpos - v4;
    }
    else
    {
        if (scale == 65536)
        {
            v3 = (xpos - 12) % 25;
            if (v3 >= 13)
                result = xpos - v3 + 25;
            else
                result = xpos - v3;

            LOG_INFO("SNAP: " << xpos << " to " << result);
        }
        else
        {
            result = xpos;
        }
    }
    return result;
}

namespace Hooks
{
    Hook<decltype(&::AbeSnap_sub_449930_hook)> AbeSnap_sub_449930(0x00449930);
}

void HookMain()
{
    TRACE_ENTRYEXIT;

    Hooks::SetWindowLong.Install(Hook_SetWindowLongA);
    Hooks::set_first_camera.Install(set_first_camera_hook);
    Hooks::gdi_draw.Install(gdi_draw_hook);
    Hooks::anim_decode.Install(anim_decode_hook);
    Hooks::get_anim_frame.Install(get_anim_frame_hook);
    Hooks::sub_418930.Install(sub_418930_hook);
    Hooks::sub_418930.Install(sub_418930_hook);
    Hooks::AbeSnap_sub_449930.Install(AbeSnap_sub_449930_hook);
    InstallSoundHooks();

    SubClassWindow();
    PatchWindowTitle();

    GetPathArray();

    //Enable DDCHEAT
    *reinterpret_cast<bool*>(0x005CA4B5) = true; // DDCheat Enabled
    *reinterpret_cast<bool*>(0x005BC000) = true; // Always Render
}

// Proxy DLL entry point
HRESULT WINAPI NewDirectDrawCreate(GUID* lpGUID, IDirectDraw** lplpDD, IUnknown* pUnkOuter)
{
    const HMODULE hDDrawDll = LoadRealDDrawDll();
    const TDirectDrawCreate pRealDirectDrawCreate = GetFunctionPointersToRealDDrawFunctions(hDDrawDll);
    const HRESULT ret = pRealDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
    if (SUCCEEDED(ret))
    {
        *lplpDD = new DirectDraw7Proxy(*lplpDD);
        HookMain();
    }
    return ret;
}

static bool gConsoleDone = false;

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (!gConsoleDone)
    {
        gConsoleDone = true;
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        SetConsoleTitle("ALIVE hook debug console");
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
    }

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        gDllInstance = hModule;
    }
    return TRUE;
}
