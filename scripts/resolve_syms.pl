#!/usr/bin/perl
# usage: perl scripts/resolve_syms.pl ADDR... (hex) ; reads build/nm.txt
my @s; open(F, "build/nm.txt") or die; while (<F>) { my ($a,$t,$n)=split; push @s,[hex($a),$n] if $t =~ /[tTwW]/; } close F;
@s = sort { $a->[0] <=> $b->[0] } @s;
for my $h (@ARGV) { my $v = hex($h); my $best; for (@s) { last if $_->[0] > $v; $best = $_; } printf "%s -> %s+0x%X\n", $h, $best ? $best->[1] : "?", $best ? $v-$best->[0] : 0; }
