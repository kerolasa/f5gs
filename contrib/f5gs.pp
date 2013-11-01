# An example puppet manifest file to maintain f5gs service.
#
# The setuid in f5gs binary will allow f5gs-admins to run command with
# an ssh loop, and is especially useful when ssh-key authentication is
# used (and combined with ssh-agent of course).
#
# ssh-gateway> for I in host{1..5}.example.net; do ssh $I f5gs -m; done
#
# If setuid is not desireable you may need sudo with combination of
# requiretty turned off in sudoers configuration.

class f5gs {

  file { '/etc/f5gs':
    ensure => 'directory',
    owner  => 'root',
    group  => 'root',
    mode   => '0555',
  } ->
  file { '/etc/f5gs/pre':
    ensure => 'present',
    owner  => 'root',
    group  => 'root',
    mode   => '0555',
    source => "puppet:///modules/${module_name}/pre-script",
  }

  package { 'f5gs':
    ensure => latest
  } ->
  service { 'f5gs':
    ensure     => 'running',
    enable     => 'true',
    hasstatus  => 'true',
    hasrestart => 'true',
  } ->
  file { '/usr/bin/f5gs':
    owner => 'root',
    group => 'f5gs-admins',
    mode  => '4550',
  }

  # Some want 'package' to notify a post installation task that enables
  # the f5gs.  Runnign
  #
  #   package { 'f5gs':
  #     ensure => 'latest',
  #     notify => Exec['f5gs --enable'],
  #    } ->
  #
  # is not sufficient.  Adding a script that first checks the state is
  # 'unknown' before changing state works better.  That is because
  # puppet should not mean re-enabling, e.g., perform a state change,
  # after update but only after initial installation.  Later state
  # settings are done by humans, and no automation should override will
  # of a person.
  #
  #!/bin/bash
  # This script is ran by puppet after installation of f5gs.
  #
  #set -e
  ## trap ERR is bashism, do not change shebang!
  #trap 'echo "f5gs-post-install-script: exit on error"; exit 1' ERR
  #set -u
  #PATH='/bin:/usr/bin:/sbin:/usr/sbin'
  #
  #CURRENT_STATE=$(f5gs)
  #if "$CURRENT_STATE" = 'current status is: unknown'; then
  #        f5gs --enable
  #else
  #        logger "f5gs-post-install did not change: $CURRENT_STATE"
  #fi
  #
  #exit 0

}
