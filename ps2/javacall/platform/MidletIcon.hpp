// PS2 JavaCall port — platform layer.
//
// MidletIcon: extracts and decodes a MIDlet's application icon straight from its JAR
// bytes, with no dependency on the Java VM. A JAR is a ZIP archive; the icon is a PNG
// entry named by the manifest (MIDlet-Icon, or the 2nd field of MIDlet-1). This uses a
// small in-memory ZIP reader (DEFLATE via stb_image's zlib decoder) plus stb_image for
// the PNG, so it runs in the standalone native front-end before any MIDP is up.
#ifndef PS2_JAVACALL_PLATFORM_MIDLETICON_HPP
#define PS2_JAVACALL_PLATFORM_MIDLETICON_HPP

namespace ps2 {
namespace platform {

class MidletIcon {
public:
    /// Random-access reader over an open JAR, so the icon can be pulled without holding
    /// the whole archive in memory. readAt() reads up to len bytes at absolute offset
    /// off into buf and returns the count (< len on short read/EOF, -1 on error).
    struct IJarReader {
        virtual ~IJarReader() {}
        virtual int readAt(unsigned int off, void* buf, int len) = 0;
    };

    /// Decode the application icon from @p jarBytes (@p jarLen bytes). On success
    /// returns a freshly allocated RGBA8888 buffer (@p *outW x @p *outH, 4 bytes per
    /// pixel) and sets the dimensions; returns nullptr if the JAR has no usable icon.
    /// Free the result with release().
    static unsigned char* load(const void* jarBytes, int jarLen, int* outW, int* outH);

    /// Same as load(), but pulls only the ZIP tail (EOCD + central directory) and the
    /// manifest + icon entries through @p reader instead of reading the whole @p jarLen
    /// JAR -- a huge I/O saving on a cache miss. Free the result with release().
    static unsigned char* loadStreaming(IJarReader& reader, int jarLen,
                                        int* outW, int* outH);

    /// Decode a raw PNG (e.g. an embedded UI asset, not a JAR) into a fresh RGBA8888
    /// buffer. Returns nullptr on failure. Free with release().
    static unsigned char* decodePng(const void* png, int len, int* outW, int* outH);

    /// Free a buffer returned by load() / decodePng().
    static void release(unsigned char* pixels);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_MIDLETICON_HPP
