# Perl playlist module used by the integration tests. Loops over the files
# in $GSTRADIO_TEST_LIST (separated by ':'), overriding the first one's title.
use strict;
use warnings;
use POSIX ();   # exercises DynaLoader / XS loading

my @files = split /:/, ($ENV{GSTRADIO_TEST_LIST} // "");
my $n = -1;

sub gstradio_init { print "pl: gstradio_init\n"; return 1; }
sub gstradio_shutdown { print "pl: gstradio_shutdown\n"; return 1; }
sub gstradio_get_next { $n++; return $files[$n % @files]; }
sub gstradio_get_metadata { return ($n % @files == 0) ? "Perl Override " . POSIX::floor(1.5) : undef; }
sub gstradio_get_lineno { return $n + 200; }

1;
