class PgTre < Formula
  desc "PostgreSQL approximate regex index access method"
  homepage "https://codeberg.org/gregburd/pg_tre"
  url "https://codeberg.org/gregburd/pg_tre/archive/v1.0.0.tar.gz"
  sha256 "REPLACE_WITH_ACTUAL_SHA256"
  license "MIT"

  depends_on "postgresql@18"
  depends_on "autoconf"
  depends_on "automake"
  depends_on "libtool"
  depends_on "gettext"

  def install
    # Initialize submodules (TRE and Lime)
    system "git", "submodule", "update", "--init", "--recursive"

    # Build TRE
    cd "vendor/tre" do
      system "./configure", "--prefix=#{buildpath}/vendor/tre/install"
      system "make"
      system "make", "install"
    end

    # Build Lime
    cd "vendor/lime" do
      system "make"
    end

    # Build pg_tre
    pg_config = Formula["postgresql@18"].opt_bin/"pg_config"
    system "make", "PG_CONFIG=#{pg_config}"
    system "make", "PG_CONFIG=#{pg_config}", "install", "DESTDIR=#{prefix}"
  end

  def caveats
    <<~EOS
      pg_tre has been installed.

      Add the following to postgresql.conf:
        shared_preload_libraries = 'pg_tre'

      Then restart PostgreSQL and run in your database:
        CREATE EXTENSION pg_tre;

      Documentation: #{opt_prefix}/share/postgresql/extension/doc/pg_tre.md
    EOS
  end

  test do
    pg_config = Formula["postgresql@18"].opt_bin/"pg_config"
    lib_dir = `#{pg_config} --pkglibdir`.strip
    assert_predicate Pathname.new(lib_dir)/"pg_tre.so", :exist?,
                     "pg_tre.so not found in #{lib_dir}"
  end
end
