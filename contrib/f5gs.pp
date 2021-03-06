# An example puppet manifest file to maintain f5gs service.
#
# The setuid in f5gs binary will allow f5gs-admins to run command with
# an ssh loop, and is especially useful when ssh-key authentication or
# some other form of single signon is in use.
#
# ssh-gateway> for I in host{1..5}.example.net; do ssh $I f5gs -m; done
#
# If setuid is not desirable you may need sudo, with combination of
# requiretty turned off in sudoers configuration file.

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

}
