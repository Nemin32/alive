#include "oddlib/anim.hpp"
#include "oddlib/lvlarchive.hpp"
#include "oddlib/stream.hpp"
#include "oddlib/compressiontype3ae.hpp"
#include "oddlib/compressiontype4or5.hpp"
#include "logger.hpp"
#include "sdl_raii.hpp"
#include <assert.h>
#include <fstream>

namespace Oddlib
{
    AnimSerializer::AnimSerializer(IStream& stream)
    {
        // Read the header
        stream.ReadUInt16(h.mMaxW);
        stream.ReadUInt16(h.mMaxH);
        stream.ReadUInt32(h.mFrameTableOffSet);
        stream.ReadUInt32(h.mPaltSize);
        if (h.mPaltSize == 0)
        {
            // Assume its an Ae file if the palt size is zero, in this case the next Uint32 is
            // actually the palt size.
            mbIsAoFile = false;
            stream.ReadUInt32(h.mPaltSize);
        }

        // Read the palette
        mPalt.reserve(h.mPaltSize);
        for (auto i = 0u; i < h.mPaltSize; i++)
        {
            Uint16 tmp = 0;
            stream.ReadUInt16(tmp);


            unsigned int oldPixel = tmp;

            // RGB555
            unsigned short int green = (((oldPixel >> 5) & 0x1F) << 6);
            unsigned short int blue = (((oldPixel >> 10) & 0x1F) << 0);
            unsigned short int red = (((oldPixel >> 0) & 0x1F) << 11);

            oldPixel = (static_cast<unsigned short int> (oldPixel) >> 15) & 0xffff; // Checking transparent bit?
            if (oldPixel)
            {
                //LOG_INFO("Transparent");
            }

            unsigned short int newPixel = red | blue | green;


            mPalt.push_back(newPixel);
        }

        // Seek to frame table offset
        stream.Seek(h.mFrameTableOffSet);

        ParseAnimationSets(stream);
        ParseFrameInfoHeaders(stream);
        GatherUniqueFrameOffsets();
        mUniqueFrameHeaderOffsets.insert(h.mFrameTableOffSet);
        DebugDecodeAllFrames(stream);
    }

    void AnimSerializer::ParseAnimationSets(IStream& stream)
    {
        // Collect all animation sets
        auto hdr = std::make_unique<AnimationHeader>();
        stream.ReadUInt16(hdr->mFps);
        stream.ReadUInt16(hdr->mNumFrames);
        stream.ReadUInt16(hdr->mLoopStartFrame);
        stream.ReadUInt16(hdr->mFlags);
        
        // Read the offsets to each frame info
        hdr->mFrameInfoOffsets.resize(hdr->mNumFrames);
        for (auto& offset : hdr->mFrameInfoOffsets)
        {
            stream.ReadUInt32(offset);
        }

        // The first set is usually the last set. Either way when we get to the end of the file from
        // reading the final set we start looking for sets at the end of this sets final info data.
        // Eventually we'll get to the info at iFrameTableOffSet again and stop the recursion.
        // This might be wrong and missing some sets, it might be better to track the position at the end
        // of each set so far and use the one nearest to the EOF.
        if (stream.AtEnd())
        {
            if (hdr->mFrameInfoOffsets.empty())
            {
                // Handle a very strange case seen in AO ROPES.BAN, we have a set header that says there are
                // zero frames, then we have a single frame offset *before* the start of the animation!
                stream.Seek(0);
                stream.Seek(stream.Size() - 0x14);
                ParseAnimationSets(stream);
            }
            else
            {
                // As we said above move to the last frame info offset
                auto offset = hdr->mFrameInfoOffsets.back();
                stream.Seek(offset);

                // Now we want to seek to the end of the frame info where we hope the FrameSet data
                // *really* starts (and why we'll end up where we started after parsing down this list)
                auto frmHdr = std::make_unique<FrameInfoHeader>();

                stream.ReadUInt32(frmHdr->mFrameHeaderOffset);
                stream.ReadUInt32(frmHdr->mMagic);
                // stream.ReadUInt16(frmHdr->points);
                // stream.ReadUInt16(frmHdr->triggers);

                // if (frmHdr->points != 0x3)
                {
                    // I think there is always 3 points?
                    //  abort();
                }
                Uint32 headerDataToSkipSize = 0;
                switch (frmHdr->mMagic)
                {
                    // Always this for AO frames, and almost always this for AE frames
                case 0x3:
                    headerDataToSkipSize = 0x14;
                    break;

                    // Sometimes we  get this for AE!
                case 0x7:
                    headerDataToSkipSize = 0x2C;
                    break;

                    // Sometimes we get this for AE!
                case 0x9:
                    headerDataToSkipSize = 0x2C;
                    break;

                default:
                    headerDataToSkipSize = 0x1C;
                    break;
                }

                stream.Seek(offset + headerDataToSkipSize);
            }
        }

        mAnimationHeaders.emplace_back(std::move(hdr));

        if (stream.Pos() != h.mFrameTableOffSet)
        {
            ParseAnimationSets(stream);
        }
    }

