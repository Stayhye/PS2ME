// PS2 JavaCall port — platform layer.
//
// Favorites: the native front-end's "favourited games" set, persisted across boots.
// A game is identified by its JAR name (as reported by hal::GameStorage::nameAt), so
// the set survives re-indexing as long as the file is still present. The on-disk form
// is a plain text file (one JAR name per line) beside the games, written through the
// same fileXio path Ps2Storage uses; all I/O holds the shared SifLock.
//
// In memory the set is a bool per game index, resolved against the current game list at
// load() time, so isFavorite()/toggle() are O(1) on the render thread.
#ifndef PS2_JAVACALL_PLATFORM_FAVORITES_HPP
#define PS2_JAVACALL_PLATFORM_FAVORITES_HPP

namespace ps2 {
namespace platform {

class Favorites {
public:
    static Favorites& instance();

    /// Read the persisted list and resolve each stored JAR name to its game index in
    /// the current list (@p gameCount entries via hal::GameStorage). Call once at menu
    /// start, after GameStorage::list(). Safe to call again (re-resolves).
    void load(int gameCount);

    /// Whether game @p game (a GameStorage index) is currently favourited.
    bool isFavorite(int game) const;

    /// Flip the favourite flag of @p game and persist the whole set immediately.
    void toggle(int game);

    /// How many present games are favourited.
    int count() const;

    /// Fill @p out (capacity @p cap) with the favourited game indices in ascending
    /// order; returns how many were written.
    int list(int* out, int cap) const;

private:
    Favorites();
    Favorites(const Favorites&);
    Favorites& operator=(const Favorites&);

    void save() const;   // rewrite the whole file from the in-memory set (holds SifLock)

    static const int MAX_GAMES = 2048;   // matches Ps2HostStorage / IconCache caps

    bool loaded_;
    int  gameCount_;
    bool fav_[MAX_GAMES];
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_FAVORITES_HPP
