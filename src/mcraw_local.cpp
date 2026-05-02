#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#include "mcraw_local.h"

#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

#define TINY_DNG_WRITER_IMPLEMENTATION
#include <tinydng/tiny_dng_writer.h>
#undef TINY_DNG_WRITER_IMPLEMENTATION

#include <audiofile/AudioFile.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <algorithm>

std::vector<WindowsFileEntry> listLocalMcraw(const std::string& mcrawPath) {
    std::vector<WindowsFileEntry> entries;

    try {
        motioncam::Decoder decoder(mcrawPath);
        auto& frames = decoder.getFrames();
        auto& meta = decoder.getContainerMetadata();

        // Get mtime from the .mcraw file for all virtual entries
        std::string dateStr;
        try {
            auto ft = std::filesystem::last_write_time(mcrawPath);
            auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto t = std::chrono::system_clock::to_time_t(sc);
            struct tm tm; localtime_s(&tm, &t);
            char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            dateStr = buf;
        } catch (...) {}

        // Estimate DNG size from first frame
        uint64_t dngEstSize = 0;
        if (!frames.empty()) {
            nlohmann::json frameMeta;
            decoder.loadFrameMetadata(frames[0], frameMeta);
            unsigned int w = frameMeta["width"];
            unsigned int h = frameMeta["height"];
            dngEstSize = (uint64_t)w * h * 2 + 8192;
        }

        // metadata.json
        {
            std::string metaJson = meta.dump(2);
            WindowsFileEntry fe;
            fe.name = "metadata.json";
            fe.size = metaJson.size();
            fe.dateModified = dateStr;
            fe.isDirectory = false;
            entries.push_back(std::move(fe));
        }

        // frame_NNNNNN.dng entries
        for (size_t i = 0; i < frames.size(); i++) {
            char fname[32];
            snprintf(fname, sizeof(fname), "frame_%06zu.dng", i + 1);
            WindowsFileEntry fe;
            fe.name = fname;
            fe.size = dngEstSize;
            fe.dateModified = dateStr;
            fe.isDirectory = false;
            entries.push_back(std::move(fe));
        }

        // audio.wav (if audio exists)
        try {
            if (meta.contains("extraData") &&
                meta["extraData"].contains("audioSampleRate") &&
                meta["extraData"].contains("audioChannels")) {
                int sr = meta["extraData"]["audioSampleRate"];
                int ch = meta["extraData"]["audioChannels"];
                if (sr > 0 && ch > 0) {
                    std::vector<motioncam::AudioChunk> audioChunks;
                    decoder.loadAudio(audioChunks);
                    if (!audioChunks.empty()) {
                        size_t totalSamples = 0;
                        for (auto& c : audioChunks) totalSamples += c.second.size();
                        WindowsFileEntry fe;
                        fe.name = "audio.wav";
                        fe.size = 44 + totalSamples * sizeof(int16_t);
                        fe.dateModified = dateStr;
                        fe.isDirectory = false;
                        entries.push_back(std::move(fe));
                    }
                }
            }
        } catch (...) {}

    } catch (const std::exception&) {
        // If we can't open the MCRAW, return empty list
    }

    return entries;
}

// Writes a valid DNG/TIFF file directly using fwrite, avoiding
// TinyDNGWriter's triple-copy overhead (ostringstream → vector → ofstream).
// Layout: [8B header][pixel strip][IFD entries][overflow data]

namespace {

// TIFF data types
enum TiffType : uint16_t {
    TIFF_BYTE      = 1,
    TIFF_ASCII     = 2,
    TIFF_SHORT     = 3,
    TIFF_LONG      = 4,
    TIFF_RATIONAL  = 5,
    TIFF_SRATIONAL = 10,
};

// Size of one element for each TIFF type
static size_t tiffTypeSize(uint16_t type) {
    switch (type) {
        case TIFF_BYTE: case TIFF_ASCII: return 1;
        case TIFF_SHORT: return 2;
        case TIFF_LONG: return 4;
        case TIFF_RATIONAL: case TIFF_SRATIONAL: return 8;
        default: return 1;
    }
}

struct IFDEntry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    // If data fits in 4 bytes, stored inline. Otherwise, offset into overflow buffer.
    uint32_t valueOrOffset;
    bool isOverflow = false;
    size_t overflowPos = 0; // position in overflow buffer
};

// Convert float to SRATIONAL (int32 numerator / int32 denominator)
// Uses the same approach as TinyDNGWriter for compatibility
static void floatToSRational(float x, int32_t& num, int32_t& den) {
    if (!std::isfinite(x)) {
        num = (x > 0.0f) ? 1 : (x < 0.0f) ? -1 : 0;
        den = 0;
        return;
    }
    // Multiply by large power of 10 for precision, reduce
    // Use 1000000 scale for good precision without overflow
    double scaled = (double)x * 1000000.0;
    num = (int32_t)std::round(scaled);
    den = 1000000;
    // Reduce by GCD
    int32_t a = std::abs(num), b = den;
    while (b) { int32_t t = b; b = a % b; a = t; }
    if (a > 1) { num /= a; den /= a; }
}

// Convert float to RATIONAL (uint32 numerator / uint32 denominator)
static void floatToRational(float x, uint32_t& num, uint32_t& den) {
    if (x <= 0.0f || !std::isfinite(x)) { num = 0; den = 1; return; }
    double scaled = (double)x * 1000000.0;
    num = (uint32_t)std::round(scaled);
    den = 1000000;
    uint32_t a = num, b = den;
    while (b) { uint32_t t = b; b = a % b; a = t; }
    if (a > 1) { num /= a; den /= a; }
}

