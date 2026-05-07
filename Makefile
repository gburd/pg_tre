EXTENSION    = pg_tre
MODULE_big   = pg_tre
DATA         = sql/pg_tre--0.1.0.sql
REGRESS      = pg_tre

OBJS = src/pg_tre.o src/tre_funcs.o src/tre_cache.o

TRE_DIR = ../tre
TRE_OBJS = \
    $(TRE_DIR)/lib/regcomp.o \
    $(TRE_DIR)/lib/regerror.o \
    $(TRE_DIR)/lib/regexec.o \
    $(TRE_DIR)/lib/tre-ast.o \
    $(TRE_DIR)/lib/tre-compile.o \
    $(TRE_DIR)/lib/tre-match-approx.o \
    $(TRE_DIR)/lib/tre-match-backtrack.o \
    $(TRE_DIR)/lib/tre-match-parallel.o \
    $(TRE_DIR)/lib/tre-mem.o \
    $(TRE_DIR)/lib/tre-parse.o \
    $(TRE_DIR)/lib/tre-stack.o \
    $(TRE_DIR)/lib/xmalloc.o

OBJS += $(TRE_OBJS)

# -Itre_config: our static config.h and tre-config.h
# -I../tre/local_includes: TRE public header (tre.h)
# -DHAVE_CONFIG_H: activates TRE's config.h inclusion
# -Wno-declaration-after-statement: TRE source uses pre-C99 style
PG_CPPFLAGS = -I$(srcdir)/tre_config \
              -I$(TRE_DIR)/local_includes \
              -DHAVE_CONFIG_H \
              -Wno-declaration-after-statement

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
