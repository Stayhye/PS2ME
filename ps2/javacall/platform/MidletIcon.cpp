// PS2 JavaCall port — platform layer. MidletIcon implementation.
//
// A JAR is a ZIP archive. To pull the icon we: locate the central directory, find the
// manifest and the icon entry by name, inflate them (raw DEFLATE via stb_image's zlib
// decoder), parse the manifest for the icon path, then decode the PNG with stb_image.
#include "MidletIcon.hpp"

#include <stdlib.h>   // malloc / free
#include <string.h>

// stb_image: PNG decode + a raw-DEFLATE decoder we reuse for the ZIP entries. Exactly
// one TU defines the implementation; PNG-only + no stdio keeps it lean. Found via
// -I<repo>/vendors on this file's compile flags.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace ps2 {
namespace platform {

namespace {

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

inline u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
inline u32 rd32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// Case-insensitive compare of a NUL-terminated name against a (ptr,len) ZIP name.
bool nameEqualsCI(const u8* zn, int znLen, const char* name) {
    int i = 0;
    for (; i < znLen && name[i] != '\0'; ++i) {
        char a = (char)zn[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        // Treat forward and back slashes as equal (defensive).
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return false;
    }
    return i == znLen && name[i] == '\0';
}

// Locate the End Of Central Directory record and return the central directory offset
// and entry count. Returns false if not a ZIP.
bool findEocd(const u8* buf, int len, u32* cdOffset, int* count) {
    if (len < 22) {
        return false;
    }
    // Scan backwards for the EOCD signature (0x06054b50), allowing for a comment.
    int maxBack = len - 22;
    int minPos = maxBack - 65536;
    if (minPos < 0) {
        minPos = 0;
    }
    for (int p = maxBack; p >= minPos; --p) {
        if (rd32(buf + p) == 0x06054b50u) {
            *count    = rd16(buf + p + 10);
            *cdOffset = rd32(buf + p + 16);
            return true;
        }
    }
    return false;
}

// Extract the named entry into a freshly malloc'd buffer; returns it and sets *outSize
// (NUL-terminates one byte past the end for text convenience). Returns nullptr if the
// entry is missing or uses an unsupported compression method.
u8* zipExtract(const u8* buf, int len, const char* name, int* outSize) {
    u32 cdOffset = 0;
    int count = 0;
    if (!findEocd(buf, len, &cdOffset, &count)) {
        return 0;
    }

    u32 p = cdOffset;
    for (int i = 0; i < count; ++i) {
        if (p + 46 > (u32)len || rd32(buf + p) != 0x02014b50u) {
            return 0;
        }
        const u16 method     = rd16(buf + p + 10);
        const u32 compSize   = rd32(buf + p + 20);
        const u32 uncompSize = rd32(buf + p + 24);
        const u16 nameLen    = rd16(buf + p + 28);
        const u16 extraLen   = rd16(buf + p + 30);
        const u16 commentLen = rd16(buf + p + 32);
        const u32 localOff   = rd32(buf + p + 42);
        const u8* entryName  = buf + p + 46;

        if (nameEqualsCI(entryName, nameLen, name)) {
            // Local header: real data starts past its own name+extra fields.
            if (localOff + 30 > (u32)len || rd32(buf + localOff) != 0x04034b50u) {
                return 0;
            }
            const u16 lNameLen  = rd16(buf + localOff + 26);
            const u16 lExtraLen = rd16(buf + localOff + 28);
            const u32 dataOff   = localOff + 30 + lNameLen + lExtraLen;
            if (dataOff + compSize > (u32)len) {
                return 0;
            }
            const u8* data = buf + dataOff;

            if (method == 0) {                     // stored
                u8* out = (u8*)malloc(compSize + 1);
                if (out == 0) return 0;
                memcpy(out, data, compSize);
                out[compSize] = 0;
                *outSize = (int)compSize;
                return out;
            }
            if (method == 8) {                     // deflate
                u8* out = (u8*)malloc(uncompSize + 1);
                if (out == 0) return 0;
                const int got = stbi_zlib_decode_noheader_buffer(
                    (char*)out, (int)uncompSize, (const char*)data, (int)compSize);
                if (got < 0) {
                    free(out);
                    return 0;
                }
                out[got] = 0;
                *outSize = got;
                return out;
            }
            return 0;                              // unsupported method
        }

        p += 46u + nameLen + extraLen + commentLen;
    }
    return 0;
}

// Copy the value of manifest header @p key (case-insensitive) into @p out. Returns
// false if not found. Handling of 72-column continuation lines is skipped (icon paths
// are short); CR and surrounding spaces are trimmed.
bool manifestValue(const char* mf, int mfLen, const char* key, char* out, int outCap) {
    const int keyLen = (int)strlen(key);
    int i = 0;
    while (i < mfLen) {
        // Start of a line: try to match "key:".
        int j = 0;
        while (j < keyLen && i + j < mfLen) {
            char a = mf[i + j];
            char b = key[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++j;
        }
        if (j == keyLen && i + j < mfLen && mf[i + j] == ':') {
            int k = i + j + 1;
            while (k < mfLen && (mf[k] == ' ' || mf[k] == '\t')) ++k;  // skip leading space
            int o = 0;
            while (k < mfLen && mf[k] != '\r' && mf[k] != '\n' && o < outCap - 1) {
                out[o++] = mf[k++];
            }
            while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) --o;  // trim trailing
            out[o] = '\0';
            return o > 0;
        }
        // Advance to the next line.
        while (i < mfLen && mf[i] != '\n') ++i;
        ++i;
    }
    return false;
}

// The icon path is MIDlet-Icon, or the 2nd comma-separated field of MIDlet-1
// ("Name, /icon.png, Class"). Writes the entry name (leading '/' stripped) to out.
bool iconPath(const char* mf, int mfLen, char* out, int outCap) {
    char line[256];
    if (manifestValue(mf, mfLen, "MIDlet-Icon", line, (int)sizeof(line))) {
        // whole value is the path
    } else if (manifestValue(mf, mfLen, "MIDlet-1", line, (int)sizeof(line))) {
        // second field
        const char* c1 = strchr(line, ',');
        if (c1 == 0) return false;
        const char* start = c1 + 1;
        while (*start == ' ' || *start == '\t') ++start;
        const char* c2 = strchr(start, ',');
        int n = (c2 != 0) ? (int)(c2 - start) : (int)strlen(start);
        while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) --n;
        if (n <= 0 || n >= (int)sizeof(line)) return false;
        memmove(line, start, n);
        line[n] = '\0';
    } else {
        return false;
    }
    const char* s = line;
    if (*s == '/') ++s;                    // ZIP entries have no leading slash
    if (*s == '\0') return false;
    int o = 0;
    while (s[o] != '\0' && o < outCap - 1) { out[o] = s[o]; ++o; }
    out[o] = '\0';
    return true;
}

} // namespace

unsigned char* MidletIcon::load(const void* jarBytes, int jarLen, int* outW, int* outH) {
    const u8* jar = (const u8*)jarBytes;

    int mfSize = 0;
    u8* mf = zipExtract(jar, jarLen, "META-INF/MANIFEST.MF", &mfSize);
    if (mf == 0) {
        return 0;
    }
    char path[256];
    const bool havePath = iconPath((const char*)mf, mfSize, path, (int)sizeof(path));
    free(mf);
    if (!havePath) {
        return 0;
    }

    int pngSize = 0;
    u8* png = zipExtract(jar, jarLen, path, &pngSize);
    if (png == 0) {
        return 0;
    }

    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(png, pngSize, &w, &h, &comp, 4);
    free(png);
    if (rgba == 0) {
        return 0;
    }
    *outW = w;
    *outH = h;
    return rgba;
}

unsigned char* MidletIcon::decodePng(const void* png, int len, int* outW, int* outH) {
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory((const u8*)png, len, &w, &h, &comp, 4);
    if (rgba == 0) {
        return 0;
    }
    *outW = w;
    *outH = h;
    return rgba;
}

void MidletIcon::release(unsigned char* pixels) {
    stbi_image_free(pixels);
}

} // namespace platform
} // namespace ps2
