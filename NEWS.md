## Release v1.1 - 2015-01-18

Please do not use version 1.0, it's is broken.  The so called stable
version had severe flaw in how status file treated.  Fact that the file
was read at start, and overwrote without re-writing it's contents
immediately lead to situation in which the file was empty at stop.  I
hoped signal action would have ensured this cannot happen, but init
script was killing the daemon with signal KILL so signal handler never
ran.  Oh dear, oh dear, the status end up getting lost - assuming one
would restart the daemon without changing state.

The status file problems are now fixed.  Other than that the daemon now
uses epoll for maximum performance, handling of buffers is made simple,
there is more error checking, and few code clean ups.  With a bit of luck
there will not be need for a new release any time soon, but we will see.

## Release v1.0 - 2015-01-05

It is time to declare the software is mature enough to come out of zero
series.  With that I also promise to try harder to avoid version
compatibility problems, which indicates this update is again breaking
stuff, so please read the four warnings below.

1.  The status file is moved (again), and this time from /var/spool to
    /var/lib to be compatible with Linux FHS.
2.  Remote status check response tells the address who responded.  This
    will break check scripts that use --address option.
3.  PATH for pre and post scripts has various bin directories in
    different order.  See manual page for details.
4.  The IPC message got internal format update, that now includes
    information who sent state change message (pid & tty).  This makes
    old f5gs client incompatible with new server software.  (But why
    anyone would have more than one of these binaries?)

Features and fixes.

The release includes new --force option, that is similar with no-scripts
--but will run scripts.  At stop systemd is notified
the service is going down.

The rest of the changes are none functional improvements, such as more
error checking, more regression tests, clarification to code, cleaner
signal handling, and so on.  See git log for details.

## Release v0.5 - 2014-07-12

The systemd support is now enabled by default when build has needed
libraries.  Earlier --quiet option implied --no-scripts option, which was
a bug and now fixed.  The syslog and journald messages are made to have
similar content, so that one can compare functionality in between system
easier.  When post-scripts fail such is no longer considered fatal.  Fix
to crash caused by missing state directory.  Thread safety improvements. 
Add ChangeLog to distributions, with content of git shortlog.

Notable interface changes include deprecation of receiving signals to
change status.  Now daemon will exit if it receives same signals earlier
changed the state.  The state changes can be done by sending IPC message,
that is easiest with the command itself.  Rationality of this interface
change is that after using this software for real it has become clear
other software will never send signals to this daemon.  Automatic state
changes are likely to use the command line interface, which means rather
messy signal catching can be replaced with a lot less problematic IPC
messaging.

Another interfece change is addition of --reason "message here" and --why
options, that persist over daemon restarts and server boot.  The --why
printout will include time stamp of when the state change happen, and how
long it has been since that time.  While the message is likely to be
omitted by most of the users the automatic time stamp is something I
think has value.  Again the actual use of the software has made it clear
that sometimes one wants to know what happen and when.  So who ever adds
state changes to scripts should always add --reason.  Otherwise debugging
is more annoying.

## Release v0.4 - 2014-02-07

Release v0.4.  When ever a developer tells program is ready to be used is
is reasonable to doubt statement being somehow wrong by mistake, or due
overconfidence.  The latest version changes persistent state file path,
and format.

Format got go be changed because use of state numbers changed, and it
became obvious the state file has to be versioned in order to recover
from similar mess in future.  This time the version checking is not done
quite by the book, because version file did not persist over reboot. 
Earlier use of /var/run was a mistake, now the state is wrote to
/var/spool instead.

If you upgrade be sure to set preferred state after an upgrade action. 
Lets hope there are no more breaking changes in future, that require user
interaction.

## Release v0.3 - 2014-01-18

This release adds support to systemd and makes messaging to use journald
when they are available.  Various threading and signaling issues are
fixed.  The contrib directory has an example to puppet management of the
f5gs, and a script demonstrating how to perform pre-state change actions. 
Some of the changes make the command performance to be better, while
other ensure the daemon is very difficult to knock out with (d)dos.  The
making of abuse hard meant fixing all resource leaks.

IMHO the software has production quality.

## Release v0.2 - 2013-10-22

This makes the utility to behave like a reasonable daemon.  It also
includes manual page, autotools setup with gnulib to make the thing
portable.

There is also support for pre and post state change scripts, that one can
use to do additional tasks at a time of an event.  The reason why a
script support was added is local installation I am working with.  When a
java service is put to maintenance or disable state, e.g., F5 is told not
to send traffic, broadcast messaging from that java service has to be
halted as well.  The easiest way to do that is to add a iptables deny
rule for broadcasts, but I cannot possibly add such to the f5gs, so the
support for 'random' actions has to be external.

My intention is to control contents of pre annd post scripts with puppet. 
The nice thing about this decision is that the puppet is environment
aware, so I can fairly precisely tell what sort of additional actions are
needed on a server I plan to control.

## Release v0.1 - 2013-10-09

The beginning.
