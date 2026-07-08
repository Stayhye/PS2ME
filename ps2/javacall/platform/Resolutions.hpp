// PS2 JavaCall port — platform layer.
//
// Resolutions: the per-game canvas resolution/orientation the launcher applies before
// running a game. J2ME titles were authored for a fixed device screen and most declare
// nothing in their manifest, so a portrait-only canvas clips landscape games. The user
// picks a resolution per game in the launcher; it is persisted by JAR name (like
// Favorites) in <bootdir>resolutions.txt. When a name embeds a size (e.g.
// "...240x320...") and there is no saved override, that size is auto-selected as the
// default. Only the fixed preset table below is selectable.
#ifndef PS2_JAVACALL_PLATFORM_RESOLUTIONS_HPP
#define PS2_JAVACALL_PLATFORM_RESOLUTIONS_HPP

namespace ps2 {
namespace platform {

class Resolutions {
public:
    static Resolutions& instance();

    /// Read the overrides file and resolve each stored JAR name to its game index in the
    /// current list (@p gameCount entries via hal::GameStorage). Also computes each game's
    /// auto-default from its filename. Call once at menu start, after GameStorage::list().
    void load(int gameCount);

    /// Number of selectable presets.
    static int presetCount();
    /// Preset @p i: its pixel size (via @p w/@p h) and "WxH" label (return value).
    static const char* preset(int i, int* w, int* h);

    /// The preset index currently chosen for game @p game (saved override, else the
    /// filename auto-default, else the portrait QVGA default).
    int indexFor(int game) const;

    /// The chosen pixel size for @p game (via @p w/@p h). Convenience over preset().
    void get(int game, int* w, int* h) const;

    /// Move @p game's preset by @p delta (+1 / -1, wrapping) and persist. No-op for an
    /// out-of-range game.
    void cycle(int game, int delta);

private:
    Resolutions();
    Resolutions(const Resolutions&);
    Resolutions& operator=(const Resolutions&);

    void save() const;   // rewrite the file: one "name=WxH" line per non-default game

    static const int MAX_GAMES = 2048;   // matches Favorites / IconCache caps

    bool          loaded_;
    int           gameCount_;
    unsigned char idx_[MAX_GAMES];   // current preset per game
    unsigned char def_[MAX_GAMES];   // filename-derived default per game
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_RESOLUTIONS_HPP
