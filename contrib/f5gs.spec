# Wben building an rpm from random git revision create a tag
# something like:
#
#	git tag -a v2.0
#
# followed by
#
#	./bootstrap && ./configure && make dist
#
# You may need to compile upstream versions of autoconf & automake.
# If so the RHEL I used gave build problems when new versions were
# available in /usr/local/bin/ and in my PATH, which I overcame by
#
# rpmbuild --nodeps -ba contrib/f5gs.spec
Name:		f5gs
Version:	0.2
Release:	1
Summary:	F5 Graceful Scaling helper daemon
Group:		System Environment/Daemons
License:	BSD
URL:		https://github.com/kerolasa/f5gs
Source0:	%{name}-%{version}.tar.xz
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires: texinfo
BuildRequires: xz
BuildRequires: autoconf >= 2.69
BuildRequires: automake

%description
This is a simple tcp daemon, which is intented to be used as a messanger
from a server to a F5 load balancing switch.  One is expected to make a
F5 switch to poll with a health check the f5gs daemon, that will tell the
state of the service.

%prep
%setup -q

%build
%configure
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%doc COPYING
%doc %_mandir/man*/*
%_bindir/*

%changelog
* Mon Oct 14 2013  Sami Kerola <kerolasa@iki.fi>
- Add a rpm spec file.
