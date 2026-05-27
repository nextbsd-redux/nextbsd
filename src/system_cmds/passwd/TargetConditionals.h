/*
 * Stub TargetConditionals.h for the passwd port on freebsd-launchd-mach.
 *
 * Apple's <TargetConditionals.h> ships with the macOS SDK and gates
 * iOS/simulator code paths. We aren't iOS. Defining both to 0 makes
 * the #if guards in passwd.h take the full-featured "not iPhone"
 * branch (NIS + OpenDirectory + PAM backends enabled).
 *
 * Picked up via -I passwd on the cc command line.
 */
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE     0
#endif
#ifndef TARGET_OS_SIMULATOR
#define TARGET_OS_SIMULATOR  0
#endif
