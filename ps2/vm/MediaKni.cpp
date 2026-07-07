// PS2 JavaCall port — KNI bridge for the MMAPI (javax.microedition.media) audio path.
//
// Wires the media Player implementation (javax.microedition.media.Ps2Player, via the
// package-private Ps2MediaNatives) to the SPU2 mixer. Same shape as NokiaSoundKni: these
// only marshal and hand off to Ps2Audio, which owns all format handling and the voice
// table. No audsrv / SIF is touched here (the mixer thread is the sole audsrv caller).
//
// Ps2MediaNatives lives in the romized `media` subsystem, so the MIDP NativesTableGen
// finds these native declarations in classes.zip and emits nativeFunctionTable.cpp entries
// referencing these C symbols by their JNI-mangled name
// (Java_javax_microedition_media_Ps2MediaNatives_<method>); the linker resolves them here.
#include <kni.h>

#include "../javacall/platform/Ps2Audio.hpp"

namespace {
// Copy a Java byte[] region into a bounded C scratch. Single OS thread of green threads on
// the EE main thread -> not reentrant, so a static scratch is safe (as in NokiaSoundKni).
// Returns the clamped length actually copied.
int copyRegion(jobject data, int off, int len, unsigned char* scratch, int cap) {
    const int arrLen = KNI_GetArrayLength(data);
    if (off < 0) {
        off = 0;
    }
    if (off > arrLen) {
        off = arrLen;
    }
    if (len < 0) {
        len = 0;
    }
    if (off + len > arrLen) {
        len = arrLen - off;
    }
    if (len > cap) {
        len = cap;
    }
    if (len > 0) {
        KNI_GetRawArrayRegion(data, off, len, (jbyte*)scratch);
    }
    return len;
}
} // namespace

extern "C" {

// int nativePlayWav(byte[] data, int off, int len, int vol, int loop) -> voice id, or -1.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativePlayWav) {
    const int off  = KNI_GetParameterAsInt(2);
    const int len  = KNI_GetParameterAsInt(3);
    const int vol  = KNI_GetParameterAsInt(4);
    const int loop = KNI_GetParameterAsInt(5);
    int id = -1;

    static unsigned char scratch[128 * 1024];
    KNI_StartHandles(1);
    KNI_DeclareHandle(data);
    KNI_GetParameterAsObject(1, data);
    const int n = copyRegion(data, off, len, scratch, (int)sizeof(scratch));
    if (n > 0) {
        id = ps2::platform::Ps2Audio::instance().submitWav(scratch, n, vol, loop);
    }
    KNI_EndHandles();
    KNI_ReturnInt(id);
}

// int nativePlayToneSeq(byte[] data, int off, int len, int vol, int loop) -> voice id, or -1.
// data is an MMAPI tone sequence (audio/x-tone-seq, ToneControl format).
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativePlayToneSeq) {
    const int off  = KNI_GetParameterAsInt(2);
    const int len  = KNI_GetParameterAsInt(3);
    const int vol  = KNI_GetParameterAsInt(4);
    const int loop = KNI_GetParameterAsInt(5);
    int id = -1;

    static unsigned char scratch[128 * 1024];
    KNI_StartHandles(1);
    KNI_DeclareHandle(data);
    KNI_GetParameterAsObject(1, data);
    const int n = copyRegion(data, off, len, scratch, (int)sizeof(scratch));
    if (n > 0) {
        id = ps2::platform::Ps2Audio::instance().submitToneSeq(scratch, n, vol, loop);
    }
    KNI_EndHandles();
    KNI_ReturnInt(id);
}

// int nativePlayMidi(byte[] data, int off, int len, int vol, int loop) -> voice id, or -1.
// data is a Standard MIDI File (audio/midi).
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativePlayMidi) {
    const int off  = KNI_GetParameterAsInt(2);
    const int len  = KNI_GetParameterAsInt(3);
    const int vol  = KNI_GetParameterAsInt(4);
    const int loop = KNI_GetParameterAsInt(5);
    int id = -1;

    static unsigned char scratch[128 * 1024];
    KNI_StartHandles(1);
    KNI_DeclareHandle(data);
    KNI_GetParameterAsObject(1, data);
    const int n = copyRegion(data, off, len, scratch, (int)sizeof(scratch));
    if (n > 0) {
        id = ps2::platform::Ps2Audio::instance().submitMidi(scratch, n, vol, loop);
    }
    KNI_EndHandles();
    KNI_ReturnInt(id);
}

// int nativePlayTone(int freq, int durMs, int vol, int loop) -> voice id, or -1.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativePlayTone) {
    const int freq  = KNI_GetParameterAsInt(1);
    const int durMs = KNI_GetParameterAsInt(2);
    const int vol   = KNI_GetParameterAsInt(3);
    const int loop  = KNI_GetParameterAsInt(4);
    KNI_ReturnInt(ps2::platform::Ps2Audio::instance().submitTone(freq, durMs, vol, loop));
}

// void nativeStop(int id)
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativeStop) {
    ps2::platform::Ps2Audio::instance().stop(KNI_GetParameterAsInt(1));
    KNI_ReturnVoid();
}

// void nativeSetVolume(int id, int vol)
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativeSetVolume) {
    ps2::platform::Ps2Audio::instance().setVolume(KNI_GetParameterAsInt(1),
                                                  KNI_GetParameterAsInt(2));
    KNI_ReturnVoid();
}

// int nativeVoiceCount() -> concurrent voices the mixer supports.
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_media_Ps2MediaNatives_nativeVoiceCount) {
    KNI_ReturnInt(ps2::platform::Ps2Audio::instance().voiceCount());
}

} // extern "C"
