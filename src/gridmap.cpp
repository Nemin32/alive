#include "gridmap.hpp"
#include "oddlib/lvlarchive.hpp"
#include "abstractrenderer.hpp"
#include "oddlib/path.hpp"
#include "oddlib/bits_factory.hpp"
#include "logger.hpp"
#include <cassert>
#include "oddlib/sdl_raii.hpp"
#include <algorithm> // min/max
#include <cmath>
#include "resourcemapper.hpp"
#include "engine.hpp"
#include "gamemode.hpp"
#include "editormode.hpp"
#include "fmv.hpp"
#include "sound.hpp"

Level::Level(Sound& sound, IAudioController& audioController, ResourceLocator& locator, AbstractRenderer& rend)
    : mLocator(locator)
{

    // Debugging - reload path and load next path
    static std::string currentPathName;
    static s32 nextPathIndex;

    Debugging().fnLoadPath = [&](const char* name)
    {
        std::unique_ptr<Oddlib::Path> path = mLocator.LocatePath(name);
        if (path)
        {
            if (!mMap)
            {
                mMap = std::make_unique<GridMap>(audioController, mLocator);
            }
            mMap->LoadMap(*path, mLocator, rend);
            currentPathName = name;
        }
        else
        {
            LOG_ERROR("LVL or file in LVL not found");
        }
    };

    Debugging().mFnNextPath = [&]() 
    {
        s32 idx = 0;
        for (const auto& pathMap : mLocator.mResMapper.mPathMaps)
        {
            if (idx == nextPathIndex)
            {
                std::unique_ptr<Oddlib::Path> path = mLocator.LocatePath(pathMap.first.c_str());
                if (path)
                {
                    if (!mMap)
                    {
                        mMap = std::make_unique<GridMap>(audioController, mLocator);
                    }
                    mMap->LoadMap(*path, mLocator, rend);

                    currentPathName = pathMap.first;
                    nextPathIndex = idx +1;
                    if (nextPathIndex > static_cast<s32>(mLocator.mResMapper.mPathMaps.size()))
                    {
                        nextPathIndex = 0;
                    }

                    sound.SetTheme(pathMap.second.mMusicTheme.c_str());
                    sound.HandleEvent("BASE_LINE");
                }
                else
                {
                    LOG_ERROR("LVL or file in LVL not found");
                }
                return;
            }
            idx++;
        }
    };

    Debugging().mFnReloadPath = [&]()
    {
        if (!currentPathName.empty())
        {
            std::unique_ptr<Oddlib::Path> path = mLocator.LocatePath(currentPathName.c_str());
            if (path)
            {
                if (!mMap)
                {
                    mMap = std::make_unique<GridMap>(audioController, mLocator);
                }
                mMap->LoadMap(*path, mLocator, rend);
            }
            else
            {
                LOG_ERROR("LVL or file in LVL not found");
            }
        }
    };
}

void Level::EnterState()
{

}

void Level::Update(const InputState& input, CoordinateSpace& coords)
{
    if (Debugging().mBrowserUi.levelBrowserOpen)
    {
        RenderDebugPathSelection();
    }

    if (mMap)
    {
        mMap->Update(input, coords);
    }
}

void Level::Render(AbstractRenderer& rend)
{
    if (mMap)
    {
        mMap->Render(rend);
    }
}

void Level::RenderDebugPathSelection()
{
    if (ImGui::Begin("Paths"))
    {
        for (const auto& pathMap : mLocator.mResMapper.mPathMaps)
        {
            if (ImGui::Button(pathMap.first.c_str()))
            {
                Debugging().fnLoadPath(pathMap.first.c_str());
            }
        }
    }
    ImGui::End();
}

GridScreen::GridScreen(const Oddlib::Path::Camera& camera, AbstractRenderer& rend, ResourceLocator& locator)
    : mFileName(camera.mName)
    , mCamera(camera)
    , mLocator(locator)
    , mRend(rend)
{
   
}

GridScreen::~GridScreen()
{
    if (mTexHandle.IsValid())
    {
        mRend.DestroyTexture(mTexHandle);
    }
    if (mTexHandle2.IsValid())
    {
        mRend.DestroyTexture(mTexHandle2);
    }
}