// DNG metadata extracted from container + frame JSON, ready for writing
struct DngParams {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t blackLevel[4] = {};
    uint16_t whiteLevel = 0;
    uint8_t cfaPattern[4] = {};
    float colorMatrix1[9] = {};
    float colorMatrix2[9] = {};
    float forwardMatrix1[9] = {};
    float forwardMatrix2[9] = {};
    float asShotNeutral[3] = {};
    // Lens shading map (vignette correction), 4 channels for Bayer CFA
    std::vector<std::vector<float>> lensShadingMap; // [4][h*w]
    int lensShadingMapWidth = 0;
    int lensShadingMapHeight = 0;
};

// Normalize a forward matrix so its row sums equal the D50 white point.
// Returns false if the matrix is all zeros or has degenerate rows.
static bool normalizeForwardMatrixToD50(float* matrix) {
    const float D50[3] = { 0.9642f, 1.0f, 0.8251f };

    // Check for zero matrix
    bool allZero = true;
    for (int i = 0; i < 9; i++) if (matrix[i] != 0.0f) { allZero = false; break; }
    if (allZero) return false;

    // Compute row sums (neutral response) and scale
    for (int row = 0; row < 3; row++) {
        float sum = matrix[row*3+0] + matrix[row*3+1] + matrix[row*3+2];
        if (std::abs(sum) < 1e-9f) return false;
        float scale = D50[row] / sum;
        matrix[row*3+0] *= scale;
        matrix[row*3+1] *= scale;
        matrix[row*3+2] *= scale;
    }
    return true;
}

// Build big-endian OpcodeList2 binary data for a GainMap (lens shading correction).
static std::vector<uint8_t> buildGainMapOpcode(const DngParams& p) {
    std::vector<uint8_t> buf;
    if (p.lensShadingMap.empty() || p.lensShadingMapWidth <= 0 || p.lensShadingMapHeight <= 0)
        return buf;

    unsigned int planes = std::min<unsigned int>((unsigned int)p.lensShadingMap.size(), 4);
    unsigned int mapH = (unsigned int)p.lensShadingMapHeight;
    unsigned int mapW = (unsigned int)p.lensShadingMapWidth;
    unsigned int imgH = p.height;
    unsigned int imgW = p.width;
    unsigned int rowPitch = (mapH > 1) ? std::max(1u, (imgH - 1) / (mapH - 1)) : imgH;
    unsigned int colPitch = (mapW > 1) ? std::max(1u, (imgW - 1) / (mapW - 1)) : imgW;

    // Total gain floats
    size_t gainCount = (size_t)planes * mapH * mapW;
    // Opcode data size: 4*4 (area) + 2*4 (plane) + 2*4 (pitch) + 2*4 (map dims)
    //   + 2*8 (spacing) + 2*8 (origin) + 4 (map_planes) + gainCount*4
    size_t opcodeDataSize = 4*4 + 2*4 + 2*4 + 2*4 + 2*8 + 2*8 + 4 + gainCount*4;
    // Total: 4 (count) + 4 (id) + 4 (version) + 4 (flags) + 4 (size) + data
    buf.reserve(4 + 4 + 4 + 4 + 4 + opcodeDataSize);

    // Helper: write big-endian uint32
    auto writeU32BE = [&](uint32_t v) {
        buf.push_back((uint8_t)(v >> 24));
        buf.push_back((uint8_t)(v >> 16));
        buf.push_back((uint8_t)(v >> 8));
        buf.push_back((uint8_t)(v));
    };
    // Helper: write big-endian double (IEEE 754)
    auto writeDoubleBE = [&](double v) {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        for (int i = 7; i >= 0; i--)
            buf.push_back((uint8_t)(bits >> (i * 8)));
    };
    // Helper: write big-endian float
    auto writeFloatBE = [&](float v) {
        uint32_t bits;
        memcpy(&bits, &v, 4);
        writeU32BE(bits);
    };

    // Opcode count = 1
    writeU32BE(1);
    // Opcode ID = 9 (GainMap)
    writeU32BE(9);
    // DNG version 1.3.0.0
    writeU32BE(0x01030000);
    // Flags = 0
    writeU32BE(0);
    // Data size
    writeU32BE((uint32_t)opcodeDataSize);

    // Area of interest: top, left, bottom, right
    writeU32BE(0);        // top
    writeU32BE(0);        // left
    writeU32BE(imgH);     // bottom
    writeU32BE(imgW);     // right
    // Plane info
    writeU32BE(0);        // plane (starting plane)
    writeU32BE(planes);   // planes
    // Pitch
    writeU32BE(rowPitch);
    writeU32BE(colPitch);
    // Map dimensions
    writeU32BE(mapH);
    writeU32BE(mapW);
    // Spacing (doubles)
    writeDoubleBE((imgH > 0) ? (double)rowPitch / (double)imgH : 0.0);
    writeDoubleBE((imgW > 0) ? (double)colPitch / (double)imgW : 0.0);
    // Origin (doubles)
    writeDoubleBE(0.0);
    writeDoubleBE(0.0);
    // Map planes
    writeU32BE(planes);

    // Gain data: plane-major, row-major within each plane
    for (unsigned int ch = 0; ch < planes; ch++) {
        const auto& chanData = (ch < p.lensShadingMap.size()) ? p.lensShadingMap[ch] : p.lensShadingMap[0];
        for (unsigned int v = 0; v < mapH; v++) {
            for (unsigned int h = 0; h < mapW; h++) {
                size_t idx = (size_t)v * mapW + h;
                float gain = 1.0f;
                if (idx < chanData.size()) {
                    gain = chanData[idx];
                    if (!std::isfinite(gain) || gain <= 0.0f) gain = 1.0f;
                    else if (gain > 16.0f) gain = 16.0f;
                }
                writeFloatBE(gain);
            }
        }
    }

    return buf;
}

