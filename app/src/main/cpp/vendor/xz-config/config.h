/* config.h for Android NDK cross-compilation of liblzma (decoder-only) */

/* Standard headers available in Android NDK (Bionic) */
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_DLFCN_H 1
#define HAVE_WCHAR_H 1
#define STDC_HEADERS 1

/* Types */
#define HAVE__BOOL 1
#define HAVE_UINTPTR_T 1
#define SIZEOF_SIZE_T 8

/* Functions */
#define HAVE_CLOCK_GETTIME 1
#define HAVE_CLOCK_MONOTONIC 1
#define HAVE_FUTIMENS 1
#define HAVE_POSIX_FADVISE 1
#define HAVE_WCWIDTH 1
#define HAVE_MBRTOWC 1
#define HAVE_FUNC_ATTRIBUTE_CONSTRUCTOR 1

/* GCC/Clang builtins */
#define HAVE___BUILTIN_BSWAPXX 1
#define HAVE___BUILTIN_ASSUME_ALIGNED 1
#define HAVE_VISIBILITY 1

/* aarch64 supports unaligned access */
#define TUKLIB_FAST_UNALIGNED_ACCESS 1

/* System detection */
#define TUKLIB_PHYSMEM_SYSINFO 1
#define TUKLIB_CPUCORES_SYSCONF 1

/* Threading - use pthreads */
#define MYTHREAD_POSIX 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1

/* Integrity checks */
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1

/* Decoders enabled (we only need decoders for unsquashfs) */
#define HAVE_DECODERS 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_DECODER_DELTA 1
#define HAVE_DECODER_X86 1
#define HAVE_DECODER_ARM 1
#define HAVE_DECODER_ARM64 1
#define HAVE_DECODER_ARMTHUMB 1
#define HAVE_DECODER_POWERPC 1
#define HAVE_DECODER_SPARC 1
#define HAVE_DECODER_IA64 1

/* Encoders enabled (needed for some shared code paths) */
#define HAVE_ENCODERS 1
#define HAVE_ENCODER_LZMA1 1
#define HAVE_ENCODER_LZMA2 1
#define HAVE_ENCODER_DELTA 1
#define HAVE_ENCODER_X86 1
#define HAVE_ENCODER_ARM 1
#define HAVE_ENCODER_ARM64 1
#define HAVE_ENCODER_ARMTHUMB 1
#define HAVE_ENCODER_POWERPC 1
#define HAVE_ENCODER_SPARC 1
#define HAVE_ENCODER_IA64 1

/* Match finders */
#define HAVE_MF_BT2 1
#define HAVE_MF_BT3 1
#define HAVE_MF_BT4 1
#define HAVE_MF_HC3 1
#define HAVE_MF_HC4 1

/* .lz (lzip) decoder support */
#define HAVE_LZIP_DECODER 1

/* Disable debugging */
#define NDEBUG 1

/* Enable GNU extensions */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Package info */
#define PACKAGE "xz"
#define PACKAGE_NAME "XZ Utils"
#define PACKAGE_VERSION "5.4.5"
#define PACKAGE_STRING "XZ Utils 5.4.5"
#define PACKAGE_BUGREPORT "xz@tukaani.org"
#define PACKAGE_URL "https://tukaani.org/xz/"
#define VERSION "5.4.5"

/* Assume 128 MiB RAM if detection fails */
#define ASSUME_RAM 128
