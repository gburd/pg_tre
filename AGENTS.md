# pg_tre

PostgreSQL extension for approximate regex matching using the TRE library.

## Build

```bash
make PG_CONFIG=/opt/homebrew/opt/postgresql@16/bin/pg_config
make install PG_CONFIG=/opt/homebrew/opt/postgresql@16/bin/pg_config
make installcheck PG_CONFIG=/opt/homebrew/opt/postgresql@16/bin/pg_config
```

Requires the TRE source tree adjacent at `../tre/`.

## Architecture

- `src/pg_tre.c` — PG module magic, GUC, SQL-callable functions (includes postgres.h)
- `src/tre_funcs.c` — TRE wrapper (includes tre.h, NOT postgres.h)
- `src/tre_funcs.h` — Bridge interface using opaque `void *` for compiled regex
- `src/tre_cache.c` — Per-session LRU cache of compiled patterns (32 slots)
- `tre_config/` — Static config.h and tre-config.h replacing TRE's autoconf output

TRE source files are compiled directly from `../tre/lib/` via the Makefile.
