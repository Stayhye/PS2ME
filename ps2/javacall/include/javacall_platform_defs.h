/*
 * PS2 JavaCall port — platform-dependent definitions.
 *
 * Implements the javacall "platform defs" contract (see
 * references/phoneme/javacall/implementation/<port>/javacall_platform_defs.h)
 * for the PlayStation 2 Emotion Engine target built with ps2sdk
 * (mips64r5900el-ps2-elf, 32-bit ABI: int/long/pointer = 4 bytes, long long = 8).
 *
 * The sized-integer typedefs and limit macros below are dictated by the JavaCall
 * API; the VM and this port must agree on them exactly.
 */
#ifndef __JAVACALL_PLATFORM_DEFINE_H_
#define __JAVACALL_PLATFORM_DEFINE_H_

/**
 * @file javacall_platform_defs.h
 * @ingroup Common
 * @brief Platform-dependent definitions for javacall on PS2 (Emotion Engine).
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sized integer types.
 *
 * We deliberately use `int` (not `long`) for the 32-bit types. On the ps2sdk EE
 * ABI `long` is 32-bit, but `int` is 32-bit on every ABI, so this is the robust
 * choice and keeps the port ABI-agnostic if the toolchain ever changes.
 */
typedef unsigned short javacall_utf16;  /**< general unicode string type   */

typedef unsigned char  javacall_uint8;  /**< 8-bit unsigned integer        */
typedef signed   char  javacall_int8;   /**< 8-bit signed integer          */

typedef unsigned short javacall_uint16; /**< 16-bit unsigned integer       */
typedef signed   short javacall_int16;  /**< 16-bit signed integer         */

typedef unsigned int   javacall_uint32; /**< 32-bit unsigned integer       */
typedef signed   int   javacall_int32;  /**< 32-bit signed integer         */

typedef unsigned long long javacall_uint64; /**< 64-bit unsigned integer   */
typedef signed   long long javacall_int64;  /**< 64-bit signed integer     */

/** Maximal length of event data. */
#define JAVACALL_MAX_EVENT_SIZE                   512

/** Maximal length of a filename supported. */
#define JAVACALL_MAX_FILE_NAME_LENGTH             256

/** Maximal number of illegal filename chars. */
#define JAVACALL_MAX_ILLEGAL_FILE_NAME_CHARS      256

/** Maximal length of a list of file system roots. */
#define JAVACALL_MAX_ROOTS_LIST_LENGTH            8192

/** Maximal length of a file system root path. */
#define JAVACALL_MAX_ROOT_PATH_LENGTH             256

/** Maximal length of a list of localized names of file system roots. */
#define JAVACALL_MAX_LOCALIZED_ROOTS_LIST_LENGTH  8192

/** Maximal length of a localized name of a special directory. */
#define JAVACALL_MAX_LOCALIZED_DIR_NAME_LENGTH    512

/* PIM record limits (unused until a PIM JSR is ported, kept for API parity). */
#define JAVACALL_PIM_MAX_ARRAY_ELEMENTS           (10)
#define JAVACALL_PIM_MAX_ATTRIBUTES               (15)
#define JAVACALL_PIM_MAX_FIELDS                   (19)

/* Standard MIDP font sizes (points). */
#define JAVACALL_FONT_SIZE_SMALL                  8
#define JAVACALL_FONT_SIZE_MEDIUM                 12
#define JAVACALL_FONT_SIZE_LARGE                  16

#ifdef __cplusplus
}
#endif

#endif /* __JAVACALL_PLATFORM_DEFINE_H_ */
