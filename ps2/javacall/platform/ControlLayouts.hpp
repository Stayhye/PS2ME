// PS2 JavaCall port — platform layer.
//
// ControlLayouts: the per-game controller mapping the launcher applies before running a
// game. Each game selects one of three values — Global (follow the Settings default),
// Simple (classic arrows + fire) or Complete (full phone keypad + both sticks). The
// choice is persisted by JAR name (like Resolutions/Favorites) in <bootdir>
// controllayouts.txt; only games that deviate from the Global default are stored. See
// Resolutions for the same shape. The Global default itself lives in Settings.
#ifndef PS2_JAVACALL_PLATFORM_CONTROLLAYOUTS_HPP
#define PS2_JAVACALL_PLATFORM_CONTROLLAYOUTS_HPP

namespace ps2 {
namespace platform {

class ControlLayouts {
public:
    // Selectable values, in cycle order. GLOBAL defers to Settings::controlLayout().
    enum { GLOBAL = 0, SIMPLE = 1, COMPLETE = 2 };

    static ControlLayouts& instance();

    /// Read the overrides file and resolve each stored JAR name to its game index in the
    /// current list (@p gameCount entries via hal::GameStorage). Call once at menu start,
    /// after GameStorage::list().
    void load(int gameCount);

    /// Number of selectable presets (3: Global / Simple / Complete).
    static int presetCount();
    /// Label for preset @p i ("Global" / "Simple" / "Complete").
    static const char* preset(int i);

    /// The value currently chosen for @p game (GLOBAL / SIMPLE / COMPLETE).
    int indexFor(int game) const;

    /// Move @p game's value by @p delta (+1 / -1, wrapping) and persist. No-op for an
    /// out-of-range game.
    void cycle(int game, int delta);

private:
    ControlLayouts();
    ControlLayouts(const ControlLayouts&);
    ControlLayouts& operator=(const ControlLayouts&);

    void save() const;   // rewrite the file: one "name=Simple|Complete" line per override

    static const int MAX_GAMES = 2048;   // matches Resolutions / Favorites / IconCache caps

    bool          loaded_;
    int           gameCount_;
    unsigned char idx_[MAX_GAMES];   // GLOBAL / SIMPLE / COMPLETE per game
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_CONTROLLAYOUTS_HPP
