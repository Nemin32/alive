#include "SDL.h"
#include "oddlib/stream.hpp"
#include "oddlib/lvlarchive.hpp"
#include "oddlib/bits_factory.hpp"
#include "oddlib/ao_bits_pc.hpp"
#include "oddlib/ae_bits_pc.hpp"
#include "oddlib/psx_bits.hpp"
#include "oddlib/anim.hpp"
#include "filesystem.hpp"
#include "logger.hpp"
#include "stdthread.h"
#include "msvc_sdl_link.hpp"
#include "gamedata.hpp"
#include <cassert>
#include "jsonxx/jsonxx.h"
#include "resourcemapper.hpp"

enum eDataSetType
{
    eAoPc,
    eAoPcDemo,
    eAoPsx,
    eAoPsxDemo,
    eAePc,
    eAePcDemo,
    eAePsxCd1,
    eAePsxCd2,
    eAePsxDemo
};

bool IsPsx(eDataSetType type)
{
    switch (type)
    {
    case eAoPc:
    case eAoPcDemo:
    case eAePc:
    case eAePcDemo:
        return false;

    case eAoPsx:
    case eAoPsxDemo:
    case eAePsxCd1:
    case eAePsxCd2:
    case eAePsxDemo:
        return true;
    }
    abort();
}

const char* ToString(eDataSetType type)
{
    switch (type)
    {
    case eAoPc:
        return "AoPc";

    case eAoPcDemo:
        return "AoPcDemo";

    case eAoPsx:
        return "AoPsx";

    case eAoPsxDemo:
        return "AoPsxDemo";

    case eAePc:
        return "AePc";

    case eAePcDemo:
        return "AePcDemo";

    case eAePsxCd1:
        return "AePsxCd1";

    case eAePsxCd2:
        return "AePsxCd2";

    case eAePsxDemo:
        return "AePsxDemo";
    }
    abort();
}

struct LvlFileChunk
{
    eDataSetType mDataSet;
    std::string mLvlName;
    std::string mFileName;
    Oddlib::LvlArchive::FileChunk* mChunk;
    std::vector<Uint8> mData;
};

struct DeDuplicatedLvlChunk
{
    std::unique_ptr<LvlFileChunk> mChunk;
    std::unique_ptr<Oddlib::AnimationSet> mAnimSet;
    std::vector<std::unique_ptr<LvlFileChunk>> mDuplicates;
};

struct DeDuplicatedAnimation
{
    const Oddlib::Animation* mAnimation;
    Uint32 mAnimationIndex;
    DeDuplicatedLvlChunk* mContainingChunk;
    std::vector<DeDuplicatedLvlChunk*> mDuplicates;
};

bool ChunksAreEqual(const LvlFileChunk& c1, const LvlFileChunk& c2)
{
    // Here just compare content and not type or id, since 2 chunks may have the same data
    // but have changed ID's across games
    return c1.mData == c2.mData;
}

class LvlFileChunkReducer
{
public:
    LvlFileChunkReducer(const LvlFileChunkReducer&) = delete;
    LvlFileChunkReducer& operator = (const LvlFileChunkReducer&) = delete;
    LvlFileChunkReducer() = default;

    void MergeReduce(IFileSystem& parentFs, const std::string& resourcePath, const std::vector<std::string>& lvlFiles, eDataSetType dataSet)
    {
        auto fs = IFileSystem::Factory(parentFs, resourcePath);
        if (!fs->Init())
        {
            throw std::runtime_error("FS init failed");
        }

        for (const auto& lvl : lvlFiles)
        {
            LOG_INFO("Opening LVL: " << lvl);
            auto stream = fs->Open(lvl);
            if (stream)
            {
                MergeReduce(std::make_unique<Oddlib::LvlArchive>(std::move(stream)), lvl, dataSet);
            }
            else
            {
                LOG_WARNING("LVL not found: " << lvl);
                abort();
            }
        }
    }

