// PS2 JavaCall port — platform layer.
//
// Recent: the native front-end's most-recently-launched games, persisted across boots
// so the launcher can surface them in a dedicated "recent" row. A game is identified by
// its JAR name (like Favorites), stored one-per-line most-recent-first in
// <bootdir>recent.txt, written through fileXio under the shared SifLock. push() records
// a launch; list() returns the present recent games (most recent first) as game indices.
#ifndef PS2_JAVACALL_PLATFORM_RECENT_HPP
#define PS2_JAVACALL_PLATFORM_RECENT_HPP

namespace ps2 {
namespace platform {

class Recent {
public:
    static Recent& instance();

    /// Read the persisted list and resolve each stored JAR name to its game index in
    /// the current list (@p gameCount via hal::GameStorage). Call once at menu start.
    void load(int gameCount);

    /// Fill @p out (capacity @p cap) with the recent game indices, most-recent first,
    /// skipping any no longer present; returns how many were written.
    int list(int* out, int cap) const;

    /// Record that game @p game was just launched (moves it to the front) and persist.
    void push(int game);

private:
    Recent();
    Recent(const Recent&);
    Recent& operator=(const Recent&);

    void save() const;

    static const int MAX_KEEP = 16;   // how many entries we remember on disk

    int gameCount_;
    int recent_[MAX_KEEP];            // game indices, most-recent first (present ones)
    int count_;
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_RECENT_HPP
