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

    /// Raw on-screen launch trace (debug mode). On real hardware the SIF/EE console is
    /// gone after the USB IOP reset, so there is no way to see the VM's stdout while it
    /// installs and starts a game. When enabled, logWrite() mirrors those bytes
    /// (System.out incl. the [Launcher] milestones and any exception traces) onto the
    /// native GS overlay, so a freeze during launch leaves the last milestone visible.
    /// This is the developer view, shown only when the "Debug mode" setting is on; the
    /// default launch view is the friendly loadingBegin() screen. It reuses the menu
    /// raster/font/GsDisplay that pick() already brought up, so it is only valid after
    /// pick() has run. logEnable(false) tears down whichever overlay is up (raw or
    /// friendly); the game's first drawn frame calls it -- see Ps2Framebuffer::present.
    void logEnable(bool on);
    void logWrite(const char* s, int len);

    /// Friendly loading screen (default, non-debug launch view). Shows the chosen game's
    /// icon + name with a staged progress bar (Preparing -> Copying -> Installing ->
    /// Starting), driven by the same [Launcher] milestones logWrite() sees. Call once,
    /// after pick() and before JavaTask(), instead of logEnable(true). Torn down by
    /// logEnable(false) when the game starts drawing.
    void loadingBegin(int gameIndex);

private:
    Ps2Frontend() {}
    Ps2Frontend(const Ps2Frontend&);
    Ps2Frontend& operator=(const Ps2Frontend&);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2FRONTEND_HPP
