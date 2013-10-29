#!/usr/bin/perl 

use strict;
use warnings;


my $header = "/usr/include/X11/keysymdef.h";

print <<__EOD;
typedef struct {
	const char* name;
	int keysym;
	int unicode;
} name2keysym_t;

static const name2keysym_t name2keysym[] = {
__EOD

open(TMP, "<$header");
while (defined(my $line = <TMP>)) {
    next if $line !~ m/\#define /;
    my @ea = split(/\s+/, $line, 4);

    next if $ea[1] !~ m/^XK_(\S+)$/;
    my $name = $1;
    
    next if $ea[2] !~ m/^0x([A-Za-z0-9]+)$/;
    my $keysym = hex($ea[2]);

    my $unicode = 0;
    if ($ea[3] && $ea[3] =~ m/\/\* U\+([0-9A-Fa-f]{4,6}) /) {
	$unicode = hex($1);
    }

    if (!$unicode) {
	# Latin-1 characters (1:1 mapping)
	if (($keysym >= 0x0020 && $keysym <= 0x007e) ||
	    ($keysym >= 0x00a0 && $keysym <= 0x00ff)) {
	    $unicode = $keysym;
	} elsif (($keysym & 0xff000000) == 0x01000000) { 
	    # directly encoded 24-bit UCS characters */
	    $unicode = $keysym & 0x00ffffff;
	} elsif ($keysym >= 0x0ff08 && $keysym <= 0x0ff1b) {
	    # tty BS, LF, RETURN, Delete
	    $unicode = $keysym - 0x0ff00;
	} elsif ($keysym >= 0x0ffaa && $keysym <= 0x0ffaf) {
	    # keypad +*-/
	    $unicode = $keysym - 0x0ff80;
	} elsif ($keysym >= 0x0ffb0 && $keysym <= 0x0ffb9) {
	    # keypad 01234...
	    $unicode = $keysym - 0x0ff80;
	}
    }

    printf("    { \"%s\", 0x%03x,  0x%04x },\n", $name, $keysym, $unicode);
}

printf("    { NULL, 0x%03x,  0x%04x },\n", 0, 0);

print <<__EOD;
};
__EOD