    std::vector<std::unique_ptr<DeDuplicatedLvlChunk>>& UniqueChunks() { return mDeDuplicatedLvlFileChunks; }

private:
    void MergeReduce(std::unique_ptr<Oddlib::LvlArchive> lvl, const std::string& lvlName, eDataSetType dataSet)
    {
        for (auto i = 0u; i < lvl->FileCount(); i++)
        {
            auto file = lvl->FileByIndex(i);
            if (file->FileName() != "ABEBLOW.BAN") // Limit testing to this 1 file for now
            {
                continue;
            }

            for (auto j = 0u; j < file->ChunkCount(); j++)
            {
                auto chunk = file->ChunkByIndex(j);
                if (chunk->Type() == Oddlib::MakeType('A', 'n', 'i', 'm')) // Only care about Anim resources at the moment
                {
                    bool deDuplicatedChunkAlreadyExists = false;

                    // Read this chunk only once
                    auto chunkInfo = std::make_unique<LvlFileChunk>();
                    chunkInfo->mChunk = chunk;
                    chunkInfo->mDataSet = dataSet;
                    chunkInfo->mFileName = file->FileName();
                    chunkInfo->mLvlName = lvlName;
                    chunkInfo->mData = chunk->ReadData();

                    // Find if this chunk exists
                    for (std::unique_ptr<DeDuplicatedLvlChunk>& deDuplicatedChunk : mDeDuplicatedLvlFileChunks)
                    {
                        if (ChunksAreEqual(*deDuplicatedChunk->mChunk, *chunkInfo))
                        {
                            // Since it exists add the chunk as a duplicate
                            deDuplicatedChunkAlreadyExists = true;
                            deDuplicatedChunk->mDuplicates.push_back(std::move(chunkInfo));
                            deDuplicatedChunk->mDuplicates.back()->mData = std::vector<Uint8>(); // Don't keep many copies of the same buffer
                            break;
                        }
                    }

                    if (!deDuplicatedChunkAlreadyExists)
                    {
                        // Otherwise add it as a unique chunk
                        auto deDuplicatedChunk = std::make_unique<DeDuplicatedLvlChunk>();
                        deDuplicatedChunk->mChunk = std::move(chunkInfo);
                        mDeDuplicatedLvlFileChunks.push_back(std::move(deDuplicatedChunk));
                    }
                }
            }
        }

        mLvls.push_back(std::move(lvl));
    }

    // Open LVLS (required because chunks must have the parent lvl in scope)
    std::vector<std::unique_ptr<Oddlib::LvlArchive>> mLvls;

    // List of unique lvl chunks with links to its duplicates
    std::vector<std::unique_ptr<DeDuplicatedLvlChunk>> mDeDuplicatedLvlFileChunks;
};

