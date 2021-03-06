# Wben building an rpm from random git revision create a tag
# something like:
#
#	git tag -a v1.1.1
#
# followed by
#
#	./bootstrap && ./configure && make dist
#
# You may need to compile upstream versions of autoconf & automake.
# If so the RHEL I used gave build problems, when new versions were
# available in /usr/local/bin/ and in my PATH, which I overcame by
#
# rpmbuild --nodeps -ba contrib/f5gs.spec

Name:		f5gs
Version:	1.1
Release:	1
Summary:	F5 Graceful Scaling helper daemon
Group:		System Environment/Daemons
License:	BSD
URL:		https://github.com/kerolasa/f5gs
Source0:	%{name}-%{version}.tar.xz
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires: texinfo
BuildRequires: xz
BuildRequires: autoconf >= 2.59
BuildRequires: automake
Requires(post): chkconfig
Requires(postun): initscripts
Requires(preun): chkconfig
Requires(preun): initscripts

# Define init script directory.  %{_initddir} is available from RHEL 6
# forward; RHEL 5 knows only %{_initrddir}.
%{!?_initddir: %{expand: %%global _initddir %{_initrddir}}}

%description
This is a simple tcp daemon, which is intended to be used as a messenger
from a server to a F5 load balancing switch.  One is expected to make a
F5 switch to poll with a health check the f5gs daemon, that will tell the
status of the service.  Other external facilities, such as monitoring and
automation, can be made to consume f5gs status making them to honor
maintenance status if needed.

%prep
%setup -q

%build
%configure
make %{?_smp_mflags}

%install
[ "%{buildroot}" != / ] && %{__rm} -rf "%{buildroot}"
%{__make} install DESTDIR=%{buildroot}
%{__install} -p -D -m 755 contrib/init.redhat %{buildroot}%{_initddir}/%{name}

%pre
if [ "$1" = "2" ]; then
	# Upgrade.
	if [ "$(f5gs --version | awk '{print $3}')" = "1.0" ]; then
		# From 1.0.  This version made status file blank at
		# startup.  Commit that fixed the issue is:
		# f996b927d01765765678ff9d4f934184900f3c80
		find /var/lib/f5gs -size 0 -delete
		pkill -TERM f5gs
	fi
fi

%post
if [ $1 -eq 1 ] ; then
	# Install
	/sbin/chkconfig --add %{name} || :
	/sbin/service %{name} start >/dev/null 2>&1 || :
	# In organizations where there is working orchestration the
	# status change should probably be removed from this rpm spec.
	if [ "$(%{name})" = 'current status is: unknown' ]; then
		%{name} --enable --reason 'rpm post installation' >/dev/null 2>&1 || :
	fi
else
	# Upgrade
	if [ ! -d %{_localstatedir}/lib/%{name} ]; then
		if [ -d %{_localstatedir}/spool/%{name} ]; then
			mv %{_localstatedir}/spool/%{name} %{_localstatedir}/lib
		fi
	fi
	/sbin/service %{name} restart >/dev/null 2>&1 || :
fi

%preun
if [ $1 -eq 0 ] ; then
	# Uninstall
	/sbin/service %{name} stop >/dev/null 2>&1 || :
	/sbin/chkconfig --del %{name} || :
	# Some want to remove state files automatically.
	# %{__rm} -rf %{_localstatedir}/lib/%{name}
fi

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%doc COPYING
%doc %_mandir/man*/*
%_bindir/*
%{_initddir}/%{name}
%_datadir/%{name}/*

%changelog
* Sun Dec 28 2014  Sami Kerola <kerolasa@iki.fi>
- Use /var/lib/ rather than /var/spool/ directory This is the primary
  place to put persistent data.

* Sun Jul 13 2014  Sami Kerola <kerolasa@iki.fi>
- add --reason to post script state change

* Fri Apr 25 2014  Sami Kerola <kerolasa@iki.fi>
- add ChangeLog to doc directory.

* Fri Feb 07 2014  Sami Kerola <kerolasa@iki.fi>
- Use /var/spool/ rather than /var/run/ that is deleted at reboot.

* Thu Dec 12 2013  Sami Kerola <kerolasa@iki.fi>
- Fix installation and upgrade time actions.

* Tue Oct 22 2013  Sami Kerola <kerolasa@iki.fi>
- Add pre-script example to rpm.

* Tue Oct 15 2013  Sami Kerola <kerolasa@iki.fi>
- Add init script.

* Mon Oct 14 2013  Sami Kerola <kerolasa@iki.fi>
- Add a rpm spec file.
