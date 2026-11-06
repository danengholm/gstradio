# Perl playlist module used by the integration tests. Loops over the files
# in $GSRADIO_TEST_LIST (separated by ':'), overriding the first one's title.
use strict;
use warnings;
use POSIX ();   # exercises DynaLoader / XS loading

my @files = split /:/, ($ENV{GSRADIO_TEST_LIST} // "");
my $n = -1;

sub gsradio_init { print "pl: gsradio_init\n"; return 1; }
sub gsradio_shutdown { print "pl: gsradio_shutdown\n"; return 1; }
sub gsradio_get_next { $n++; return $files[$n % @files]; }
sub gsradio_get_metadata { return ($n % @files == 0) ? "Perl Override " . POSIX::floor(1.5) : undef; }
sub gsradio_get_lineno { return $n + 200; }

1;
