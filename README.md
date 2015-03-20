## F5 Graceful Scaling

This program was originally intended to be an F5 health check for
services that other vice are difficult to make announce they should not
receive traffic.  When load balancing is done to services such as DNS,
LDAP, or SMTP, the case is valid.  That is also where the three states,
'enable', 'maintenance', and 'disable' come from.  The maintenance is
meant to be the state in which a system will serve existing requests
while new requests are sent to other members of load balanced group. 
Most often the f5gs is additional health check aside of the actual
service healt check.

After use of this program it is increasingly clear states can be used as
generic declaration of intended system state.  When an operator switches
state from enabled to maintenance not only incoming traffic should stop
coming, but application monitoring will need pause as well.  The disabled
state can be used to declare incoming decommissioning of the system,
which makes even basic monitoring such as disk space alarming to halt

Same is true for various automation pieces.  When system is not enabled
configuration management tasks (puppet runs) or scheduled jobs (cron
scripts) should not happen.  That is why Testing a f5gs state is made
easy, see contrib/crontab.txt for an example.

Nothing forbids servers to self declare their state using f5gs
instrumentation.  Depending on environment that can be good or bad. 
Where servers declare their state automatically one is trusting automatic
decision making in such cases to be solid.  Author of this program thinks
automatic state changes can be quite challenging to get right.  Primary
reason is that it is hard to predicting what sort of error states should
forbid or cause automatic state changes, and how to avoid flapping.

Conservative approach to take this utility in use is to leave state
changes to be solely human expressions of will.  Extending human activity
by automating partially is good approach.  For example if automatic
system upgrade could set state to maintenance, due a reboot that follows
it, leaving post upgrade success check to be human task along with f5gs
enabling.

See also NEWS file for a blog like information about the command, and
rationality why something was implemented including what was a thought to
be use of it.

Sami Kerola
