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

private:
    Ps2Frontend() {}
    Ps2Frontend(const Ps2Frontend&);
    Ps2Frontend& operator=(const Ps2Frontend&);
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_PS2FRONTEND_HPP