void GridScreen::LoadTextures()
{
    if (!mTexHandle.IsValid())
    {
        mCam = mLocator.LocateCamera(mFileName.c_str());
        if (mCam) // One path trys to load BRP08C10.CAM which exists in no data sets anywhere!
        {
            SDL_Surface* surf = mCam->GetSurface();
            mTexHandle = mRend.CreateTexture(AbstractRenderer::eTextureFormats::eRGB, surf->w, surf->h, AbstractRenderer::eTextureFormats::eRGB, surf->pixels, true);

            if (!mTexHandle2.IsValid())
            {
                if (mCam->GetFg1())
                {
                    SDL_Surface* fg1Surf = mCam->GetFg1()->GetSurface();
                    if (fg1Surf)
                    {
                        mTexHandle2 = mRend.CreateTexture(AbstractRenderer::eTextureFormats::eRGBA, fg1Surf->w, fg1Surf->h, AbstractRenderer::eTextureFormats::eRGBA, fg1Surf->pixels, true);
                    }
                }
            }
        }
    }
}

bool GridScreen::hasTexture() const
{
    bool onlySpaces = true;
    for (size_t i = 0; i < mFileName.size(); ++i) 
    {
        if (mFileName[i] != ' ' && mFileName[i] != '\0')
        {
            onlySpaces = false;
            break;
        }
    }
    return !onlySpaces;
}

void GridScreen::Render(float x, float y, float w, float h)
{
    LoadTextures();
    if (mTexHandle.IsValid())
    {
        mRend.TexturedQuad(mTexHandle, x, y, w, h, AbstractRenderer::eForegroundLayer0, ColourU8{ 255, 255, 255, 255 });
    }

    if (mTexHandle2.IsValid())
    {
        mRend.TexturedQuad(mTexHandle2, x, y, w, h, AbstractRenderer::eForegroundLayer1, ColourU8{ 255, 255, 255, 255 });
    }
}

/*static*/ void GridMap::RegisterScriptBindings()
{
    Sqrat::Class<IMap, Sqrat::NoConstructor<IMap>> im(Sqrat::DefaultVM::Get(), "IMap");
    Sqrat::RootTable().Bind("IMap", im);

    Sqrat::DerivedClass<GridMap, IMap, Sqrat::NoConstructor<GridMap>> gm(Sqrat::DefaultVM::Get(), "GridMap");

    gm.Func("GetMapObject", &GridMap::GetMapObject);

    Sqrat::RootTable().Bind("GridMap", gm);
}

GridMap::GridMap(IAudioController& audioController, ResourceLocator& locator)
    : mScriptInstance("gMap", this)
{
    mEditorMode = std::make_unique<EditorMode>(mMapState);
    mGameMode = std::make_unique<GameMode>(mMapState);

    mFmv = std::make_unique<Fmv>(audioController, locator);

    // Size of the screen you see during normal game play, this is always less the the "block" the camera image fits into
    mMapState.kVirtualScreenSize = glm::vec2(368.0f, 240.0f);
}

