# Perl playlist module used by the integration tests. Loops over the files
# in $ICES_TEST_LIST (separated by ':'), overriding the first one's title.
use strict;
use warnings;
use POSIX ();   # exercises DynaLoader / XS loading

my @files = split /:/, ($ENV{ICES_TEST_LIST} // "");
my $n = -1;

sub ices_init { print "pl: ices_init\n"; return 1; }
sub ices_shutdown { print "pl: ices_shutdown\n"; return 1; }
sub ices_get_next { $n++; return $files[$n % @files]; }
sub ices_get_metadata { return ($n % @files == 0) ? "Perl Override " . POSIX::floor(1.5) : undef; }
sub ices_get_lineno { return $n + 200; }

1;
