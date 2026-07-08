// PS2 JavaCall port — platform layer.
//
// Settings: the native launcher's persisted preferences (a tiny key=value text file,
// <bootdir>settings.txt, written through fileXio under the shared SifLock). Currently:
// whether the ALL GAMES tab shows the recent row, and the default sort order applied at
// startup. Each setter persists immediately.
#ifndef PS2_JAVACALL_PLATFORM_SETTINGS_HPP
#define PS2_JAVACALL_PLATFORM_SETTINGS_HPP

namespace ps2 {
namespace platform {

class Settings {
public:
    static Settings& instance();

    /// Read settings.txt (defaults applied when absent). Call once at menu start.
    void load();

    bool showRecent() const  { return showRecent_; }   // default: true
    int  defaultSort() const { return defaultSort_; }  // default: 0 (A-Z); 1 = Z-A

    void setShowRecent(bool on);
    void setDefaultSort(int mode);

private:
    Settings();
    Settings(const Settings&);
    Settings& operator=(const Settings&);

    void save() const;

    bool showRecent_;
    int  defaultSort_;
    bool loaded_;
};

} // namespace platform
} // namespace ps2

#endif // PS2_JAVACALL_PLATFORM_SETTINGS_HPP
