// PS2 JavaCall port — platform layer.
//
// Ps2SaveIcon: gives a memory-card directory the appearance of a real PS2 "save" so the
// console's OSD browser shows it with a 3D icon and a title instead of "Corrupted Data".
//
// A directory only counts as a save to the browser when it holds a valid icon.sys (the
// 964-byte metadata: title, background gradient, lighting and the icon filenames) plus the
// 3D icon file(s) it names. Ps2SaveIcon generates both: a textured flat quad (icon.icn) in
// the documented PS2 icon format, textured with the PS2ME logo, and the matching icon.sys.
// It writes them through Ps2MemCard into the launcher's app directory ("/PS2ME").
//
// This is deliberately its own module: the icon/format knowledge is separate from the raw
// card I/O in Ps2MemCard, and the same generator will give each per-game save its own
// browser entry later.
#ifndef PS2_JAVACALL_PLATFORM_PS2SAVEICON_HPP
#define PS2_JAVACALL_PLATFORM_PS2SAVEICON_HPP

namespace ps2 {
namespace platform {

class Ps2SaveIcon {
public:
    /// Generate icon.sys + icon.icn and write them into the launcher's memory-card app
    /// directory (via Ps2MemCard::configWrite), titling the save @p title. Requires a
    /// present card (Ps2MemCard::available()); a no-op-worth call otherwise. Returns true
    /// when both files were written.
    static bool installAppIcon(const char* title);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2SAVEICON_HPP
