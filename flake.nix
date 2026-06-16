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
          inherit nativeBuildInputs buildInputs;

          # Default PG_CONFIG to the flake's PostgreSQL so `make` works
          # out of the box; still overridable on the command line.
          shellHook = ''
            export PG_CONFIG="${postgresql}/bin/pg_config"
            echo "pg_tre dev shell"
            echo "  PostgreSQL: $(${postgresql}/bin/pg_config --version)"
            echo "  PG_CONFIG : $PG_CONFIG"
            echo "  build     : make PG_CONFIG=\$PG_CONFIG"
            echo "  override  : make PG_CONFIG=~/.pgrx/18.3/pgrx-install/bin/pg_config"
          '';
        };

        # Convenience: expose the pinned PostgreSQL for scripting.
        packages.postgresql = postgresql;
      });
}