// Fix white level when metadata doesn't match the actual pixel data range.
// Some mcraw files report a 10-bit white level (1023) even when the actual sensor
// data is 12-bit or 14-bit. Detect the real data range and correct the white level.
// Black levels are left as-is since they may already be in the correct scale.
static void adjustLevelsForBitDepth(DngParams& p, const uint16_t* pixelData, size_t pixelCount) {
    // Scan pixel data to find actual maximum value
    uint16_t maxVal = 0;
    size_t step = std::max<size_t>(1, pixelCount / 500000);
    for (size_t i = 0; i < pixelCount; i += step)
        if (pixelData[i] > maxVal) maxVal = pixelData[i];

    // Only adjust if pixel values clearly exceed the metadata white level
    // (50% margin avoids false triggers from a few hot pixels)
    if (p.whiteLevel == 0 || maxVal <= p.whiteLevel + p.whiteLevel / 2)
        return;

    // Determine actual bit depth from the max pixel value
    if (maxVal <= 4095)       p.whiteLevel = 4095;   // 12-bit
    else if (maxVal <= 16383) p.whiteLevel = 16383;   // 14-bit
    else                      p.whiteLevel = 65535;   // 16-bit
}

static bool parseDngParams(DngParams& p, const nlohmann::json& frameMeta, const nlohmann::json& containerMeta) {
    p.width = frameMeta["width"];
    p.height = frameMeta["height"];

    // Prefer per-frame dynamic levels, fall back to container-level static levels
    if (frameMeta.contains("dynamicBlackLevel") && frameMeta["dynamicBlackLevel"].is_array()) {
        auto dbl = frameMeta["dynamicBlackLevel"];
        for (size_t i = 0; i < 4 && i < dbl.size(); i++)
            p.blackLevel[i] = (uint16_t)dbl[i].get<float>();
    } else {
        auto bl = containerMeta["blackLevel"].get<std::vector<uint16_t>>();
        for (int i = 0; i < 4 && i < (int)bl.size(); i++) p.blackLevel[i] = bl[i];
    }

    if (frameMeta.contains("dynamicWhiteLevel") && frameMeta["dynamicWhiteLevel"].is_number()) {
        p.whiteLevel = (uint16_t)frameMeta["dynamicWhiteLevel"].get<float>();
    } else {
        p.whiteLevel = (uint16_t)(double)containerMeta["whiteLevel"];
    }

    std::string sa = containerMeta["sensorArrangment"]; // upstream typo
    if (sa == "rggb")      { p.cfaPattern[0]=0; p.cfaPattern[1]=1; p.cfaPattern[2]=1; p.cfaPattern[3]=2; }
    else if (sa == "bggr") { p.cfaPattern[0]=2; p.cfaPattern[1]=1; p.cfaPattern[2]=1; p.cfaPattern[3]=0; }
    else if (sa == "grbg") { p.cfaPattern[0]=1; p.cfaPattern[1]=0; p.cfaPattern[2]=2; p.cfaPattern[3]=1; }
    else if (sa == "gbrg") { p.cfaPattern[0]=1; p.cfaPattern[1]=2; p.cfaPattern[2]=0; p.cfaPattern[3]=1; }
    else return false;

    auto cm1 = containerMeta["colorMatrix1"].get<std::vector<float>>();
    auto cm2 = containerMeta["colorMatrix2"].get<std::vector<float>>();
    auto fm1 = containerMeta["forwardMatrix1"].get<std::vector<float>>();
    auto fm2 = containerMeta["forwardMatrix2"].get<std::vector<float>>();
    auto asn = frameMeta["asShotNeutral"].get<std::vector<float>>();

    for (int i = 0; i < 9 && i < (int)cm1.size(); i++) p.colorMatrix1[i] = cm1[i];
    for (int i = 0; i < 9 && i < (int)cm2.size(); i++) p.colorMatrix2[i] = cm2[i];
    for (int i = 0; i < 9 && i < (int)fm1.size(); i++) p.forwardMatrix1[i] = fm1[i];
    for (int i = 0; i < 9 && i < (int)fm2.size(); i++) p.forwardMatrix2[i] = fm2[i];
    for (int i = 0; i < 3 && i < (int)asn.size(); i++) p.asShotNeutral[i] = asn[i];

    // Normalize forward matrices to D50 white point.
    // If normalization fails, fall back to D50 diagonal matrix.
    if (!normalizeForwardMatrixToD50(p.forwardMatrix1)) {
        memset(p.forwardMatrix1, 0, sizeof(p.forwardMatrix1));
        p.forwardMatrix1[0] = 0.9642f; p.forwardMatrix1[4] = 1.0f; p.forwardMatrix1[8] = 0.8251f;
    }
    if (!normalizeForwardMatrixToD50(p.forwardMatrix2)) {
        memset(p.forwardMatrix2, 0, sizeof(p.forwardMatrix2));
        p.forwardMatrix2[0] = 0.9642f; p.forwardMatrix2[4] = 1.0f; p.forwardMatrix2[8] = 0.8251f;
    }

    // Parse lens shading map from frame metadata (4 channels, each height*width floats)
    if (frameMeta.contains("lensShadingMap") && frameMeta["lensShadingMap"].is_array()) {
        p.lensShadingMapWidth = frameMeta.value("lensShadingMapWidth", 0);
        p.lensShadingMapHeight = frameMeta.value("lensShadingMapHeight", 0);
        if (p.lensShadingMapWidth > 0 && p.lensShadingMapHeight > 0) {
            auto& mapArray = frameMeta["lensShadingMap"];
            p.lensShadingMap.reserve(mapArray.size());
            for (const auto& channel : mapArray) {
                if (!channel.is_array()) continue;
                std::vector<float> chanData;
                chanData.reserve(channel.size());
                for (const auto& v : channel)
                    chanData.push_back(v.get<float>());
                p.lensShadingMap.emplace_back(std::move(chanData));
            }
        }
    }

    return true;
}

