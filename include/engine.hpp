#pragma once

#include "gamedata.hpp"
#include "filesystem.hpp"
#include <memory>
#include "oddlib/masher.hpp"
#include "SDL.h"
#include <GL/glew.h>
#include "SDL_opengl.h"
#include "core/audiobuffer.hpp"

class Engine
{
public:
    Engine();
    virtual ~Engine();
    virtual bool Init();
    virtual int Run();
private:
    void Update();
    void Render();
    bool InitSDL();
    int LoadNanoVgFonts(struct NVGcontext* vg);
    void InitNanoVg();
    void InitGL();
    void InitImGui();
    void RenderVideoUi();
    void ImGui_WindowResize();
protected:
    virtual void InitSubSystems();
    virtual void DebugRender() { };

    enum eStates
    {
        eStarting,
        eRunning,
        ePlayingFmv,
        eShuttingDown,
    };
    eStates mPreviousState = eStarting;
    eStates mState = eStarting;
    
    void ToState(eStates newState);

    // Audio must init early
    SdlAudioWrapper mAudioHandler;

    FileSystem mFileSystem;
    GameData mGameData;
 
    SDL_Window* mWindow = nullptr;
    SDL_GLContext mContext = nullptr;


    std::unique_ptr<class Fmv> mFmv;
    std::unique_ptr<class Sound> mSound;

    struct NVGLUframebuffer* mNanoVgFrameBuffer = nullptr;
    struct NVGcontext* mNanoVg = nullptr;
};
