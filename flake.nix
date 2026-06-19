{
  description = "pg_tre - approximate regex matching index for PostgreSQL 18+";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # PostgreSQL the dev shell builds against by default.  The
        # Makefile is PGXS-driven via PG_CONFIG, so this flake's
        # postgresql_18 makes the project buildable without a separate
        # install.  Override at the make level to use a pgrx install:
        #   make PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config
        postgresql = pkgs.postgresql_18;

        # On current nixpkgs, pg_config is a split passthru output
        # (pkgs.postgresql_18.pg_config), NOT $out/bin/pg_config -- the
        # main output ships only the server/client binaries.  Resolve
        # the real pg_config robustly: prefer the split output, fall
        # back to $out/bin for older nixpkgs that still bundle it.
        pgConfigPkg =
          if postgresql ? pg_config then postgresql.pg_config else postgresql;

        # Toolchain for the PGXS shared library plus the vendored TRE
        # submodule's autotools build.  vendor/tre/configure is already
        # generated, so a plain `make` does not need autopoint; the
        # autotools set is here for the rare `configure.ac` regeneration.
        nativeBuildInputs = with pkgs; [
          gcc
          clang
          gnumake
          pkg-config
          autoconf
          automake
          libtool
          gettext       # provides autopoint for TRE autogen
          m4
          perl          # TAP tests + PG regress harness
        ];

        buildInputs = with pkgs; [
          postgresql
          icu           # pgxs links ICU; matches CI's libicu dep
          readline
          zlib
        ];
      in
      {
        devShells.default = pkgs.mkShell {
          nativeBuildInputs = nativeBuildInputs ++ [ pgConfigPkg ];
          inherit buildInputs;

          # Default PG_CONFIG to the flake's PostgreSQL so `make` works
          # out of the box; still overridable on the command line.  Use
          # the resolved pg_config package (split output on current
          # nixpkgs); fall back to a PATH lookup if the layout differs.
          shellHook = ''
            if [ -x "${pgConfigPkg}/bin/pg_config" ]; then
              export PG_CONFIG="${pgConfigPkg}/bin/pg_config"
            else
              export PG_CONFIG="$(command -v pg_config || true)"
            fi
            echo "pg_tre dev shell"
            echo "  PostgreSQL: $("$PG_CONFIG" --version 2>/dev/null)"
            echo "  PG_CONFIG : $PG_CONFIG"
            echo "  build     : make PG_CONFIG=\$PG_CONFIG"
            echo "  override  : make PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config"
          '';
        };

        # Convenience: expose the pinned PostgreSQL for scripting.
        packages.postgresql = postgresql;
      });
}
