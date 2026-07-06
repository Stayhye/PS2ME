// PS2 JavaCall port — KNI bridge for com.nokia.mid.sound.Sound.
//
// Wires the Nokia Sound API to the SPU2 mixer. Sound.java declares these native
// methods; the MIDP NativesTableGen scans the romized classes.zip, finds them, and
// emits nativeFunctionTable.cpp entries referencing these C symbols by their
// JNI-mangled name (Java_com_nokia_mid_sound_Sound_<method>) -- the linker resolves
// them from this object, exactly like GameLoaderKni. romize-midlet.sh injects
// Sound.java before the table is regenerated, so the entries appear automatically.
//
// These only marshal + hand off to Ps2Audio, which owns all audio format handling and
// the voice table. No audsrv / SIF is touched here: the mixer thread is the sole audsrv
// caller, so submitting a sound adds no SIF traffic (it just flips a voice live under
// Ps2Audio's own table lock).
#include <kni.h>

#include "../javacall/platform/Ps2Audio.hpp"

extern "C" {

// int nativePlayTone(int freq, int durMs, int vol, int loop) -> voice id, or -1.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_nokia_mid_sound_Sound_nativePlayTone) {
    const int freq  = KNI_GetParameterAsInt(1);
    const int durMs = KNI_GetParameterAsInt(2);
    const int vol   = KNI_GetParameterAsInt(3);
    const int loop  = KNI_GetParameterAsInt(4);
    KNI_ReturnInt(ps2::platform::Ps2Audio::instance().submitTone(freq, durMs, vol, loop));
}

// int nativePlayWav(byte[] data, int off, int len, int vol, int loop) -> voice id, or -1.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_nokia_mid_sound_Sound_nativePlayWav) {
    int off = KNI_GetParameterAsInt(2);
    int len = KNI_GetParameterAsInt(3);
    const int vol  = KNI_GetParameterAsInt(4);
    const int loop = KNI_GetParameterAsInt(5);
    int id = -1;

    // Copy the WAV bytes into a bounded C scratch (Nokia SFX are small). Single OS thread
    // of green threads on the EE main thread -> not reentrant, static scratch is safe.
    static unsigned char scratch[128 * 1024];

    KNI_StartHandles(1);
    KNI_DeclareHandle(data);
    KNI_GetParameterAsObject(1, data);
    const int cap = KNI_GetArrayLength(data);
    if (off < 0) {
        off = 0;
    }
    if (off > cap) {
        off = cap;
    }
    if (len < 0) {
        len = 0;
    }
    if (off + len > cap) {
        len = cap - off;
    }
    if (len > (int)sizeof(scratch)) {
        len = (int)sizeof(scratch);
    }
    if (len > 0) {
        KNI_GetRawArrayRegion(data, off, len, (jbyte*)scratch);
        id = ps2::platform::Ps2Audio::instance().submitWav(scratch, len, vol, loop);
    }
    KNI_EndHandles();
    KNI_ReturnInt(id);
}

// int nativePlayOta(byte[] data, int off, int len, int vol, int loop) -> voice id, or -1.
// data is a Nokia OTA ringing-tone (Smart Messaging) melody.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_nokia_mid_sound_Sound_nativePlayOta) {
    int off = KNI_GetParameterAsInt(2);
    int len = KNI_GetParameterAsInt(3);
    const int vol  = KNI_GetParameterAsInt(4);
    const int loop = KNI_GetParameterAsInt(5);
    int id = -1;

    static unsigned char scratch[128 * 1024];

    KNI_StartHandles(1);
    KNI_DeclareHandle(data);
    KNI_GetParameterAsObject(1, data);
    const int cap = KNI_GetArrayLength(data);
    if (off < 0) {
        off = 0;
    }
    if (off > cap) {
        off = cap;
    }
    if (len < 0) {
        len = 0;
    }
    if (off + len > cap) {
        len = cap - off;
    }
    if (len > (int)sizeof(scratch)) {
        len = (int)sizeof(scratch);
    }
    if (len > 0) {
        KNI_GetRawArrayRegion(data, off, len, (jbyte*)scratch);
        id = ps2::platform::Ps2Audio::instance().submitOta(scratch, len, vol, loop);
    }
    KNI_EndHandles();
    KNI_ReturnInt(id);
}

// void nativeStop(int id)
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_nokia_mid_sound_Sound_nativeStop) {
    ps2::platform::Ps2Audio::instance().stop(KNI_GetParameterAsInt(1));
    KNI_ReturnVoid();
}

// int nativeVoiceCount() -> concurrent voices the mixer supports.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_nokia_mid_sound_Sound_nativeVoiceCount) {
    KNI_ReturnInt(ps2::platform::Ps2Audio::instance().voiceCount());
}

} // extern "C"
