/*
 * config.h - Static configuration for TRE, replacing autoconf output.
 *
 * Used when building TRE as part of the pg_tre PostgreSQL extension.
 * Enables approximate matching, wchar, and multibyte support.
 * Disables alloca (safer in PG backend) and debug features.
 */

#ifndef PG_TRE_CONFIG_H
#define PG_TRE_CONFIG_H

/* Field name in regex_t that stores the compiled TNFA pointer */
#define TRE_REGEX_T_FIELD value

/* Standard POSIX headers available on macOS and Linux */
#define STDC_HEADERS 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1

/* Wide character and multibyte support */
#define HAVE_MBRTOWC 1
#define HAVE_MBSTATE_T 1
#define HAVE_WCTYPE 1
#define HAVE_ISWCTYPE 1
#define HAVE_ISWBLANK 1
#define HAVE_ISBLANK 1
#define HAVE_ISASCII 1

/* Disable alloca: safer in long-running PG backend processes */
/* TRE_USE_ALLOCA deliberately left undefined */

/* Disable malloc debugging */
/* MALLOC_DEBUGGING deliberately left undefined */

/* Disable TRE debug output */
#define NDEBUG 1

/* TRE version */
#define TRE_VERSION "0.9.0"
#define TRE_VERSION_1 0
#define TRE_VERSION_2 9
#define TRE_VERSION_3 0

#endif /* PG_TRE_CONFIG_H */
