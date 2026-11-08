#include "xex.h"
#include "image.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <aes.hpp>
#include <TinySHA1.hpp>
#include <xex_patcher.h>

#define STRINGIFY(X) #X
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), "__import__" STRINGIFY(NAME) }

#ifndef _WIN32

typedef struct _IMAGE_DOS_HEADER { 
    uint16_t   e_magic;                
    uint16_t   e_cblp;                 
    uint16_t   e_cp;                   
    uint16_t   e_crlc;                 
    uint16_t   e_cparhdr;              
    uint16_t   e_minalloc;             
    uint16_t   e_maxalloc;             
    uint16_t   e_ss;                   
    uint16_t   e_sp;                   
    uint16_t   e_csum;                 
    uint16_t   e_ip;                   
    uint16_t   e_cs;                   
    uint16_t   e_lfarlc;               
    uint16_t   e_ovno;                 
    uint16_t   e_res[4];               
    uint16_t   e_oemid;                
    uint16_t   e_oeminfo;              
    uint16_t   e_res2[10];             
    uint32_t   e_lfanew;               
} IMAGE_DOS_HEADER, * PIMAGE_DOS_HEADER;

typedef struct _IMAGE_FILE_HEADER {
    uint16_t   Machine;
    uint16_t   NumberOfSections;
    uint32_t   TimeDateStamp;
    uint32_t   PointerToSymbolTable;
    uint32_t   NumberOfSymbols;
    uint16_t   SizeOfOptionalHeader;
    uint16_t   Characteristics;
} IMAGE_FILE_HEADER, * PIMAGE_FILE_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY {
    uint32_t   VirtualAddress;
    uint32_t   Size;
} IMAGE_DATA_DIRECTORY, * PIMAGE_DATA_DIRECTORY;

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES    16