/*
class DataTest
{
public:
    DataTest(IFileSystem& fs, eDataSetType eType, const std::string& resourcePath, const std::vector<std::string>& lvls, eDataSetType dataSet)
        : mType(eType), 
          mReducer(fs, resourcePath, lvls, dataSet)
    {
        //ReadAllAnimations();
        //ReadFg1s();
        //ReadFonts();
        //ReadAllPaths();
        //ReadAllCameras();
        // TODO: Handle sounds/fmvs
    }

 
    void ForChunksOfType(Uint32 type, std::function<void(const std::string&, Oddlib::LvlArchive::FileChunk&, bool)> cb)
    {
        for (auto chunkPair : mReducer.Chunks())
        {
            auto chunk = chunkPair.first;
            auto fileName = chunkPair.second.first;
            bool isPsx = chunkPair.second.second;
            if (chunk->Type() == type)
            {
                cb(fileName, *chunk, isPsx);
            }
        }
    }

    void ReadFg1s()
    {
        ForChunksOfType(Oddlib::MakeType('F', 'G', '1', ' '), [&](const std::string&, Oddlib::LvlArchive::FileChunk&, bool)
        {
            // TODO: FG1 parsing
        });
    }

    void ReadFonts()
    {
        ForChunksOfType(Oddlib::MakeType('F', 'o', 'n', 't'), [&](const std::string&, Oddlib::LvlArchive::FileChunk&, bool)
        {
            // TODO: Font parsing
        });
    }
    */

    /*
    void ReadAllPaths()
    {
        ForChunksOfType(Oddlib::MakeType('P', 'a', 't', 'h'), [&](const std::string&, Oddlib::LvlArchive::FileChunk&, bool)
        {
            // TODO: Load the game data json for the required hard coded data to load the path
            Oddlib::Path path(*chunk.Stream(),
                entry->mCollisionDataOffset,
                entry->mObjectIndexTableOffset,
                entry->mObjectDataOffset,
                entry->mMapXSize,
                entry->mMapYSize);
        });
    }

    void ReadAllCameras()
    {
        ForChunksOfType(Oddlib::MakeType('B', 'i', 't', 's'), [&](const std::string&, Oddlib::LvlArchive::FileChunk& chunk, bool )
        {
            auto bits = Oddlib::MakeBits(*chunk.Stream());
            Oddlib::IBits* ptr = nullptr;
            switch (mType)
            {
            case eAoPc:
                ptr = dynamic_cast<Oddlib::AoBitsPc*>(bits.get());
                break;

            case eAePc:
                ptr = dynamic_cast<Oddlib::AeBitsPc*>(bits.get());
                break;

            case eAePsxDemo:
            case eAePsxCd1:
            case eAePsxCd2:
            {
                auto tmp = dynamic_cast<Oddlib::PsxBits*>(bits.get());
                if (tmp && tmp->IncludeLength())
                {
                    ptr = tmp;
                }
            }
                break;

            case eAoPsxDemo:
            case eAoPsx:
            {
                auto tmp = dynamic_cast<Oddlib::PsxBits*>(bits.get());
                if (tmp && !tmp->IncludeLength())
                {
                    ptr = tmp;
                }
            }
                break;

            case eAoPcDemo:
                ptr = dynamic_cast<Oddlib::AoBitsPc*>(bits.get());
                break;

            case eAePcDemo:
                ptr = dynamic_cast<Oddlib::AoBitsPc*>(bits.get());
                if (!ptr)
                {
                    // Very strange, AE PC demo contains "half" cameras of random old PSX
                    // cameras, even half of the anti piracy screen is in here
                    auto tmp = dynamic_cast<Oddlib::PsxBits*>(bits.get());
                    if (tmp && tmp->IncludeLength())
                    {
                        ptr = tmp;
                    }
                }
                break;

            default:
                abort();
            }

            if (!ptr)
            {
                LOG_ERROR("Wrong camera type constructed");
                abort();
            }
            else
            {
                ptr->Save();
            }

        });
    }

    void ReadAllAnimations()
    {
        ForChunksOfType(Oddlib::MakeType('A', 'n', 'i', 'm'), [&](const std::string& fileName, Oddlib::LvlArchive::FileChunk& chunk, bool isPsx)
        {
            if (chunk.Id() == 0x1770)
            {
                // ELECWALL.BAN
             //   abort();
            }
            Oddlib::DebugDumpAnimationFrames(fileName, chunk.Id(), *chunk.Stream(), isPsx, ToString(mType));
        });
    }
   

private:
    eDataSetType mType;
    LvlFileChunkReducer mReducer;
};
*/


