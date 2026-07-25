{
  description = "pg_tre - approximate regex matching index for PostgreSQL 18+";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # Vendored build dependencies, pinned to the same commits as the
    # git submodules (.gitmodules).  Fetched here so `nix build` works
    # without `?submodules=1` -- flakes do not fetch submodules by
    # default, which is why building the extension from a plain flake ref
    # failed for downstream deployers.  Update these revs in lockstep
    # with the submodules.
    tre-src = {
      url = "github:laurikari/tre/d0e0c997336b3210f05b3e1daa7bb5cb9900d274";
      flake = false;
    };
    lime-src = {
      url = "git+https://codeberg.org/gregburd/lime.git?rev=3a0829df74c8a163f0e0aa557427c217fa79e580";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, tre-src, lime-src }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # ------------------------------------------------------------------
        # Build the pg_tre extension against a given PostgreSQL package.
        #
        # Produces a PGXS-layout $out, matching pg_fts, so deployers can
        # overlay the three files into an official postgres:NN image:
        #     $out/lib/pg_tre.so
        #     $out/share/postgresql/extension/pg_tre.control
        #     $out/share/postgresql/extension/pg_tre--<ver>.sql (+ upgrades)
        #
        # The .so links only libc + libm (the vendored TRE is static), so it
        # is ABI-portable into the Debian PG image the way pg_fts already is.
        #
        # requires shared_preload_libraries = 'pg_tre' at runtime.
        # ------------------------------------------------------------------
        mkPgTre = postgresql:
          let
            pgConfig =
              if postgresql ? pg_config then postgresql.pg_config else postgresql;
          in
          pkgs.stdenv.mkDerivation {
            pname = "pg_tre";
            version = "3.0.2";

            # The vendored submodules (vendor/tre, vendor/lime) are pinned
            # as flake inputs and copied in during postPatch, so this build
            # does not depend on `?submodules=1`.
            src = self;

            nativeBuildInputs = with pkgs; [
              gcc
              clang
              gnumake
              pkg-config
              # TRE autotools (configure is pre-generated in the submodule, so
              # autoreconf is only needed if configure.ac changed; kept for
              # robustness).
              autoconf
              automake
              libtool
              gettext
              m4
            ];

            buildInputs = with pkgs; [
              postgresql
              icu
              readline
              zlib
            ];

            # The vendored TRE ./configure and the Lime codegen run during the
            # normal `make`; PG_CONFIG selects the target PostgreSQL.
            PG_CONFIG = "${pgConfig}/bin/pg_config";

            # Populate the vendored submodules from the pinned flake inputs
            # and apply the TRE progress-hook patch here (postPatch), because
            # the Makefile's own patch step uses `git -C vendor/tre apply`,
            # which needs a .git dir the Nix source does not carry.
            postPatch = ''
              # Populate the vendored submodule trees from the pinned flake
              # inputs (the Nix source of `self` does not carry submodule
              # contents; the vendor/tre and vendor/lime dirs are empty
              # placeholders).  Copy preserving the execute bits (autogen.sh
              # et al. must stay runnable) but make the trees writable so the
              # TRE autotools + Lime codegen can generate files in place.
              mkdir -p vendor
              find vendor/tre vendor/lime -mindepth 1 -delete 2>/dev/null || true
              rmdir vendor/tre vendor/lime 2>/dev/null || true
              cp -r --no-preserve=ownership ${tre-src} vendor/tre
              cp -r --no-preserve=ownership ${lime-src} vendor/lime
              chmod -R u+w vendor/tre vendor/lime

              # Apply the TRE progress-hook patch (standard unified diff) if it
              # has not already been applied to this checkout.
              if ! patch -p1 -d vendor/tre --dry-run --reverse \
                     < patches/tre-progress-hook.patch >/dev/null 2>&1; then
                patch -p1 -d vendor/tre < patches/tre-progress-hook.patch
              fi
              # Pre-satisfy the Makefile's patch stamp
              # ($(TRE_DIR)/.pg_tre-patched) so its `git apply` guard (which
              # needs a .git dir the Nix source lacks) is skipped.  Touch it
              # last so it is newer than the patch file and the rule is a
              # no-op.
              touch vendor/tre/.pg_tre-patched
            '';

            # Nix's stdenv exports CC as the wrapped compiler; the vendored
            # TRE ./configure honours CC/CFLAGS.  Force the wrapped clang and a
            # PIC/-O2 build so the static libtre.a links into the .so, and do
            # NOT swallow configure output (the upstream Makefile redirects it
            # to /dev/null, which hid the real error for the operator report).
            buildPhase = ''
              runHook preBuild
              make -j"$NIX_BUILD_CORES" \
                PG_CONFIG="$PG_CONFIG" \
                CC="$CC" \
                V=1
              runHook postBuild
            '';

            # Install PGXS-style but redirect the two install dirs into $out so
            # the layout matches pg_fts (lib/ + share/postgresql/extension/)
            # instead of the read-only postgres store path.
            installPhase = ''
              runHook preInstall
              make install \
                PG_CONFIG="$PG_CONFIG" \
                pkglibdir="$out/lib" \
                datadir="$out/share/postgresql"
              runHook postInstall
            '';

            # PGXS `installcheck` needs a running cluster; the flake build is
            # compile+install only.  Correctness is covered by CI's regression
            # + upgrade suites.
            doCheck = false;

            meta = with pkgs.lib; {
              description =
                "PostgreSQL index access method for approximate regex / edit-distance matching (pg_tre)";
              homepage = "https://codeberg.org/gregburd/pg_tre";
              license = licenses.postgresql;
              platforms = platforms.unix;
            };
          };

        # Default dev-shell PostgreSQL (PG18).
        postgresql = pkgs.postgresql_18;
        pgConfigPkg =
          if postgresql ? pg_config then postgresql.pg_config else postgresql;

        nativeBuildInputs = with pkgs; [
          gcc clang gnumake pkg-config
          autoconf automake libtool gettext m4
          perl
        ];
        buildInputs = with pkgs; [ postgresql icu readline zlib ];
      in
      {
        packages = {
          # Built extension, per PostgreSQL major -- mirrors pg_fts.
          pg17 = mkPgTre pkgs.postgresql_17;
          pg18 = mkPgTre pkgs.postgresql_18;
          # `nix build .#extension` and the flake default both target PG18,
          # the project's baseline.
          extension = mkPgTre pkgs.postgresql_18;
          default = mkPgTre pkgs.postgresql_18;
          # Convenience: the pinned PostgreSQL for scripting.
          postgresql = postgresql;
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = nativeBuildInputs ++ [ pgConfigPkg ];
          inherit buildInputs;
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
      });
}
