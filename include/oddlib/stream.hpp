#pragma once

#include <vector>
#include <iostream>
#include <memory>
#include "SDL_types.h"

namespace Oddlib
{
    class Stream
    {
    public:
        Stream(const std::string& fileName);
        Stream(std::vector<Uint8>&& data);
        void ReadUInt32(Uint32& output);
        void ReadUInt16(Uint16& output);
        void ReadBytes(Sint8* pDest, size_t destSize);
        void ReadBytes(Uint8* pDest, size_t destSize);
        void Seek(size_t pos);
        size_t Pos() const;
    private:
        mutable std::unique_ptr<std::istream> mStream;
    };
}