    void AnimSerializer::ParseFrameInfoHeaders(IStream& stream)
    {
        for (const std::unique_ptr<AnimationHeader>& animationHeader : mAnimationHeaders)
        {
            for (const Uint32 frameInfoOffset : animationHeader->mFrameInfoOffsets)
            {
                stream.Seek(frameInfoOffset);

                auto frameInfo = std::make_unique<FrameInfoHeader>();

                stream.ReadUInt32(frameInfo->mFrameHeaderOffset);
                stream.ReadUInt32(frameInfo->mMagic);

                stream.ReadSInt16(frameInfo->mColx);
                stream.ReadSInt16(frameInfo->mColy);

                stream.ReadSInt16(frameInfo->mColw);
                stream.ReadSInt16(frameInfo->mColh);

                stream.ReadSInt16(frameInfo->mOffx);
                stream.ReadSInt16(frameInfo->mOffy);

                animationHeader->mFrameInfos.emplace_back(std::move(frameInfo));
            }
        }
    }

    void AnimSerializer::GatherUniqueFrameOffsets()
    {
        for (const std::unique_ptr<AnimationHeader>& animationHeader : mAnimationHeaders)
        {
            for (const std::unique_ptr<FrameInfoHeader>& frameInfo : animationHeader->mFrameInfos)
            {
                mUniqueFrameHeaderOffsets.insert(frameInfo->mFrameHeaderOffset);
            }
        }
    }

    void AnimSerializer::DebugDecodeAllFrames(IStream& stream)
    {
        auto endIt = mUniqueFrameHeaderOffsets.end();
        std::advance(endIt, -2);

        for (auto it = mUniqueFrameHeaderOffsets.begin(); it != endIt; it++)
        {
            const Uint32 frameOffset = *it;
            auto itCopy = it;
            itCopy++;
            const Uint32 nextFrameOffset = *itCopy;
            const Uint32 frameDataSize = (nextFrameOffset - frameOffset) - sizeof(FrameHeader);
            DecodeFrame(stream, frameOffset, frameDataSize);
        }
    }

    void AnimSerializer::DebugSaveFrame(AnimSerializer::FrameHeader& header, Uint32 realWidth, const std::vector<Uint8>& decompressedData)
    {
        // Apply the pallete
        std::vector<Uint16> convertedBuffer;
        for (auto v : decompressedData)
        {
            convertedBuffer.push_back(mPalt[v]);
        }

        // Create an SDL surface
        const auto red_mask = 0xF800;
        const auto green_mask = 0x7E0;
        const auto blue_mask = 0x1F;
        SDL_SurfacePtr surface(SDL_CreateRGBSurfaceFrom(convertedBuffer.data(), realWidth, header.mHeight, 16, realWidth*sizeof(Uint16), red_mask, green_mask, blue_mask, 0));

        // Save surface to disk
        static int i = 1;
        SDL_SaveBMP(surface.get(), ("frame" + std::to_string(i++) + ".bmp").c_str());
    }