void GridMap::LoadMap(Oddlib::Path& path, ResourceLocator& locator, AbstractRenderer& rend)
{
    // Clear out existing objects from previous map
    mMapState.mObjs.clear();
    mMapState.mCollisionItems.clear();
    mEditorMode->OnMapChanged();

    // The "block" or grid square that a camera fits into, it never usually fills the grid
    mMapState.kCameraBlockSize = (path.IsAo()) ? glm::vec2(1024, 480) : glm::vec2(375, 260);
    mMapState.kCamGapSize = (path.IsAo()) ? glm::vec2(1024, 480) : glm::vec2(375, 260);

    // Since the camera won't fill a block it can be offset so the camera image is in the middle
    // of the block or else where.
    mMapState.kCameraBlockImageOffset = (path.IsAo()) ? glm::vec2(257, 114) : glm::vec2(0, 0);

    ConvertCollisionItems(path.CollisionItems());

    mMapState.mScreens.resize(path.XSize());
    for (auto& col : mMapState.mScreens)
    {
        col.resize(path.YSize());
    }

    for (u32 x = 0; x < path.XSize(); x++)
    {
        for (u32 y = 0; y < path.YSize(); y++)
        {
            mMapState.mScreens[x][y] = std::make_unique<GridScreen>(path.CameraByPosition(x, y), rend, locator);
        }
    }

    SquirrelVm::CompileAndRun(locator, "object_factory.nut");
    Sqrat::Function objFactoryInit(Sqrat::RootTable(), "init_object_factory");
    objFactoryInit.Execute();
    SquirrelVm::CheckError();

    SquirrelVm::CompileAndRun(locator, "map.nut");

    // Load objects
    for (auto x = 0u; x < mMapState.mScreens.size(); x++)
    {
        for (auto y = 0u; y < mMapState.mScreens[x].size(); y++)
        {
            GridScreen* screen = mMapState.mScreens[x][y].get();
            const Oddlib::Path::Camera& cam = screen->getCamera();
            for (size_t i = 0; i < cam.mObjects.size(); ++i)
            {
                const Oddlib::Path::MapObject& obj = cam.mObjects[i];
                Oddlib::MemoryStream ms(std::vector<u8>(obj.mData.data(), obj.mData.data() + obj.mData.size()));
                const ObjRect rect =
                {
                    obj.mRectTopLeft.mX,
                    obj.mRectTopLeft.mY,
                    obj.mRectBottomRight.mX - obj.mRectTopLeft.mX,
                    obj.mRectBottomRight.mY - obj.mRectTopLeft.mY
                };

                auto tmp = std::make_unique<MapObject>(locator, rect);


                Sqrat::Function objFactory(Sqrat::RootTable(), "object_factory");
                Oddlib::IStream* s = &ms; // Script only knows about IStream, not the derived types
                Sqrat::SharedPtr<bool> ret = objFactory.Evaluate<bool>(tmp.get(), this, path.IsAo(), obj.mType, rect, s); // TODO: Don't need to pass rect?
                SquirrelVm::CheckError();
                if (ret.get() && *ret)
                {
                    tmp->Init();
                    mMapState.mObjs.push_back(std::move(tmp));
                }
            }
        }
    }

    // TODO: Need to figure out what the right way to figure out where abe goes is
    // HACK: Place the player in the first screen that isn't blank
    for (auto x = 0u; x < mMapState.mScreens.size(); x++)
    {
        for (auto y = 0u; y < mMapState.mScreens[x].size(); y++)
        {
            GridScreen *screen = mMapState.mScreens[x][y].get();
            if (screen->hasTexture())
            {

                auto xPos = (x * mMapState.kCamGapSize.x) + 100.0f;
                auto yPos = (y * mMapState.kCamGapSize.y) + 100.0f;

                auto tmp = std::make_unique<MapObject>(locator, ObjRect{});

                Sqrat::Function onInitMap(Sqrat::RootTable(), "on_init_map");
                Sqrat::SharedPtr<bool> ret = onInitMap.Evaluate<bool>(tmp.get(), this, xPos, yPos);
                SquirrelVm::CheckError();
                if (ret.get() && *ret)
                {
                    tmp->Init();
                    tmp->SnapXToGrid(); // Ensure player is locked to grid
                    mMapState.mCameraSubject = tmp.get();
                    mMapState.mObjs.push_back(std::move(tmp));
                    return;
                }
            }
        }
    }
}

GridMap::~GridMap()
{
    TRACE_ENTRYEXIT;
}

void GridMap::Update(const InputState& input, CoordinateSpace& coords)
{
    mFmv->Update();

    if (mFmv->IsPlaying())
    {
        if (input.Mapping().GetActions().mIsPressed)
        {
            LOG_INFO("Stopping FMV due to key press");
            mFmv->Stop();
        }
    }
    else if (mMapState.mState == GridMapState::eStates::eEditor)
    {
        mEditorMode->Update(input, coords);
    }
    else if (mMapState.mState == GridMapState::eStates::eInGame)
    {
        mGameMode->Update(input, coords);
    }
    else
    {
        UpdateToEditorOrToGame(input, coords);
    }
}

void GridMap::UpdateToEditorOrToGame(const InputState& input, CoordinateSpace& coords)
{
    std::ignore = input;

    if (mMapState.mState == GridMapState::eStates::eToEditor)
    {
        coords.SetScreenSize(glm::vec2(coords.Width(), coords.Height()) * mEditorMode->mEditorCamZoom);
        if (SDL_TICKS_PASSED(SDL_GetTicks(), mMapState.mModeSwitchTimeout))
        {
            mMapState.mState = GridMapState::eStates::eEditor;
        }
    }
    else if (mMapState.mState == GridMapState::eStates::eToGame)
    {
        coords.SetScreenSize(mMapState.kVirtualScreenSize);
        if (SDL_TICKS_PASSED(SDL_GetTicks(), mMapState.mModeSwitchTimeout))
        {
            mMapState.mState = GridMapState::eStates::eInGame;
        }
    }
    coords.SetCameraPosition(mMapState.mCameraPosition);
}

