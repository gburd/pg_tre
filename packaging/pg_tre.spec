# RPM spec for pg_tre — PostgreSQL 18+ approximate regex index AM.

%global pgmajor 18
%global pgname postgresql%{pgmajor}

Name:           pg_tre
Version:        1.0.0
Release:        0.rc1%{?dist}
Summary:        PostgreSQL approximate-regex index access method

License:        MIT AND BSD-2-Clause
URL:            https://codeberg.org/gregburd/pg_tre
Source0:        %{name}-%{version}-rc1.tar.gz

BuildRequires:  %{pgname}-devel >= 18
BuildRequires:  autoconf automake libtool gettext m4
BuildRequires:  gcc make
Requires:       %{pgname}-server >= 18

%description
Native PostgreSQL index access method for approximate-regex matching.
Supports edit-distance k queries over text columns via a three-tier
filter funnel backed by the TRE library for recheck.

%prep
%setup -q

%build
export PG_CONFIG=/usr/pgsql-%{pgmajor}/bin/pg_config
# Build vendored TRE (static)
cd vendor/tre && ./utils/autogen.sh && \
  ./configure --enable-static --disable-shared --disable-nls \
              --without-alloca CFLAGS="-fPIC -O2 -g"
%make_build -C vendor/tre
cd ../..
%make_build

%install
export PG_CONFIG=/usr/pgsql-%{pgmajor}/bin/pg_config
%make_install

%files
%license LICENSE NOTICE
%doc README.md doc/pg_tre.md doc/perf.md
/usr/pgsql-%{pgmajor}/lib/pg_tre.so
/usr/pgsql-%{pgmajor}/share/extension/pg_tre.control
/usr/pgsql-%{pgmajor}/share/extension/pg_tre--1.0.0.sql
/usr/pgsql-%{pgmajor}/share/extension/pg_tre--0.1.0--1.0.0.sql

%changelog
* Tue May 12 2026 Greg Burd <greg@burd.me> - 1.0.0-0.rc1
- Initial release candidate.