    std::vector<Uint8> AnimSerializer::DecodeFrame(IStream& stream, Uint32 frameOffset, Uint32 frameDataSize)
    {
        stream.Seek(frameOffset);

        FrameHeader frameHeader;
        stream.ReadUInt32(frameHeader.mMagic);
        stream.ReadUInt8(frameHeader.mWidth);
        stream.ReadUInt8(frameHeader.mHeight);
        stream.ReadUInt8(frameHeader.mColourDepth);
        stream.ReadUInt8(frameHeader.mCompressionType);
        stream.ReadUInt32(frameHeader.mFrameDataSize);

        Uint32 nTextureWidth = 0;
        Uint32 actualWidth = 0;
        if (frameHeader.mColourDepth == 8)
        {
            nTextureWidth = ((frameHeader.mWidth + 3) / 2) & ~1;
            actualWidth = nTextureWidth * 2;
        }
        else if (frameHeader.mColourDepth == 16)
        {
            nTextureWidth = ((frameHeader.mWidth + 1)) & ~1;
            actualWidth = nTextureWidth;
        }
        else
        {
            // 4?
            assert(frameHeader.mColourDepth == 4);
            nTextureWidth = ((frameHeader.mWidth + 7) / 4)&~1;
            actualWidth = nTextureWidth * 4; // TODO: Check me
        }

        // TODO: Decompressors

        if (frameHeader.mCompressionType != 3 && frameHeader.mCompressionType != 0 && frameHeader.mCompressionType != 4)
        {
            LOG_INFO("Compression type " << static_cast<Uint32>(frameHeader.mCompressionType));
        }

        const auto pos = stream.Pos();

        switch (frameHeader.mCompressionType)
        {
        case 0:
            // Used in AE and AO (seems to mean "no compression"?)
        {
            // TODO: Frame widths are wrong for some AE PC demo frames
            std::vector<Uint8> data(frameDataSize+4);
            stream.ReadBytes(data.data(), data.size());
            //DebugSaveFrame(frameHeader, actualWidth, data);
        }
            break;

        // Run length encoding compression type
        case 1:
            // In AE but never used, used for AO, same algorithm, 0x0040A610 in AE
            break;

        case 2:
            // In AE but never used, used for AO, same algorithm, 0x0040AA50 in AE
            break;

        case 3:
        {
            // Not the same algo in AO?
            CompressionType3Ae d;
            auto decompressedData = d.Decompress(stream, actualWidth, frameHeader.mWidth, frameHeader.mHeight, frameDataSize);
            //DebugSaveFrame(frameHeader, actualWidth, decompressedData);
        }
            break;

        case 4:
        case 5:
            // Both AO and AE
        {
            CompressionType4Or5 d;
            auto decompressedData = d.Decompress(stream, actualWidth, frameHeader.mWidth, frameHeader.mHeight, frameDataSize);
            //DebugSaveFrame(frameHeader, actualWidth, decompressedData);
        }
            break;

        case 6:
            // AE, never seems to get hit for sprites
            // TODO: Actually does get hit, or hit because of some other parsing failure
            //abort();
            LOG_INFO("Type 6");
            break;

        case 7:
        case 8:
            // AE, also never seems to be used
            abort();
            break;
        }

        // TODO: Apply pallete to decompressed frame

        stream.Seek(pos);

        return std::vector<Uint8>();
    }

    // ==================================================

    /*static*/ std::vector<std::unique_ptr<Animation>> AnimationFactory::Create(Oddlib::LvlArchive& archive, const std::string& fileName, Uint32 resourceId)
    {
        std::vector < std::unique_ptr<Animation> > r;

        Stream stream(archive.FileByName(fileName)->ChunkById(resourceId)->ReadData());
        AnimSerializer anim(stream);

        return r;
    }

}