MapObject* GridMap::GetMapObject(s32 x, s32 y, const char* type)
{
    for (auto& obj : mMapState.mObjs)
    {
        if (obj->Name() == type)
        {
            if (obj->ContainsPoint(x, y))
            {
                return obj.get();
            }
        }
    }
    return nullptr;
}

void GridMapState::RenderGrid(AbstractRenderer& rend) const
{
    const int gridLineCountX = static_cast<int>((rend.ScreenSize().x / mEditorGridSizeX));
    for (int x = -gridLineCountX; x < gridLineCountX; x++)
    {
        const glm::vec2 worldPos(rend.CameraPosition().x + (x * mEditorGridSizeX) - (static_cast<int>(rend.CameraPosition().x) % mEditorGridSizeX), 0);
        const glm::vec2 screenPos = rend.WorldToScreen(worldPos);
        rend.Line(ColourU8{ 255, 255, 255, 30 }, screenPos.x, 0, screenPos.x, static_cast<f32>(rend.Height()), 2.0f, AbstractRenderer::eLayers::eEditor, AbstractRenderer::eNormal, AbstractRenderer::eScreen);
    }

    const int gridLineCountY = static_cast<int>((rend.ScreenSize().y / mEditorGridSizeY));
    for (int y = -gridLineCountY; y < gridLineCountY; y++)
    {
        const glm::vec2 screenPos = rend.WorldToScreen(glm::vec2(0, rend.CameraPosition().y + (y * mEditorGridSizeY) - (static_cast<int>(rend.CameraPosition().y) % mEditorGridSizeY)));
        rend.Line(ColourU8{ 255, 255, 255, 30 }, 0, screenPos.y, static_cast<f32>(rend.Width()), screenPos.y, 2.0f, AbstractRenderer::eLayers::eEditor, AbstractRenderer::eNormal, AbstractRenderer::eScreen);
    }
}

void GridMapState::RenderDebug(AbstractRenderer& rend) const
{
    //rend.SetActiveLayer(AbstractRenderer::eEditor);

    // Draw collisions
    if (Debugging().mCollisionLines)
    {
        CollisionLine::Render(rend, mCollisionItems);
    }

    // Draw grid
    if (Debugging().mGrid)
    {
        RenderGrid(rend);
    }

    // Draw objects
    if (Debugging().mObjectBoundingBoxes)
    {
        for (auto x = 0u; x < mScreens.size(); x++)
        {
            for (auto y = 0u; y < mScreens[x].size(); y++)
            {
                GridScreen* screen = mScreens[x][y].get();
                const Oddlib::Path::Camera& cam = screen->getCamera();
                for (size_t i = 0; i < cam.mObjects.size(); ++i)
                {
                    const Oddlib::Path::MapObject& obj = cam.mObjects[i];

                    glm::vec2 topLeft = glm::vec2(obj.mRectTopLeft.mX, obj.mRectTopLeft.mY);
                    glm::vec2 bottomRight = glm::vec2(obj.mRectBottomRight.mX, obj.mRectBottomRight.mY);

                    glm::vec2 objPos = rend.WorldToScreen(glm::vec2(topLeft.x, topLeft.y));
                    glm::vec2 objSize = rend.WorldToScreen(glm::vec2(bottomRight.x, bottomRight.y)) - objPos;
                   
                    rend.Rect(
                        objPos.x, objPos.y,
                        objSize.x, objSize.y,
                        AbstractRenderer::eLayers::eEditor, ColourU8{ 255, 255, 255, 255 }, AbstractRenderer::eNormal, AbstractRenderer::eScreen);
                   
                }
            }
        }
    }
}

void GridMap::RenderToEditorOrToGame(AbstractRenderer& rend) const
{
    // TODO: Better transition
    // Keep everything rendered for now
    mEditorMode->Render(rend);
}