int main(int /*argc*/, char** /*argv*/)
{
    const std::vector<std::string> aoPcLvls =
    {
        "c1.lvl",
        "d1.lvl",
        "d2.lvl",
        "d7.lvl",
        "e1.lvl",
        "e2.lvl",
        "f1.lvl",
        "f2.lvl",
        "f4.lvl",
        "l1.lvl",
        "r1.lvl",
        "r2.lvl",
        "r6.lvl",
        "s1.lvl"
    };

    const std::vector<std::string> aoPsxLvls =
    {
        "e1.lvl",
        "f1.lvl",
        "f2.lvl",
        "f4.lvl",
        "l1.lvl",
        "r1.lvl",
        "s1.lvl",
        "p2\\c1.lvl",
        "p2\\d1.lvl",
        "p2\\d2.lvl",
        "p2\\d7.lvl",
        "p2\\e2.lvl",
        "p2\\r2.lvl",
        "p2\\r6.lvl"
    };

    const std::vector<std::string> aoPcDemoLvls =
    {
        "c1.lvl",
        "r1.lvl",
        "s1.lvl"
    };

    const std::vector<std::string> aoPsxDemoLvls =
    {
        "ABESODSE\\C1.LVL",
        "ABESODSE\\E1.LVL",
        "ABESODSE\\R1.LVL",
        "ABESODSE\\S1.LVL"
    };

    const std::vector<std::string> aePsxCd1Lvls =
    {
        "ba\\ba.lvl",
        "br\\br.lvl",
        "bw\\bw.lvl",
        "cr\\cr.lvl",
        "fd\\fd.lvl",
        "mi\\mi.lvl",
        "ne\\ne.lvl",
        "pv\\pv.lvl",
        "st\\st.lvl",
        "sv\\sv.lvl"
    };

    const std::vector<std::string> aePsxCd2Lvls =
    {
        "ba\\ba.lvl",
        "bm\\bm.lvl",
        "br\\br.lvl",
        "bw\\bw.lvl",
        "cr\\cr.lvl",
        "fd\\fd.lvl",
        "mi\\mi.lvl",
        "ne\\ne.lvl",
        "pv\\pv.lvl",
        "st\\st.lvl",
        "sv\\sv.lvl"
    };

    const std::vector<std::string> aePcLvls =
    {
        "ba.lvl",
        "bm.lvl",
        "br.lvl",
        "bw.lvl",
        "cr.lvl",
        "fd.lvl",
        "mi.lvl",
        "ne.lvl",
        "pv.lvl",
        "st.lvl",
        "sv.lvl"
    };

    const std::vector<std::string> aePcDemoLvls =
    {
        "cr.lvl",
        "mi.lvl",
        "st.lvl"
    };

    const std::vector<std::string> aePsxDemoLvls =
    {
        "ABE2\\CR.LVL",
        "ABE2\\MI.LVL",
        "ABE2\\ST.LVL"
    };

    const std::map<eDataSetType, const std::vector<std::string>*> DataTypeLvlMap =
    {
        { eDataSetType::eAoPc, &aoPcLvls },
        { eDataSetType::eAoPsx, &aoPsxLvls },
        { eDataSetType::eAePc, &aePcLvls },
        { eDataSetType::eAePsxCd1, &aePsxCd1Lvls },
        { eDataSetType::eAePsxCd2, &aePsxCd2Lvls },
        { eDataSetType::eAoPcDemo, &aoPcDemoLvls },
        { eDataSetType::eAoPsxDemo, &aoPsxDemoLvls },
        { eDataSetType::eAePcDemo, &aePcDemoLvls },
        { eDataSetType::eAePsxDemo, &aePsxDemoLvls }
    };

    const std::string dataPath = "F:\\Data\\alive\\all_data\\";

    const std::vector<std::pair<eDataSetType, std::string>> datas =
    {
        { eDataSetType::eAePc, dataPath + "Oddworld Abes Exoddus" },
        { eDataSetType::eAePcDemo, dataPath + "exoddemo" },
        { eDataSetType::eAePsxDemo, dataPath + "Euro Demo 38 (E) (Track 1) [SCED-01148].bin" },
        { eDataSetType::eAePsxCd1, dataPath + "Oddworld - Abe's Exoddus (E) (Disc 1) [SLES-01480].bin" },
        { eDataSetType::eAePsxCd2, dataPath + "Oddworld - Abe's Exoddus (E) (Disc 2) [SLES-11480].bin" },
        
        { eDataSetType::eAoPc, dataPath + "Oddworld Abes Oddysee" },
        { eDataSetType::eAoPcDemo, dataPath + "abeodd" },
        { eDataSetType::eAoPsxDemo, dataPath + "Oddworld - Abe's Oddysee (Demo) (E) [SLED-00725].bin" },
        { eDataSetType::eAoPsx, dataPath + "Oddworld - Abe's Oddysee (E) [SLES-00664].bin" },
    };


    // Used to create the "base" json db's. E.g the list of LVL's each data set has, unique collection of anim id's
    // and what BAN/BND's they live in. And which LVL's each BAN/BND lives in.
    class Db
    {
    private:
        std::vector<std::unique_ptr<DeDuplicatedAnimation>> mDeDuplicatedAnimations;
    public:
        Db() = default;

        void Merge(IFileSystem& fs, eDataSetType eType, const std::string& resourcePath, const std::vector<std::string>& lvls)
        {
            mLvlChunkReducer.MergeReduce(fs, resourcePath, lvls, eType);
            /*
            for (const auto& lvl : lvls)
            {
                AddLvlMapping(eType, lvl, isPsx, reducer.LvlFileContent(lvl));
            }*/
        }

        /*
        bool CompareAnims(DeDuplicatedLvlChunk& lhs, DeDuplicatedLvlChunk& rhs)
        {
            // TODO: Check if 2 animations are "the same enough" to actually "be the same"
            const Oddlib::Animation* anim = lhs.mAnimSet->AnimationAt(0);
            const Oddlib::Animation::Frame& frame = anim->GetFrame(0);

            std::ignore = frame;

            return false;
        }
        */

        void MergePcAndPsx()
        {
            // Load the animation set for each unique Anim chunk
            std::vector<std::unique_ptr<DeDuplicatedLvlChunk>>& chunks = mLvlChunkReducer.UniqueChunks();
            for (std::unique_ptr<DeDuplicatedLvlChunk>& chunk : chunks)
            {
                Oddlib::LvlArchive::FileChunk* lvlFileChunk = chunk->mChunk->mChunk;
                if (lvlFileChunk->Type() == Oddlib::MakeType('A', 'n', 'i', 'm'))
                {
                    auto stream = lvlFileChunk->Stream();
                    Oddlib::AnimSerializer as(*stream, IsPsx(chunk->mChunk->mDataSet));
                    chunk->mAnimSet = std::make_unique<Oddlib::AnimationSet>(as);
                }
            }

            // Create a list of deduplicated animations by creating an instance for every animation in
            // each unique chunks animation set
            for (size_t i = 0; i < chunks.size(); i++)
            {
                std::unique_ptr<DeDuplicatedLvlChunk>& chunk = chunks[i];
                Oddlib::AnimationSet& animSet = *chunk->mAnimSet;
                for (Uint32 j = 0; j < animSet.NumberOfAnimations(); j++)
                {
                    auto deDuplicatedAnimation = std::make_unique<DeDuplicatedAnimation>();
                    deDuplicatedAnimation->mAnimation = animSet.AnimationAt(j);
                    deDuplicatedAnimation->mAnimationIndex = j;
                    deDuplicatedAnimation->mContainingChunk = chunk.get();
                    mDeDuplicatedAnimations.push_back(std::move(deDuplicatedAnimation));
                }
            }
        }

        void ToJson()
        {
            jsonxx::Array resources;

            jsonxx::Object animsObject;

            for (const std::unique_ptr<DeDuplicatedAnimation>& deDupedAnim : mDeDuplicatedAnimations)
            {
     
                jsonxx::Object anim;

                // Generated globally unique name
                const std::string strName = 
                    deDupedAnim->mContainingChunk->mChunk->mFileName + "_" +
                    std::to_string(deDupedAnim->mContainingChunk->mChunk->mChunk->Id()) +
                    "_" +
                    ToString(deDupedAnim->mContainingChunk->mChunk->mDataSet) +
                    "_" +
                    std::to_string(deDupedAnim->mAnimationIndex);

                anim
                    << "name"
                    << strName;

                animsObject << "anims" << anim;
                resources << animsObject;
                
                /*
                // Guessed blending mode
                anim << "blend_mode" << 1;

                // TODO: Semi trans flag
                // TODO: pallet res id?

                jsonxx::Array locations;


                anim << "locations" << locations;

                anims << anim;

                // TODO: Index array for each data set
                */

               
            }
           

            /*
            // Map of which LVL's live in what data set
            for (const auto& dataSetPair : mLvlToDataSetMap)
            {
                jsonxx::Object dataSet;
                const std::string strName = DataTest::ToString(dataSetPair.first);
                dataSet << "data_set_name" << strName;
                dataSet << "is_psx" << dataSetPair.second.begin()->second.mIsPsx;

                jsonxx::Array lvlsArray;

            
                for (const auto& lvlData : dataSetPair.second)
                {
                    jsonxx::Object lvlObj;
                    lvlObj << "name" << lvlData.first;

                    jsonxx::Array lvlContent;
                    const std::set<std::string>& content = lvlData.second.mFiles;
                    for (const auto& lvlFile : content)
                    {
                        lvlContent << lvlFile;
                    }
                    lvlObj << "files" << lvlContent;
                    lvlsArray << lvlObj;
                }
                dataSet << "lvls" << lvlsArray;
            

                resources << dataSet;
            }
            */

            std::ofstream jsonFile("test.json");
            if (!jsonFile.is_open())
            {
                abort();
            }
            jsonFile << resources.json().c_str() << std::endl;
        }

        private:

            /*
        void AddNumAnimationsMapping(Uint32 resId, Uint32 numAnims)
        {
            // TODO: Must store num anims for each dataset for saving indexes
            // in ToJson

            auto it = mNumberOfAnimsMap.find(resId);
            if (it == std::end(mNumberOfAnimsMap))
            {
                mNumberOfAnimsMap[resId] = numAnims;
            }
            else
            {
                // TODO: If not equal then should probably just take the biggest value
                if (resId == 314) return; // BLOOD.BAN and SPARKS.BAN have same id but diff number of anims
                if (resId == 2020) return; // SLAM.BAN and SLAMVLTS.BAN

                if (resId == 10) return; // ABEBSIC.BAN 47 VS 51 of AePcDemo
                if (resId == 55) return; // ABEBSIC1.BAN 11 VS 10 of AePcDemo
                if (resId == 43) return; // ABEEDGE.BAN 43 VS 10 of AePcDemo
                if (resId == 28) return; // ABEKNOKZ.BAN 2 VS 3 of AePcDemo
                if (resId == 518) return; // MUDTORT.BAN 3 VS 2
                if (resId == 604) return;
                if (resId == 605) return;
                if (resId == 45) return; // ABEDOOR.BAN 4 vs 2 ao demo
                if (resId == 26) return; // 4 VS 5
                if (resId == 203) return; // 1 VS 2
                if (resId == 511) return; // 3 VS 6
                if (resId == 130) return; // 15 vs 10
                if (resId == 576) return; // 2 vs 3
                if (resId == 1000) return; //1 vs 2
                if (resId == 800) return; // 24 vs 5
                if (resId == 200) return; // 3 vs 14
                if (resId == 700) return; // 18 vs 12
                if (resId == 711) return; // 2 vs 1
                if (resId == 701) return; // 3 vs 2
                if (resId == 704) return; // 2 vs 1
                if (resId == 113) return; // 2 vs 1
                if (resId == 600) return; // 22 vs 13
                if (resId == 2013) return; // 2 vs 1
                if (resId == 27) return; // 2 vs 3
                if (resId == 16) return;
                if (resId == 415) return;
                if (resId == 100) return;
                if (resId == 6000) return;
                if (resId == 2012) return;
                if (resId == 2019) return;
                if (resId == 2018) return;
                if (resId == 2007) return;
                if (resId == 337) return;
                if (resId == 2026) return;
                if (resId == 508) return;
                if (resId == 8002) return;

                assert(it->second == numAnims);
            }
        }
        */



        /*
        void AddLvlMapping(eDataSetType eType, const std::string& lvlName, bool isPsx, const std::set<std::string>& files)
        {
            auto it = mLvlToDataSetMap[eType].find(lvlName);
            if (it == std::end(mLvlToDataSetMap[eType]))
            {
                mLvlToDataSetMap[eType][lvlName] = LvlData{ isPsx, files };
            }
        }
        */

    private:

        LvlFileChunkReducer mLvlChunkReducer;
    };

    Db db;
    GameFileSystem gameFs;
    if (!gameFs.Init())
    {
        std::cout << "Game FS init failed" << std::endl;
        return 1;
    }

    for (const auto& data : datas)
    {
        const auto it = DataTypeLvlMap.find(data.first);
        if (it == std::end(DataTypeLvlMap))
        {
            // Defined struct is wrong
            abort();
        }
        db.Merge(gameFs, data.first, data.second, *it->second);
    }

    db.MergePcAndPsx();

    db.ToJson();

    return 0;
}