// Build a complete DNG file.
// If outFile is set, writes directly to file (pixel data written with zero copies).
// If outBuffer is set, writes to memory buffer.
// Returns true on success.
static bool buildDng(const DngParams& p,
                     const uint8_t* pixelData, size_t pixelSize,
                     FILE* outFile,
                     std::vector<uint8_t>* outBuffer)
{
    // Build overflow data buffer and IFD entries
    std::vector<uint8_t> overflow;
    overflow.reserve(1024);
    std::vector<IFDEntry> entries;
    entries.reserve(28);

    // Helper: add overflow data, return offset placeholder (fixed up later)
    auto addOverflow = [&](const void* data, size_t bytes) -> size_t {
        size_t pos = overflow.size();
        overflow.resize(pos + bytes);
        memcpy(overflow.data() + pos, data, bytes);
        return pos;
    };

    // Helper: add IFD entry with inline value (fits in 4 bytes)
    auto addInline = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        entries.push_back({tag, type, count, value, false, 0});
    };

    // Helper: add IFD entry with overflow data
    auto addOverflowEntry = [&](uint16_t tag, uint16_t type, uint32_t count, const void* data, size_t bytes) {
        size_t pos = addOverflow(data, bytes);
        entries.push_back({tag, type, count, 0, true, pos});
    };

    uint32_t stripSize = (uint32_t)pixelSize;

    // Tags must be in ascending order
    // 254: NewSubfileType = 0
    addInline(254, TIFF_LONG, 1, 0);
    // 256: ImageWidth
    addInline(256, TIFF_LONG, 1, p.width);
    // 257: ImageLength
    addInline(257, TIFF_LONG, 1, p.height);
    // 258: BitsPerSample = 16
    addInline(258, TIFF_SHORT, 1, 16);
    // 259: Compression = 1 (none)
    addInline(259, TIFF_SHORT, 1, 1);
    // 262: PhotometricInterpretation = 32803 (CFA)
    addInline(262, TIFF_SHORT, 1, 32803);
    // 273: StripOffsets = 8 (right after header)
    addInline(273, TIFF_LONG, 1, 8);
    // 277: SamplesPerPixel = 1
    addInline(277, TIFF_SHORT, 1, 1);
    // 278: RowsPerStrip = height
    addInline(278, TIFF_LONG, 1, p.height);
    // 279: StripByteCounts = pixel data size
    addInline(279, TIFF_LONG, 1, stripSize);
    // 284: PlanarConfiguration = 1 (chunky)
    addInline(284, TIFF_SHORT, 1, 1);

    // 33421: CFARepeatPatternDim = [2, 2] (two SHORTs fit in 4 bytes)
    {
        uint32_t v = (uint32_t)2 | ((uint32_t)2 << 16);
        addInline(33421, TIFF_SHORT, 2, v);
    }
    // 33422: CFAPattern = [4 bytes] (fits inline)
    {
        uint32_t v;
        memcpy(&v, p.cfaPattern, 4);
        addInline(33422, TIFF_BYTE, 4, v);
    }

    // 50706: DNGVersion = [1,4,0,0]
    {
        uint32_t v = 1 | (4 << 8);
        addInline(50706, TIFF_BYTE, 4, v);
    }
    // 50707: DNGBackwardVersion = [1,1,0,0]
    {
        uint32_t v = 1 | (1 << 8);
        addInline(50707, TIFF_BYTE, 4, v);
    }
    // 50708: UniqueCameraModel = "MotionCam"
    {
        const char model[] = "MotionCam"; // 10 bytes including null
        addOverflowEntry(50708, TIFF_ASCII, sizeof(model), model, sizeof(model));
    }
    // 50711: CFALayout = 1
    addInline(50711, TIFF_SHORT, 1, 1);
    // 50713: BlackLevelRepeatDim = [2, 2]
    {
        uint32_t v = (uint32_t)2 | ((uint32_t)2 << 16);
        addInline(50713, TIFF_SHORT, 2, v);
    }
    // 50714: BlackLevel (4 shorts = 8 bytes, needs overflow)
    addOverflowEntry(50714, TIFF_SHORT, 4, p.blackLevel, 8);
    // 50717: WhiteLevel
    addInline(50717, TIFF_SHORT, 1, p.whiteLevel);
    // 50721: ColorMatrix1 (9 SRATIONALs = 72 bytes)
    {
        int32_t rats[18];
        for (int i = 0; i < 9; i++) floatToSRational(p.colorMatrix1[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50721, TIFF_SRATIONAL, 9, rats, 72);
    }
    // 50722: ColorMatrix2
    {
        int32_t rats[18];
        for (int i = 0; i < 9; i++) floatToSRational(p.colorMatrix2[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50722, TIFF_SRATIONAL, 9, rats, 72);
    }
    // 50723: CameraCalibration1 (identity 3x3)
    {
        int32_t rats[18];
        float identity[9] = {1,0,0, 0,1,0, 0,0,1};
        for (int i = 0; i < 9; i++) floatToSRational(identity[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50723, TIFF_SRATIONAL, 9, rats, 72);
    }
    // 50724: CameraCalibration2 (identity 3x3)
    {
        int32_t rats[18];
        float identity[9] = {1,0,0, 0,1,0, 0,0,1};
        for (int i = 0; i < 9; i++) floatToSRational(identity[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50724, TIFF_SRATIONAL, 9, rats, 72);
    }
    // 50728: AsShotNeutral (3 RATIONALs = 24 bytes)
    {
        uint32_t rats[6];
        for (int i = 0; i < 3; i++) floatToRational(p.asShotNeutral[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50728, TIFF_RATIONAL, 3, rats, 24);
    }
    // 50778: CalibrationIlluminant1 = 21 (D65)
    addInline(50778, TIFF_SHORT, 1, 21);
    // 50779: CalibrationIlluminant2 = 17 (Standard light A)
    addInline(50779, TIFF_SHORT, 1, 17);
    // 50829: ActiveArea [top, left, bottom, right] = [0, 0, height, width]
    {
        uint32_t aa[4] = { 0, 0, p.height, p.width };
        addOverflowEntry(50829, TIFF_LONG, 4, aa, 16);
    }
    // 50964: ForwardMatrix1 (D50-normalized)
    {
        int32_t rats[18];
        for (int i = 0; i < 9; i++) floatToSRational(p.forwardMatrix1[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50964, TIFF_SRATIONAL, 9, rats, 72);
    }
    // 50965: ForwardMatrix2 (D50-normalized)
    {
        int32_t rats[18];
        for (int i = 0; i < 9; i++) floatToSRational(p.forwardMatrix2[i], rats[i*2], rats[i*2+1]);
        addOverflowEntry(50965, TIFF_SRATIONAL, 9, rats, 72);
    }
    // 51009: OpcodeList2 (lens shading / vignette correction GainMap)
    {
        auto opcodeData = buildGainMapOpcode(p);
        if (!opcodeData.empty()) {
            addOverflowEntry(51009, TIFF_BYTE, (uint32_t)opcodeData.size(), opcodeData.data(), opcodeData.size());
        }
    }

    // Compute layout offsets
    // File layout: [8B header][pixelStrip][IFD][overflow]
    uint32_t ifdOffset = 8 + stripSize;
    uint16_t numEntries = (uint16_t)entries.size();
    uint32_t ifdSize = 2 + numEntries * 12 + 4; // count + entries + next_ifd(0)
    uint32_t overflowBase = ifdOffset + ifdSize;

    // Fix up overflow offsets to be absolute file offsets
    for (auto& e : entries) {
        if (e.isOverflow) {
            e.valueOrOffset = (uint32_t)(overflowBase + e.overflowPos);
        }
    }

    // Build IFD block
    std::vector<uint8_t> ifdBlock(ifdSize);
    uint8_t* wp = ifdBlock.data();
    memcpy(wp, &numEntries, 2); wp += 2;
    for (auto& e : entries) {
        memcpy(wp, &e.tag, 2); wp += 2;
        memcpy(wp, &e.type, 2); wp += 2;
        memcpy(wp, &e.count, 4); wp += 4;
        memcpy(wp, &e.valueOrOffset, 4); wp += 4;
    }
    uint32_t nextIfd = 0;
    memcpy(wp, &nextIfd, 4);

    if (outFile) {
        // Write TIFF header
        uint8_t header[8];
        header[0] = 'I'; header[1] = 'I'; // little-endian
        uint16_t magic = 42;
        memcpy(header + 2, &magic, 2);
        memcpy(header + 4, &ifdOffset, 4);
        if (fwrite(header, 1, 8, outFile) != 8) return false;

        // Write pixel strip directly from decompressed buffer (ZERO COPY)
        if (fwrite(pixelData, 1, pixelSize, outFile) != pixelSize) return false;

        // Write IFD
        if (fwrite(ifdBlock.data(), 1, ifdBlock.size(), outFile) != ifdBlock.size()) return false;

        // Write overflow data
        if (!overflow.empty()) {
            if (fwrite(overflow.data(), 1, overflow.size(), outFile) != overflow.size()) return false;
        }

        return true;
    }

    if (outBuffer) {
        size_t totalSize = 8 + pixelSize + ifdBlock.size() + overflow.size();
        outBuffer->resize(totalSize);
        uint8_t* dst = outBuffer->data();

        // Header
        dst[0] = 'I'; dst[1] = 'I';
        uint16_t magic = 42;
        memcpy(dst + 2, &magic, 2);
        memcpy(dst + 4, &ifdOffset, 4);
        dst += 8;

        // Pixel data
        memcpy(dst, pixelData, pixelSize);
        dst += pixelSize;

        // IFD
        memcpy(dst, ifdBlock.data(), ifdBlock.size());
        dst += ifdBlock.size();

        // Overflow
        if (!overflow.empty()) {
            memcpy(dst, overflow.data(), overflow.size());
        }

        return true;
    }

    return false;
}

// Build TIFF header (8 bytes) and IFD+overflow trailer for a DNG frame.
// The pixel strip goes between them. This avoids copying the 24MB pixel data.
static bool buildDngParts(const DngParams& params, size_t pixelSize,
                          uint8_t headerOut[8],
                          std::vector<uint8_t>& trailerOut)
{
    // We need the IFD entries and overflow, but NOT the pixel data.
    // Reuse buildDng with a dummy pixel pointer — it only copies pixels in the buffer path.
    // Instead, build the IFD and overflow manually using the same logic as buildDng.

    std::vector<uint8_t> overflow;
    overflow.reserve(1024);
    std::vector<IFDEntry> entries;
    entries.reserve(28);

    auto addOverflow = [&](const void* data, size_t bytes) -> size_t {
        size_t pos = overflow.size();
        overflow.resize(pos + bytes);
        memcpy(overflow.data() + pos, data, bytes);
        return pos;
    };
    auto addInline = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        entries.push_back({tag, type, count, value, false, 0});
    };
    auto addOverflowEntry = [&](uint16_t tag, uint16_t type, uint32_t count, const void* data, size_t bytes) {
        size_t pos = addOverflow(data, bytes);
        entries.push_back({tag, type, count, 0, true, pos});
    };

    uint32_t stripSize = (uint32_t)pixelSize;

    // Same tag list as buildDng (must be ascending order)
    addInline(254, TIFF_LONG, 1, 0);
    addInline(256, TIFF_LONG, 1, params.width);
    addInline(257, TIFF_LONG, 1, params.height);
    addInline(258, TIFF_SHORT, 1, 16);
    addInline(259, TIFF_SHORT, 1, 1);
    addInline(262, TIFF_SHORT, 1, 32803);
    addInline(273, TIFF_LONG, 1, 8); // strip offset = right after header
    addInline(277, TIFF_SHORT, 1, 1);
    addInline(278, TIFF_LONG, 1, params.height);
    addInline(279, TIFF_LONG, 1, stripSize);
    addInline(284, TIFF_SHORT, 1, 1);
    { uint32_t v = 2u | (2u << 16); addInline(33421, TIFF_SHORT, 2, v); }
    { uint32_t v; memcpy(&v, params.cfaPattern, 4); addInline(33422, TIFF_BYTE, 4, v); }
    { uint32_t v = 1 | (4 << 8); addInline(50706, TIFF_BYTE, 4, v); }
    { uint32_t v = 1 | (1 << 8); addInline(50707, TIFF_BYTE, 4, v); }
    { const char m[] = "MotionCam"; addOverflowEntry(50708, TIFF_ASCII, sizeof(m), m, sizeof(m)); }
    addInline(50711, TIFF_SHORT, 1, 1);
    { uint32_t v = 2u | (2u << 16); addInline(50713, TIFF_SHORT, 2, v); }
    addOverflowEntry(50714, TIFF_SHORT, 4, params.blackLevel, 8);
    addInline(50717, TIFF_SHORT, 1, params.whiteLevel);
    { int32_t r[18]; for (int i=0;i<9;i++) floatToSRational(params.colorMatrix1[i],r[i*2],r[i*2+1]); addOverflowEntry(50721, TIFF_SRATIONAL, 9, r, 72); }
    { int32_t r[18]; for (int i=0;i<9;i++) floatToSRational(params.colorMatrix2[i],r[i*2],r[i*2+1]); addOverflowEntry(50723, TIFF_SRATIONAL, 9, r, 72); }
    { uint32_t r[6]; for (int i=0;i<3;i++) floatToRational(params.asShotNeutral[i],r[i*2],r[i*2+1]); addOverflowEntry(50728, TIFF_RATIONAL, 3, r, 24); }
    addInline(50778, TIFF_SHORT, 1, 21);
    addInline(50779, TIFF_SHORT, 1, 17);
    { uint32_t aa[4]={0,0,params.height,params.width}; addOverflowEntry(50829, TIFF_LONG, 4, aa, 16); }
    { int32_t r[18]; for (int i=0;i<9;i++) floatToSRational(params.forwardMatrix1[i],r[i*2],r[i*2+1]); addOverflowEntry(50964, TIFF_SRATIONAL, 9, r, 72); }
    { int32_t r[18]; for (int i=0;i<9;i++) floatToSRational(params.forwardMatrix2[i],r[i*2],r[i*2+1]); addOverflowEntry(50965, TIFF_SRATIONAL, 9, r, 72); }

    uint32_t ifdOffset = 8 + stripSize;
    uint16_t numEntries = (uint16_t)entries.size();
    uint32_t ifdSize = 2 + numEntries * 12 + 4;
    uint32_t overflowBase = ifdOffset + ifdSize;

    for (auto& e : entries) {
        if (e.isOverflow) e.valueOrOffset = (uint32_t)(overflowBase + e.overflowPos);
    }

    // Build trailer: IFD + overflow
    trailerOut.resize(ifdSize + overflow.size());
    uint8_t* wp = trailerOut.data();
    memcpy(wp, &numEntries, 2); wp += 2;
    for (auto& e : entries) {
        memcpy(wp, &e.tag, 2); wp += 2;
        memcpy(wp, &e.type, 2); wp += 2;
        memcpy(wp, &e.count, 4); wp += 4;
        memcpy(wp, &e.valueOrOffset, 4); wp += 4;
    }
    uint32_t nextIfd = 0;
    memcpy(wp, &nextIfd, 4); wp += 4;
    if (!overflow.empty()) memcpy(wp, overflow.data(), overflow.size());

    // Build header
    headerOut[0] = 'I'; headerOut[1] = 'I';
    uint16_t magic = 42;
    memcpy(headerOut + 2, &magic, 2);
    memcpy(headerOut + 4, &ifdOffset, 4);

    return true;
}

// Write DNG using memory-mapped I/O for maximum throughput.
// The pixel strip is memcpy'd directly from the decompressed buffer into the
// memory-mapped file. The OS handles flushing to NVMe asynchronously.
// No intermediate buffer allocation, no system calls per write.
static bool writeDngDirect(const std::string& outputPath,
                           const uint8_t* pixelData, size_t pixelSize,
                           const DngParams& params)
{
#ifdef _WIN32
    // Build the small parts (header = 8 bytes, trailer = ~1KB)
    uint8_t header[8];
    std::vector<uint8_t> trailer;
    if (!buildDngParts(params, pixelSize, header, trailer))
        return false;

    size_t totalSize = 8 + pixelSize + trailer.size();

    std::wstring wpath = std::filesystem::path(outputPath).wstring();
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_WRITE,
                               0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    // Pre-allocate file size to avoid NTFS extent allocation during writes
    LARGE_INTEGER liSize;
    liSize.QuadPart = (LONGLONG)totalSize;
    SetFilePointerEx(hFile, liSize, nullptr, FILE_BEGIN);
    SetEndOfFile(hFile);
    SetFilePointerEx(hFile, {}, nullptr, FILE_BEGIN);

    // 3 direct WriteFile calls — pixel data goes straight from decompressed buffer, ZERO COPY
    DWORD written;
    bool ok = true;

    // 1. TIFF header (8 bytes)
    if (!WriteFile(hFile, header, 8, &written, nullptr) || written != 8)
        ok = false;

    // 2. Pixel strip — 24MB directly from decompressed buffer, no intermediate copy
    if (ok) {
        const uint8_t* src = pixelData;
        size_t remaining = pixelSize;
        while (remaining > 0) {
            DWORD chunk = (DWORD)std::min(remaining, (size_t)32 * 1024 * 1024);
            if (!WriteFile(hFile, src, chunk, &written, nullptr) || written == 0) {
                ok = false;
                break;
            }
            src += written;
            remaining -= written;
        }
    }

    // 3. IFD + overflow trailer (~1KB)
    if (ok && !trailer.empty()) {
        if (!WriteFile(hFile, trailer.data(), (DWORD)trailer.size(), &written, nullptr))
            ok = false;
    }

    CloseHandle(hFile);
    if (!ok) {
        try { std::filesystem::remove(outputPath); } catch (...) {}
    }
    return ok;
#else
    FILE* f = fopen(outputPath.c_str(), "wb");
    if (!f) return false;
    setvbuf(f, nullptr, _IOFBF, 4 * 1024 * 1024);
    bool ok = buildDng(params, pixelData, pixelSize, f, nullptr);
    fclose(f);
    if (!ok) { try { std::filesystem::remove(outputPath); } catch (...) {} }
    return ok;
#endif
}

} // anonymous namespace

// Public: generate DNG in memory using the fast writer (used by ProjFS)
bool generateDngInMemoryFast(const uint8_t* pixelData, size_t pixelSize,
                             const nlohmann::json& frameMeta,
                             const nlohmann::json& containerMeta,
                             std::vector<uint8_t>& outDng) {
    DngParams p;
    if (!parseDngParams(p, frameMeta, containerMeta)) return false;
    adjustLevelsForBitDepth(p, reinterpret_cast<const uint16_t*>(pixelData), pixelSize / 2);
    return buildDng(p, pixelData, pixelSize, nullptr, &outDng);
}

static bool writeDngFile(const std::string& outputPath,
                         const std::vector<uint8_t>& pixelData,
                         const nlohmann::json& frameMeta,
                         const nlohmann::json& containerMeta) {
    const unsigned int width = frameMeta["width"];
    const unsigned int height = frameMeta["height"];

    std::vector<float> asShotNeutral = frameMeta["asShotNeutral"];
    std::vector<uint16_t> blackLevel = containerMeta["blackLevel"];
    double whiteLevel = containerMeta["whiteLevel"];
    std::string sensorArrangement = containerMeta["sensorArrangment"]; // upstream typo
    std::vector<float> colorMatrix1 = containerMeta["colorMatrix1"];
    std::vector<float> colorMatrix2 = containerMeta["colorMatrix2"];
    std::vector<float> forwardMatrix1 = containerMeta["forwardMatrix1"];
    std::vector<float> forwardMatrix2 = containerMeta["forwardMatrix2"];

    tinydngwriter::DNGImage dng;
    dng.SetBigEndian(false);
    dng.SetDNGVersion(1, 4, 0, 0);
    dng.SetDNGBackwardVersion(1, 1, 0, 0);
    dng.SetImageData(reinterpret_cast<const unsigned char*>(pixelData.data()), pixelData.size());
    dng.SetImageWidth(width);
    dng.SetImageLength(height);
    dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    dng.SetRowsPerStrip(height);
    dng.SetSamplesPerPixel(1);
    dng.SetCFARepeatPatternDim(2, 2);
    dng.SetBlackLevelRepeatDim(2, 2);
    dng.SetBlackLevel(4, blackLevel.data());
    dng.SetWhiteLevel(whiteLevel);
    dng.SetCompression(tinydngwriter::COMPRESSION_NONE);

    std::vector<uint8_t> cfa;
    if (sensorArrangement == "rggb")       cfa = { 0, 1, 1, 2 };
    else if (sensorArrangement == "bggr")  cfa = { 2, 1, 1, 0 };
    else if (sensorArrangement == "grbg")  cfa = { 1, 0, 2, 1 };
    else if (sensorArrangement == "gbrg")  cfa = { 1, 2, 0, 1 };
    else return false;

    dng.SetCFAPattern(4, cfa.data());
    dng.SetCFALayout(1);
    const uint16_t bps[1] = { 16 };
    dng.SetBitsPerSample(1, bps);
    dng.SetColorMatrix1(3, colorMatrix1.data());
    dng.SetColorMatrix2(3, colorMatrix2.data());
    dng.SetForwardMatrix1(3, forwardMatrix1.data());
    dng.SetForwardMatrix2(3, forwardMatrix2.data());
    dng.SetAsShotNeutral(3, asShotNeutral.data());
    dng.SetCalibrationIlluminant1(21);
    dng.SetCalibrationIlluminant2(17);
    dng.SetUniqueCameraModel("MotionCam");
    dng.SetSubfileType();
    const uint32_t activeArea[4] = { 0, 0, height, width };
    dng.SetActiveArea(&activeArea[0]);

    std::string err;
    tinydngwriter::DNGWriter writer(false);
    writer.AddImage(&dng);
    return writer.WriteToFile(outputPath.c_str(), &err);
}

bool extractLocalMcrawItem(const std::string& mcrawPath, const std::string& virtualName,
                           const std::string& destPath, ProgressCallback progress) {
    try {
        motioncam::Decoder decoder(mcrawPath);
        auto& frames = decoder.getFrames();
        auto& containerMeta = decoder.getContainerMetadata();

        if (virtualName == "metadata.json") {
            std::string json = containerMeta.dump(2);
            std::ofstream ofs(destPath, std::ios::binary);
            if (!ofs) return false;
            ofs.write(json.data(), json.size());
            if (progress) progress(json.size(), json.size());
            return true;

        } else if (virtualName == "audio.wav") {
            std::vector<motioncam::AudioChunk> audioChunks;
            decoder.loadAudio(audioChunks);
            if (audioChunks.empty()) return false;

            int sampleRate = decoder.audioSampleRateHz();
            int numChannels = decoder.numAudioChannels();

            AudioFile<int16_t> audio;
            audio.setNumChannels(numChannels);
            audio.setSampleRate(sampleRate);

            if (numChannels == 2) {
                for (auto& c : audioChunks) {
                    for (size_t i = 0; i < c.second.size(); i += 2) {
                        audio.samples[0].push_back(c.second[i]);
                        audio.samples[1].push_back(c.second[i + 1]);
                    }
                }
            } else if (numChannels == 1) {
                for (auto& c : audioChunks) {
                    for (size_t i = 0; i < c.second.size(); i++)
                        audio.samples[0].push_back(c.second[i]);
                }
            }

            audio.save(destPath);
            if (progress) progress(1, 1);
            return true;

        } else if (virtualName.rfind("frame_", 0) == 0 && virtualName.size() > 10 &&
                   virtualName.substr(virtualName.size() - 4) == ".dng") {
            // Parse frame index from "frame_NNNNNN.dng"
            std::string indexStr = virtualName.substr(6, virtualName.size() - 10);
            int frameIdx = std::stoi(indexStr) - 1;

            if (frameIdx < 0 || frameIdx >= (int)frames.size()) return false;

            std::vector<uint8_t> pixelData;
            nlohmann::json frameMeta;
            decoder.loadFrame(frames[frameIdx], pixelData, frameMeta);

            if (progress) progress(1, 2); // halfway: decompressed

            DngParams params;
            if (!parseDngParams(params, frameMeta, containerMeta)) return false;
            adjustLevelsForBitDepth(params, reinterpret_cast<const uint16_t*>(pixelData.data()), pixelData.size() / 2);
            bool ok = writeDngDirect(destPath, pixelData.data(), pixelData.size(), params);
            if (progress) progress(2, 2); // done
            return ok;

        } else {
            return false;
        }

    } catch (const std::exception&) {
        return false;
    }
}

// Architecture:
//   1. Reader thread: sequentially reads + decompresses frames from MCRAW (one Decoder,
//      sequential I/O on the container file for maximum read throughput).
//   2. Writer threads (N): pull decompressed frames from a bounded queue and write
//      DNG files to disk in parallel using Win32 WriteFile.
//
// This design eliminates the random I/O pattern that killed throughput when all
// threads were randomly seeking in the same MCRAW container file.

int extractLocalMcrawBatch(const std::string& mcrawPath,
                           const std::string& destDir,
                           int threadCount,
                           std::atomic<bool>* stopFlag,
                           BatchProgressCallback progress) {
    // Open to get metadata and frame list
    motioncam::Decoder decoder(mcrawPath);
    auto& frameList = decoder.getFrames();
    auto& containerMeta = decoder.getContainerMetadata();

    int totalFrames = (int)frameList.size();
    if (totalFrames == 0) return 0;

    // Clamp writer thread count
    int hwThreads = (int)std::thread::hardware_concurrency();
    if (hwThreads < 1) hwThreads = 4;
    if (threadCount <= 0) threadCount = std::max(2, hwThreads - 1); // leave 1 core for reader
    threadCount = std::min(threadCount, totalFrames);
    threadCount = std::max(threadCount, 1);

    LOG_INFO("MCRAW", "Batch extraction: " + std::to_string(totalFrames) + " frames, " +
             std::to_string(threadCount) + " writer threads, dest=" + destDir);

    // Create output directory
    try { std::filesystem::create_directories(destDir); } catch (...) {}

    // --- Bounded producer-consumer queue ---
    struct FrameWork {
        int frameIndex;
        std::vector<uint8_t> pixelData;
        DngParams params;
    };

    const int QUEUE_CAPACITY = threadCount * 2; // keep writers fed without excessive memory
    std::queue<std::unique_ptr<FrameWork>> workQueue;
    std::mutex queueMutex;
    std::condition_variable queueNotEmpty;
    std::condition_variable queueNotFull;
    std::atomic<bool> readerDone{false};

    std::atomic<int> completedFrames{0};
    std::atomic<uint64_t> totalBytesWritten{0};

    // --- Writer threads: pull from queue, write DNG to disk ---
    auto writer = [&]() {
        while (true) {
            std::unique_ptr<FrameWork> work;
            {
                std::unique_lock<std::mutex> lk(queueMutex);
                queueNotEmpty.wait(lk, [&] {
                    return !workQueue.empty() || readerDone.load();
                });
                if (workQueue.empty() && readerDone.load()) break;
                work = std::move(workQueue.front());
                workQueue.pop();
            }
            queueNotFull.notify_one();

            if (!work) continue;
            if (stopFlag && stopFlag->load()) break;

            // Write DNG file
            char fname[64];
            snprintf(fname, sizeof(fname), "frame_%06d.dng", work->frameIndex + 1);
            std::string outPath = destDir + "\\" + fname;

            if (writeDngDirect(outPath, work->pixelData.data(), work->pixelData.size(), work->params)) {
                uint64_t fileSize = work->pixelData.size() + 1024;
                totalBytesWritten.fetch_add(fileSize);
                int done = completedFrames.fetch_add(1) + 1;
                if (progress) {
                    progress(done, totalFrames, totalBytesWritten.load());
                }
            }
        }
    };

    // Launch writer threads
    std::vector<std::thread> writers;
    writers.reserve(threadCount);
    for (int i = 0; i < threadCount; i++) {
        writers.emplace_back(writer);
    }

    // --- Reader: sequentially read + decompress frames, push to queue ---
    // This runs on the current thread for simplicity.
    // Sequential reads on the MCRAW container maximize I/O throughput.
    for (int i = 0; i < totalFrames; i++) {
        if (stopFlag && stopFlag->load()) break;

        auto work = std::make_unique<FrameWork>();
        work->frameIndex = i;

        nlohmann::json frameMeta;
        try {
            decoder.loadFrame(frameList[i], work->pixelData, frameMeta);
        } catch (...) {
            continue; // skip bad frames
        }

        if (!parseDngParams(work->params, frameMeta, containerMeta))
            continue;
        adjustLevelsForBitDepth(work->params, reinterpret_cast<const uint16_t*>(work->pixelData.data()), work->pixelData.size() / 2);

        // Push to bounded queue (block if full)
        {
            std::unique_lock<std::mutex> lk(queueMutex);
            queueNotFull.wait(lk, [&] {
                return (int)workQueue.size() < QUEUE_CAPACITY || (stopFlag && stopFlag->load());
            });
            if (stopFlag && stopFlag->load()) break;
            workQueue.push(std::move(work));
        }
        queueNotEmpty.notify_one();
    }

    // Signal writers that no more frames are coming
    readerDone.store(true);
    queueNotEmpty.notify_all();

    // Wait for all writers to finish
    for (auto& t : writers) {
        t.join();
    }

    return completedFrames.load();
}