typedef struct _IMAGE_OPTIONAL_HEADER {
    uint16_t   Magic;
    uint8_t    MajorLinkerVersion;
    uint8_t    MinorLinkerVersion;
    uint32_t   SizeOfCode;
    uint32_t   SizeOfInitializedData;
    uint32_t   SizeOfUninitializedData;
    uint32_t   AddressOfEntryPoint;
    uint32_t   BaseOfCode;
    uint32_t   BaseOfData;
    uint32_t   ImageBase;
    uint32_t   SectionAlignment;
    uint32_t   FileAlignment;
    uint16_t   MajorOperatingSystemVersion;
    uint16_t   MinorOperatingSystemVersion;
    uint16_t   MajorImageVersion;
    uint16_t   MinorImageVersion;
    uint16_t   MajorSubsystemVersion;
    uint16_t   MinorSubsystemVersion;
    uint32_t   Win32VersionValue;
    uint32_t   SizeOfImage;
    uint32_t   SizeOfHeaders;
    uint32_t   CheckSum;
    uint16_t   Subsystem;
    uint16_t   DllCharacteristics;
    uint32_t   SizeOfStackReserve;
    uint32_t   SizeOfStackCommit;
    uint32_t   SizeOfHeapReserve;
    uint32_t   SizeOfHeapCommit;
    uint32_t   LoaderFlags;
    uint32_t   NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32, * PIMAGE_OPTIONAL_HEADER32;

typedef struct _IMAGE_NT_HEADERS {
    uint32_t Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32, * PIMAGE_NT_HEADERS32;

#define IMAGE_SIZEOF_SHORT_NAME              8

typedef struct _IMAGE_SECTION_HEADER {
    uint8_t    Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        uint32_t   PhysicalAddress;
        uint32_t   VirtualSize;
    } Misc;
    uint32_t   VirtualAddress;
    uint32_t   SizeOfRawData;
    uint32_t   PointerToRawData;
    uint32_t   PointerToRelocations;
    uint32_t   PointerToLinenumbers;
    uint16_t   NumberOfRelocations;
    uint16_t   NumberOfLinenumbers;
    uint32_t   Characteristics;
} IMAGE_SECTION_HEADER, * PIMAGE_SECTION_HEADER;

#define IMAGE_SCN_CNT_CODE                   0x00000020

#endif

std::unordered_map<size_t, const char*> XamExports = 
{
    #include "xbox/xam_table.inc"
};

std::unordered_map<size_t, const char*> XboxKernelExports =
{
    #include "xbox/xboxkrnl_table.inc"
};

Image PeLoadImage(const uint8_t* data, size_t dataSize)
{
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(data);
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(data + dosHeader->e_lfanew);

    // PPC big-endian PE (Xbox 360 kernel / system module); header fields are little-endian
    if (ntHeaders->FileHeader.Machine != 0x01F2)
        return {};

    Image image{};
    const uint32_t imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    image.data = std::make_unique<uint8_t[]>(imageSize);
    memset(image.data.get(), 0, imageSize);
    image.size = imageSize;
    image.base = ntHeaders->OptionalHeader.ImageBase;
    image.entry_point = image.base + ntHeaders->OptionalHeader.AddressOfEntryPoint;

    // The Xbox 360 kernel .bin files (xboxkrnlce/cf.bin) are MEMORY IMAGES, not
    // packed on-disk PEs: the file offset of an RVA is the RVA itself, and the
    // section headers' PointerToRawData is stale left-over metadata. Emu loads
    // them the same way -- it maps the raw file flat at ImageBase, and logs
    // "Mapping guest image range at 0x80040000 (size=1572864)", which is
    // exactly the file size.
    //
    // Loading such an image through PointerToRawData is silently catastrophic:
    // PPC is fixed-width and bl is PC-relative, so .text lands at a wrong
    // offset (ce +0x1E00, cf +0x1A00) yet still disassembles as coherent,
    // internally consistent code. Only absolute cross-section references break.
    const bool flatImage = (imageSize == dataSize);

    if (flatImage)
        memcpy(image.data.get(), data, imageSize);
    else
        memcpy(image.data.get(), data, ntHeaders->OptionalHeader.SizeOfHeaders);

    const auto numSections = ntHeaders->FileHeader.NumberOfSections;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(ntHeaders + 1);

    for (size_t i = 0; i < numSections; i++)
    {
        const auto& section = sections[i];

        if (section.VirtualAddress >= imageSize)
            continue;

        // In a flat image the bytes are already in place from the single copy
        // above. Copying per-section would also truncate any section whose raw
        // size is smaller than its virtual size (the ce kernel's .data has
        // SizeOfRawData 0x2200 against VirtualSize 0xCB700).
        if (!flatImage)
        {
            uint32_t copySize = section.SizeOfRawData;
            if (section.VirtualAddress + copySize > imageSize)
                copySize = imageSize - section.VirtualAddress;
            if (section.PointerToRawData + copySize > dataSize)
                copySize = section.PointerToRawData < dataSize ? static_cast<uint32_t>(dataSize - section.PointerToRawData) : 0;

            if (copySize != 0)
                memcpy(image.data.get() + section.VirtualAddress, data + section.PointerToRawData, copySize);
        }

        uint8_t flags{};
        if (section.Characteristics & IMAGE_SCN_CNT_CODE)
        {
            flags |= SectionFlags_Code;
        }

        char sectionName[9]{};
        memcpy(sectionName, section.Name, 8);

        image.Map(sectionName, section.VirtualAddress,
            section.Misc.VirtualSize, flags, image.data.get() + section.VirtualAddress);
    }

    return image;
}

// Decrypt and decompress a XEX to its raw image, WITHOUT the section mapping and
// import-thunk rewriting that Xex2LoadImage performs. Xex2LoadImage clobbers every
// import thunk with nop/nop/nop/blr for the recompiler's benefit -- fine for
// recompilation, fatal if the result is going to be executed by the emulator.
static bool Xex2DecodeImage(const uint8_t* data, size_t dataSize, std::vector<uint8_t>& image)
{
    const auto* header = reinterpret_cast<const Xex2Header*>(data);
    const auto* security = reinterpret_cast<const Xex2SecurityInfo*>(data + header->securityOffset);
    const auto* fileFormatInfo = reinterpret_cast<const Xex2OptFileFormatInfo*>(getOptHeaderPtr(data, XEX_HEADER_FILE_FORMAT_INFO));

    if (fileFormatInfo == nullptr || fileFormatInfo->compressionType > XEX_COMPRESSION_NORMAL)
        return false;

    std::unique_ptr<uint8_t[]> decryptedData;
    const uint8_t* srcData = nullptr;

    if (fileFormatInfo->encryptionType == XEX_ENCRYPTION_NORMAL)
    {
        constexpr uint32_t KeySize = 16;
        AES_ctx aesContext;

        uint8_t decryptedKey[KeySize];
        memcpy(decryptedKey, security->aesKey, KeySize);
        AES_init_ctx_iv(&aesContext, Xex2RetailKey, AESBlankIV);
        AES_CBC_decrypt_buffer(&aesContext, decryptedKey, KeySize);

        decryptedData = std::make_unique<uint8_t[]>(dataSize - header->headerSize);
        memcpy(decryptedData.get(), data + header->headerSize, dataSize - header->headerSize);
        AES_init_ctx_iv(&aesContext, decryptedKey, AESBlankIV);
        AES_CBC_decrypt_buffer(&aesContext, decryptedData.get(), dataSize - header->headerSize);

        srcData = decryptedData.get();
    }
    else
    {
        srcData = data + header->headerSize;
    }

    if (fileFormatInfo->compressionType == XEX_COMPRESSION_NONE)
    {
        image.assign(srcData, srcData + security->imageSize);
    }
    else if (fileFormatInfo->compressionType == XEX_COMPRESSION_BASIC)
    {
        const auto* blocks = reinterpret_cast<const Xex2FileBasicCompressionBlock*>(fileFormatInfo + 1);
        const size_t numBlocks = (fileFormatInfo->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;

        size_t imageSize = 0;
        for (size_t i = 0; i < numBlocks; i++)
            imageSize += blocks[i].dataSize + blocks[i].zeroSize;

        image.assign(imageSize, 0);
        uint8_t* destData = image.data();

        for (size_t i = 0; i < numBlocks; i++)
        {
            memcpy(destData, srcData, blocks[i].dataSize);
            srcData += blocks[i].dataSize;
            destData += blocks[i].dataSize;
            destData += blocks[i].zeroSize; // already zeroed
        }
    }
    else // XEX_COMPRESSION_NORMAL
    {
        image.assign(security->imageSize, 0);

        const Xex2CompressedBlockInfo* blocks = &((const Xex2FileNormalCompressionInfo*)(fileFormatInfo + 1))->firstBlock;
        const uint32_t exeLength = uint32_t(dataSize) - header->headerSize;

        auto compressBuffer = std::make_unique<uint8_t[]>(exeLength);
        const uint8_t* p = srcData;
        uint8_t* d = compressBuffer.get();
        sha1::SHA1 s;

        uint8_t blockCalcedDigest[0x14];
        while (blocks->blockSize)
        {
            const uint8_t* pNext = p + blocks->blockSize;
            const auto* nextBlock = (const Xex2CompressedBlockInfo*)p;

            s.reset();
            s.processBytes(p, blocks->blockSize);
            s.finalize(blockCalcedDigest);

            if (memcmp(blockCalcedDigest, blocks->blockHash, 0x14) != 0)
                return false;

            p += 4;
            p += 20;

            while (true)
            {
                const size_t chunkSize = (p[0] << 8) | p[1];
                p += 2;
                if (!chunkSize)
                    break;

                memcpy(d, p, chunkSize);
                p += chunkSize;
                d += chunkSize;
            }

            p = pNext;
            blocks = nextBlock;
        }

        if (lzxDecompress(compressBuffer.get(), d - compressBuffer.get(), image.data(), security->imageSize,
                ((const Xex2FileNormalCompressionInfo*)(fileFormatInfo + 1))->windowSize, nullptr, 0) != 0)
            return false;
    }

    return true;
}

bool Xex2RepackBasic(const uint8_t* data, size_t dataSize, std::vector<uint8_t>& out, bool encrypt)
{
    const auto* header = reinterpret_cast<const Xex2Header*>(data);
    if (header->magic != 0x58455832) // 'XEX2'
        return false;

    const auto* security = reinterpret_cast<const Xex2SecurityInfo*>(data + header->securityOffset);
    const auto* fileFormatInfo = reinterpret_cast<const Xex2OptFileFormatInfo*>(getOptHeaderPtr(data, XEX_HEADER_FILE_FORMAT_INFO));
    if (fileFormatInfo == nullptr)
        return false;

    std::vector<uint8_t> image;
    if (!Xex2DecodeImage(data, dataSize, image))
        return false;

    const uint32_t headerSize = header->headerSize;
    const uint32_t imageSize = uint32_t(image.size());

    // A single basic block holding the whole image, no zero-fill tail.
    out.resize(size_t(headerSize) + imageSize);
    memcpy(out.data(), data, headerSize);
    memcpy(out.data() + headerSize, image.data(), imageSize);

    // Optionally re-encrypt with the same file key the original used, so the result
    // is enc=1/comp=1 -- the exact form MS's BC packaging ships game executables in.
    if (encrypt)
    {
        constexpr uint32_t KeySize = 16;
        AES_ctx aesContext;

        uint8_t decryptedKey[KeySize];
        memcpy(decryptedKey, security->aesKey, KeySize);
        AES_init_ctx_iv(&aesContext, Xex2RetailKey, AESBlankIV);
        AES_CBC_decrypt_buffer(&aesContext, decryptedKey, KeySize);

        if ((imageSize % 16) != 0)
            return false;

        AES_init_ctx_iv(&aesContext, decryptedKey, AESBlankIV);
        AES_CBC_encrypt_buffer(&aesContext, out.data() + headerSize, imageSize);
    }

    const size_t fileFormatOffset = reinterpret_cast<const uint8_t*>(fileFormatInfo) - data;
    auto* newInfo = reinterpret_cast<Xex2OptFileFormatInfo*>(out.data() + fileFormatOffset);
    newInfo->infoSize = sizeof(Xex2OptFileFormatInfo) + sizeof(Xex2FileBasicCompressionBlock);
    newInfo->encryptionType = encrypt ? XEX_ENCRYPTION_NORMAL : XEX_ENCRYPTION_NONE;
    newInfo->compressionType = XEX_COMPRESSION_BASIC;

    auto* block = reinterpret_cast<Xex2FileBasicCompressionBlock*>(newInfo + 1);
    block->dataSize = imageSize;
    block->zeroSize = 0;

    return true;
}

Image Xex2LoadImage(const uint8_t* data, size_t dataSize)
{
    auto* header = reinterpret_cast<const Xex2Header*>(data);
    auto* security = reinterpret_cast<const Xex2SecurityInfo*>(data + header->securityOffset);
    const auto* fileFormatInfo = reinterpret_cast<const Xex2OptFileFormatInfo*>(getOptHeaderPtr(data, XEX_HEADER_FILE_FORMAT_INFO));

    Image image{};
    std::unique_ptr<uint8_t[]> result{};
    size_t imageSize = security->imageSize;

    // Decompress image
    if (fileFormatInfo != nullptr)
    {
        assert(fileFormatInfo->compressionType <= XEX_COMPRESSION_NORMAL);

        std::unique_ptr<uint8_t[]> decryptedData;
        const uint8_t* srcData = nullptr;

        if (fileFormatInfo->encryptionType == XEX_ENCRYPTION_NORMAL)
        {
            constexpr uint32_t KeySize = 16;
            AES_ctx aesContext;

            uint8_t decryptedKey[KeySize];
            memcpy(decryptedKey, security->aesKey, KeySize);
            AES_init_ctx_iv(&aesContext, Xex2RetailKey, AESBlankIV);
            AES_CBC_decrypt_buffer(&aesContext, decryptedKey, KeySize);

            decryptedData = std::make_unique<uint8_t[]>(dataSize - header->headerSize);
            memcpy(decryptedData.get(), data + header->headerSize, dataSize - header->headerSize);
            AES_init_ctx_iv(&aesContext, decryptedKey, AESBlankIV);
            AES_CBC_decrypt_buffer(&aesContext, decryptedData.get(), dataSize - header->headerSize);

            srcData = decryptedData.get();
        }
        else
        {
            srcData = data + header->headerSize;
        }

        if (fileFormatInfo->compressionType == XEX_COMPRESSION_NONE)
        {
            result = std::make_unique<uint8_t[]>(imageSize);
            memcpy(result.get(), srcData, imageSize);
        }
        else if (fileFormatInfo->compressionType == XEX_COMPRESSION_BASIC)
        {
            auto* blocks = reinterpret_cast<const Xex2FileBasicCompressionBlock*>(fileFormatInfo + 1);
            const size_t numBlocks = (fileFormatInfo->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;

            size_t blockTotal = 0;
            for (size_t i = 0; i < numBlocks; i++)
            {
                blockTotal += blocks[i].dataSize + blocks[i].zeroSize;
            }

            // s4: allocate security->imageSize, NOT the block total.
            //
            // The blocks can cover less VA than the image spans -- Fable II's
            // blocks sum to 0x1600000 while security->imageSize is 0x1620000,
            // the 0x20000 shortfall being the tail of .reloc, which the xex
            // simply does not store. This code used to overwrite the local
            // `imageSize` with the block total and allocate THAT, while line
            // ~507 still assigns `image.size = security->imageSize`. The result
            // was an Image whose `data` buffer was 128 KB SHORTER than its
            // advertised `size`, so every consumer that trusts size to bound
            // data -- Image::Find on a high VA, or anything sweeping the image
            // -- read past the end of the heap block.
            //
            // Found when a flat dump of image.data for image.size bytes failed
            // with EINVAL at exactly offset 0x1600000. Zero-filling the tail is
            // what the guest sees there anyway, and it leaves image.size (which
            // becomes PPC_IMAGE_SIZE, the runtime's guest mapping extent, and
            // must stay the true VA span) untouched.
            if (blockTotal > imageSize)
                imageSize = blockTotal;

            result = std::make_unique<uint8_t[]>(imageSize);
            memset(result.get(), 0, imageSize);
            auto* destData = result.get();

            for (size_t i = 0; i < numBlocks; i++)
            {
                memcpy(destData, srcData, blocks[i].dataSize);

                srcData += blocks[i].dataSize;
                destData += blocks[i].dataSize;

                memset(destData, 0, blocks[i].zeroSize);
                destData += blocks[i].zeroSize;
            }
        }
        else if (fileFormatInfo->compressionType == XEX_COMPRESSION_NORMAL)
        {
            result = std::make_unique<uint8_t[]>(imageSize);
            auto* destData = result.get();

            const Xex2CompressedBlockInfo* blocks = &((const Xex2FileNormalCompressionInfo*)(fileFormatInfo + 1))->firstBlock;
            const uint32_t headerSize = header->headerSize.get();

            const uint32_t exeLength = dataSize - headerSize;
            const uint8_t* exeBuffer = srcData;

            auto compressBuffer = std::make_unique<uint8_t[]>(exeLength);
            const uint8_t* p = NULL;
            uint8_t* d = NULL;
            sha1::SHA1 s;

            p = exeBuffer;
            d = compressBuffer.get();

            uint8_t blockCalcedDigest[0x14];
            while (blocks->blockSize) 
            {
                const uint8_t* pNext = p + blocks->blockSize;
                const auto* nextBlock = (const Xex2CompressedBlockInfo*)p;

                s.reset();
                s.processBytes(p, blocks->blockSize);
                s.finalize(blockCalcedDigest);

                if (memcmp(blockCalcedDigest, blocks->blockHash, 0x14) != 0)
                    return {};

                p += 4;
                p += 20;

                while (true) 
                {
                    const size_t chunkSize = (p[0] << 8) | p[1];
                    p += 2;

                    if (!chunkSize)
                        break;

                    memcpy(d, p, chunkSize);
                    p += chunkSize;
                    d += chunkSize;
                }

                p = pNext;
                blocks = nextBlock;
            }

            int resultCode = 0;
            uint32_t uncompressedSize = security->imageSize;
            uint8_t* buffer = destData;

            resultCode = lzxDecompress(compressBuffer.get(), d - compressBuffer.get(), buffer, uncompressedSize, ((const Xex2FileNormalCompressionInfo*)(fileFormatInfo + 1))->windowSize, nullptr, 0);

            if (resultCode)
                return {};
        }
    }

    image.data = std::move(result);
    image.size = security->imageSize;

    // Map image
    const auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data.get());
    const auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data.get() + dosHeader->e_lfanew);

    image.base = security->loadAddress;
    const void* xex2BaseAddressPtr = getOptHeaderPtr(data, XEX_HEADER_IMAGE_BASE_ADDRESS);
    if (xex2BaseAddressPtr != nullptr)
    {
        image.base = *reinterpret_cast<const be<uint32_t>*>(xex2BaseAddressPtr);
    }
    const void* xex2EntryPointPtr = getOptHeaderPtr(data, XEX_HEADER_ENTRY_POINT);
    if (xex2EntryPointPtr != nullptr)
    {
        image.entry_point = *reinterpret_cast<const be<uint32_t>*>(xex2EntryPointPtr);
    }

    const auto numSections = ntHeaders->FileHeader.NumberOfSections;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(ntHeaders + 1);

    for (size_t i = 0; i < numSections; i++)
    {
        const auto& section = sections[i];
        uint8_t flags{};

        if (section.Characteristics & IMAGE_SCN_CNT_CODE)
        {
            flags |= SectionFlags_Code;
        }

        image.Map(reinterpret_cast<const char*>(section.Name), section.VirtualAddress, 
            section.Misc.VirtualSize, flags, image.data.get() + section.VirtualAddress);
    }

    auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(data, XEX_HEADER_IMPORT_LIBRARIES));
    if (imports != nullptr)
    {
        std::vector<std::string_view> stringTable;
        auto* pStrTable = reinterpret_cast<const char*>(imports + 1);

        size_t paddedStringOffset = 0;
        for (size_t i = 0; i < imports->numImports; i++)
        {
            stringTable.emplace_back(pStrTable + paddedStringOffset);
            
            // pad the offset to the next multiple of 4
            paddedStringOffset += ((stringTable.back().length() + 1) + 3) & ~3;
        }

        // s32: walk to the END OF THE BLOB, not stringTable.size() times. `numImports` in
        // the header is the number of NAMES, not the number of records, and a XEX may
        // carry more records than names. The 2014 NXE dash.xex has THREE records --
        //     [0] xam.xex       593 thunks
        //     [1] xam.xex       425 thunks   (a second record for the same library)
        //     [2] xboxkrnl.exe  580 thunks
        // -- behind a string table of two. Looping twice silently dropped every kernel
        // import, and the boot died with `FATAL ERROR - Failed to find import 0x929174fc`,
        // which is one of that third record's thunks. Reading the guest memory there is
        // what gave it away: the 16-byte thunks are lis/addi/mtctr/bctr and the
        // unresolved ones encode type|libindex|ordinal, showing a library index of 1 in a
        // range no declared record covered.
        auto* library = (Xex2ImportLibrary*)(((char*)imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
        const auto* importsEnd = ((const char*)imports) + imports->sizeOfHeader;
        while ((const char*)library + sizeof(Xex2ImportLibrary) <= importsEnd && library->size != 0)
        {
            auto* descriptors = (Xex2ImportDescriptor*)(library + 1);
            static std::unordered_map<size_t, const char*> DummyExports;
            const std::unordered_map<size_t, const char*>* names = &DummyExports;

            // s32: pick the export map from THIS RECORD'S OWN name index, not from the
            // loop counter. A XEX may carry several import records for the SAME library
            // -- the 2014 NXE dash.xex has two, both xam.xex (593 + 425 thunks), and the
            // string table's second entry ("xboxkrnl.exe") belongs to no record at all.
            // Indexing by `i` therefore looked record 1's xam ordinals up in the KERNEL
            // export table, found nothing, emitted no Symbol_Import for any of them, and
            // the boot died with `FATAL ERROR - Failed to find import 0x9291670c` --
            // which is exactly one of that record's thunks.
            const std::string_view libName = library->name < stringTable.size()
                ? stringTable[library->name] : std::string_view{};

            if (libName == "xam.xex")
            {
                names = &XamExports;
            }
            else if (libName == "xboxkrnl.exe")
            {
                names = &XboxKernelExports;
            }

            for (size_t im = 0; im < library->numberOfImports; im++)
            {
                auto originalThunk = (Xex2ThunkData*)image.Find(descriptors[im].firstThunk);
                auto originalData = originalThunk;
                originalData->data = ByteSwap(originalData->data);

                if (originalData->originalData.type != 0)
                {
                    uint32_t thunk[4] = { 0x00000060, 0x00000060, 0x00000060, 0x2000804E };
                    const uint32_t ordinal = originalData->originalData.ordinal;
                    auto name = names->find(ordinal);

                    // s32: an ordinal our tables do not know STILL needs an import entry.
                    // Skipping it here means no Symbol_Import, so no row in
                    // PrecompiledImportTable, so Emu aborts the whole image load with
                    // `FATAL ERROR - Failed to find import <thunk va>`. Our xam table is
                    // Xenia's, which has 1735 entries for ordinals running to 2890 -- the
                    // 2014 NXE dash imports 2845, which is simply absent from it. Synthesise
                    // a name instead: the thunk is already overwritten with nop/nop/nop/blr,
                    // so an unresolvable import returns immediately rather than killing the
                    // load, and anything that actually depends on it fails visibly later.
                    std::string importName;
                    if (name != names->end())
                    {
                        importName = name->second;
                    }
                    else
                    {
                        std::string lib(libName);
                        const auto dot = lib.find('.');
                        if (dot != std::string::npos)
                            lib.resize(dot);
                        if (lib.empty())
                            lib = "unk";
                        importName = "__" + lib + "_" + std::to_string(ordinal);
                    }

                    image.symbols.insert({ importName, descriptors[im].firstThunk, sizeof(thunk), Symbol_Import });

                    memcpy(originalThunk, thunk, sizeof(thunk));
                }
            }
            library = (Xex2ImportLibrary*)((char*)(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
    }

    return image;
}
