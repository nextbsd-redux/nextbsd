/* config.h.  Generated from config.h.in by configure.  */
/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Whether loading unversioned modules support is disabled */
/* #undef DISABLE_UNVERSIONED_MODULES */

/* Define to 1 if you have the <crypt.h> header file. */
/* #undef HAVE_CRYPT_H */

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the `fpurge' function. */
#define HAVE_FPURGE 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* OpenPAM library major number.
 *
 * Apple's upstream sets this to 2 (macOS ships libpam.2.dylib).
 * FreeBSD-pam ships pam_NAME.so.6 modules paired with libpam.so.6.
 * openpam_dynamic_load() searches /usr/lib/pam/pam_NAME.so.${LIB_MAJ}
 * first, then falls back to /usr/lib/pam/pam_NAME.so (unversioned).
 * With LIB_MAJ=2 the search misses FreeBSD's .so.6 modules; both
 * candidates ENOENT and login dies with "no pam_NAME.so found".
 *
 * Bumped to 6 so our libpam (also versioned as libpam.so.6 — see
 * Makefile LIBPAM_SONAME) is consistent with the FreeBSD-pam
 * modules it needs to load this iter for ABI verification. */
#define LIB_MAJ 6

/* Turn debugging on by default */
/* #undef OPENPAM_DEBUG */

/* OpenPAM modules directory */
/* OPENPAM_MODULES_DIR: Apple's upstream is "/usr/lib/pam/" (macOS
 * convention — modules in a per-framework subdir alongside libpam).
 * FreeBSD's bsd.lib.mk default puts PAM modules in SHLIBDIR which
 * defaults to LIBDIR = "/usr/lib", so FreeBSD-pam ships
 * /usr/lib/pam_NAME.so.6 — flat, no per-framework subdir. We
 * need to match that to find FreeBSD's modules during this iter's
 * ABI-verification phase. Iter 3 (once we drop FreeBSD-pam and
 * install our own modules) may revisit this if we want to put
 * /usr/lib/pam/ back as the canonical location. */
#define OPENPAM_MODULES_DIR "/usr/lib/"

/* Name of package */
#define PACKAGE "openpam"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "des@des.no"

/* Define to the full name of this package. */
#define PACKAGE_NAME "OpenPAM"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "OpenPAM 20071221"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "openpam"

/* Define to the version of this package. */
#define PACKAGE_VERSION "20071221"

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "20071221"

/* Define to empty if the keyword `volatile' does not work. Warning: valid
   code using `volatile' can become incorrect without. Disable with care. */
/* #undef volatile */
