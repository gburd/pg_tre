/*
 * tre-config.h - Static TRE public configuration, replacing autoconf output.
 *
 * Included by tre.h via #include "tre-config.h" (found through -I path).
 * Defines feature flags visible to consumers of the TRE public API.
 */

#ifndef TRE_CONFIG_H
#define TRE_CONFIG_H

#define HAVE_SYS_TYPES_H 1
#define HAVE_WCHAR_H 1

/* Enable approximate matching */
#define TRE_APPROX 1

/* Enable wide character support */
#define TRE_WCHAR 1

/* Enable multibyte character set support */
#define TRE_MULTIBYTE 1

/* TRE version */
#define TRE_VERSION "0.9.0"
#define TRE_VERSION_1 0
#define TRE_VERSION_2 9
#define TRE_VERSION_3 0

#endif /* TRE_CONFIG_H */
