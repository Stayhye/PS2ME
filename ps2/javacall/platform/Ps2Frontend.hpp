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

    /// Boot splash: decode the embedded SPLASH.png and show it full-screen through the
    /// shared GsDisplay with an ease-in-out fade in and out, then return. Brings up video
    /// as needed and frees the splash's VRAM texture on the way out. Call once from main()
    /// before the first pick(). Touches no VM; a no-op if video can't come up.
    void splash();

    /// Begin/end the debug overlay (developer view, shown only with "Debug mode" on).
    /// logEnable(true) starts the split view directly (game left, full app log right) --
    /// see debugPresent(); it clears the rolling log and paints the initial "loading"
    /// split. logWrite() feeds the teed VM/native stdout into that log. On real hardware
    /// the SIF/EE console is gone after the USB IOP reset, so this GS overlay is the only
    /// way to see the log. It reuses the menu raster/font/GsDisplay that pick() already
    /// brought up, so it is only valid after pick() has run. logEnable(false) tears down
    /// whichever overlay is up (debug or friendly loader); the non-debug game path calls
    /// it on the first drawn frame -- see Ps2Framebuffer::present.
    void logEnable(bool on);
    void logWrite(const char* s, int len);

    /// Friendly loading screen (default, non-debug launch view). Shows the chosen game's
    /// icon + name with a staged progress bar (Preparing -> Copying -> Installing ->
    /// Starting), driven by the same [Launcher] milestones logWrite() sees. Call once,
    /// after pick() and before JavaTask(), instead of logEnable(true). Torn down by
    /// logEnable(false) when the game starts drawing.
    void loadingBegin(int gameIndex);

    /// Debug split view. While the "Debug mode" setting is on, composite the running
    /// game into the LEFT half of the screen (aspect-preserved) and the full running
    /// application log into the RIGHT half (TTY-style, legible), then present that
    /// combined frame. Called by Ps2Framebuffer::present() each game frame with the VM's
    /// RGB565 raster. Returns true when it handled the present (debug on, so the caller
    /// must not present again); false when debug is off (caller presents fullscreen as
    /// usual). While active it also keeps the fullscreen launch trace's log rolling, so
    /// the install trace scrolls straight into the in-game log.
    bool debugPresent(const unsigned short* raster565, int w, int h);

private:
    Ps2Frontend() {}
    Ps2Frontend(const Ps2Frontend&);
    Ps2Frontend& operator=(const Ps2Frontend&);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2FRONTEND_HPP