void GridMapState::DebugRayCast(AbstractRenderer& rend, const glm::vec2& from, const glm::vec2& to, u32 collisionType, const glm::vec2& fromDrawOffset) const
{
    if (Debugging().mRayCasts)
    {
        Physics::raycast_collision collision;
        if (CollisionLine::RayCast<1>(mCollisionItems, from, to, { collisionType }, &collision))
        {
            const glm::vec2 fromDrawPos = rend.WorldToScreen(from + fromDrawOffset);
            const glm::vec2 hitPos = rend.WorldToScreen(collision.intersection);

            rend.Line(ColourU8{ 255, 0, 255, 255 },
                fromDrawPos.x, fromDrawPos.y,
                hitPos.x, hitPos.y,
                2.0f,
                AbstractRenderer::eLayers::eEditor,
                AbstractRenderer::eBlendModes::eNormal,
                AbstractRenderer::eCoordinateSystem::eScreen);
        }
    }
}

static CollisionLine* GetCollisionIndexByIndex(CollisionLines& lines, s16 index)
{
    const s32 count = static_cast<s32>(lines.size());
    if (index > 0)
    {
        if (index < count)
        {
            return lines[index].get();
        }
        else
        {
            LOG_ERROR("Link index is out of bounds: " << index);
        }
    }
    return nullptr;
}

static void ConvertLink(CollisionLines& lines, const Oddlib::Path::Links& oldLink, CollisionLine::Link& newLink)
{
    newLink.mPrevious = GetCollisionIndexByIndex(lines, oldLink.mPrevious);
    newLink.mNext = GetCollisionIndexByIndex(lines, oldLink.mNext);
}

void GridMap::ConvertCollisionItems(const std::vector<Oddlib::Path::CollisionItem>& items)
{
    const s32 count = static_cast<s32>(items.size());
    mMapState.mCollisionItems.resize(count);

    // First pass to create/convert from original/"raw" path format
    for (auto i = 0; i < count; i++)
    {
        mMapState.mCollisionItems[i] = std::make_unique<CollisionLine>();
        mMapState.mCollisionItems[i]->mLine.mP1.x = items[i].mP1.mX;
        mMapState.mCollisionItems[i]->mLine.mP1.y = items[i].mP1.mY;

        mMapState.mCollisionItems[i]->mLine.mP2.x = items[i].mP2.mX;
        mMapState.mCollisionItems[i]->mLine.mP2.y = items[i].mP2.mY;

        mMapState.mCollisionItems[i]->mType = CollisionLine::ToType(items[i].mType);
    }

    // Second pass to set up raw pointers to existing lines for connected segments of 
    // collision lines
    for (auto i = 0; i < count; i++)
    {
        // TODO: Check if optional link is ever used in conjunction with link
        ConvertLink(mMapState.mCollisionItems, items[i].mLinks[0], mMapState.mCollisionItems[i]->mLink);
        ConvertLink(mMapState.mCollisionItems, items[i].mLinks[1], mMapState.mCollisionItems[i]->mOptionalLink);
    }

    // Now we can re-order collision items without breaking prev/next links, thus we want to ensure
    // that anything that either has no links, or only a single prev/next links is placed first
    // so that we can render connected segments from the start or end.
    std::sort(std::begin(mMapState.mCollisionItems), std::end(mMapState.mCollisionItems), [](std::unique_ptr<CollisionLine>& a, std::unique_ptr<CollisionLine>& b)
    {
        return std::tie(a->mLink.mNext, a->mLink.mPrevious) < std::tie(b->mLink.mNext, b->mLink.mPrevious);
    });

    // Ensure that lines link together physically
    for (auto i = 0; i < count; i++)
    {
        // Some walls have next links, overlapping the walls will break them
        if (mMapState.mCollisionItems[i]->mLink.mNext && mMapState.mCollisionItems[i]->mType == CollisionLine::eTrackLine)
        {
            mMapState.mCollisionItems[i]->mLine.mP2 = mMapState.mCollisionItems[i]->mLink.mNext->mLine.mP1;
        }
    }

    // TODO: Render connected segments as one with control points
}

void GridMap::Render(AbstractRenderer& rend) const
{
    mFmv->Render(rend);
    if (!mFmv->IsPlaying())
    {
        if (mMapState.mState == GridMapState::eStates::eEditor)
        {
            mEditorMode->Render(rend);
        }
        else if (mMapState.mState == GridMapState::eStates::eInGame)
        {
            mGameMode->Render(rend);
        }
        else
        {
            RenderToEditorOrToGame(rend);
        }
    }
}
