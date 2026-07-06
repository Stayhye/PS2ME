// PS2 JavaCall port — platform layer.
//
// Ps2Frontend: the native (C) game launcher menu. It is a STANDALONE front-end with
// no ties to the Java VM -- it runs from main() before the VM is initialized, owns
// its own RGBA5551 raster, presents it directly through the shared GsDisplay (the
// PS2 "video card"), reads the controller through the shared pad backend, and lists
// the games straight from hal::GameStorage (host:games/). It draws with the embedded
// TrueType font (vendors/ui.ttf via stb_truetype), navigates with the D-pad, and
// returns the chosen game index (or -1 to quit). Only after a game is chosen does the
// caller boot the VM to run that one game -- the menu itself never calls into MIDP.
#ifndef PS2_JAVACALL_PLATFORM_PS2FRONTEND_HPP
#define PS2_JAVACALL_PLATFORM_PS2FRONTEND_HPP

namespace ps2 {
namespace platform {

class Ps2Frontend {
public:
    static Ps2Frontend& instance();

    /// Draw the menu and block until the user selects a game (Cross). Returns the
    /// game's storage index, or -1 if there is nothing to run / on failure / quit.
    /// Self-contained: brings up video, input and storage as needed; touches no VM.
    int pick();

    /// On-screen launch trace. On real hardware the SIF/EE console is gone after the
    /// USB IOP reset, so there is no way to see the VM's stdout while it installs and
    /// starts a game. When enabled, logWrite() mirrors those bytes (System.out incl.
    /// the [Launcher] milestones and any exception traces) onto the native GS overlay,
    /// so a freeze during launch leaves the last milestone visible. It reuses the menu
    /// raster/font/GsDisplay that pick() already brought up, so it is only valid after
    /// pick() has run. Disabled again the moment the game starts drawing (so its own
    /// screen is not clobbered) -- see Ps2Framebuffer::present.
    void logEnable(bool on);
    void logWrite(const char* s, int len);

private:
    Ps2Frontend() {}
    Ps2Frontend(const Ps2Frontend&);
    Ps2Frontend& operator=(const Ps2Frontend&);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2FRONTEND_HPP
