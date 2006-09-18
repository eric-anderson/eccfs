#!/usr/bin/perl -w
use strict;
use File::Find;

usage("missing arguments.")
    unless @ARGV >= 4;

my($n,$m,$importdir,@eccdirs) = @ARGV;

usage("n not an integer or <= 0") unless $n =~ /^\d+$/o && $n > 0;
usage("m not an integer or < 0") unless $n =~ /^\d+$/o && $m >= 0;
usage("importdir not a dir") unless -d $importdir;
map { usage("eccdir $_ not a dir") unless -d $_; } @eccdirs;
usage("n+m != #eccdirs") unless ($n+$m) == @eccdirs;

find(\&wanted, $importdir);

sub wanted {
    my $subname = $File::Find::name;
    $subname =~ s!^$importdir/!!o;
    if (-d $File::Find::name) {
	handledir($subname);
    } elsif (-f $File::Find::name) {
	handlefile($subname);
    } else {
	die "$File::Find::name is not a file or a directory, this is unsupported.";
    }
}

sub handledir {
    my($subname) = @_;
    foreach my $eccdir (@eccdirs) {
	next if -d "$eccdir/$subname"; # already ok
	if (-f "$eccdir/$subname") {
	    unless (defined $Global::fixup{$subname}) {
		print "$eccdir/$subname is a file, but $importdir/$subname is a directory.\n";
		print "what do you want to do with $subname: abort, delete, or rename [abort]?";
		my $choice = <STDIN>;
		chomp;
		$choice = 'abort' if $choice eq '';
		if ($choice =~ /^a(bort)?$/io) {
		    exit(1);
		} elsif ($choice =~ /^d(elete)?$/io) {
		    $Global::fixup{$subname} = ['delete'];
		} elsif ($choice =~ /^r(ename)?$/io) {
		    my $n = 0;
		    my $default = "$subname.$n";
		    while(-e $default) {
			++$n;
			$default = "$subname.$n";
		    }
		    print "what should $subname be renamed to [$default]: ";
		    my $rename_to = <STDIN>;
		    chomp;
		    $rename_to = $default if $rename_to eq '';
		    die "$rename_to invalid" if $rename_to =~ m!^/!o;
		    $Global::fixup{$subname} = ['rename',$rename_to];
		} else {
		    die "Unrecognized choice '$choice'";
		}
	    } else {
		print "Already chose to $Global::fixup{$subname}->[0] $subname\n";
	    }
	    my $t = $Global::fixup{$subname};
	    if ($t->[0] eq 'delete') {
		unlink("$eccdir/$subname") or die "Can't remove $eccdir/$subname: $!";
	    } elsif ($t->[0] eq 'rename') {
		rename("$eccdir/$subname","$eccdir/$t->[1]")
		    or die "Can't rename $eccdir/$subname to $eccdir/$t->[1]: $!";
	    }
	}
	mkdir("$eccdir/$subname",0777) or die "Can't mkdir $eccdir/$subname: $!";
    }
}

sub handlefile {
    my($subname) = @_;

    die "blah blah blah";
    foreach my $eccdir (@eccdirs) {
	next if -d "$eccdir/$subname"; # already ok
	if (-f "$eccdir/$subname") {
	    unless (defined $Global::fixup{$subname}) {
		print "$eccdir/$subname is a file, but $importdir/$subname is a directory.\n";
		print "what do you want to do with $subname: abort, delete, or rename [abort]?";
		my $choice = <STDIN>;
		chomp;
		$choice = 'abort' if $choice eq '';
		if ($choice =~ /^a(bort)?$/io) {
		    exit(1);
		} elsif ($choice =~ /^d(elete)?$/io) {
		    $Global::fixup{$subname} = ['delete'];
		} elsif ($choice =~ /^r(ename)?$/io) {
		    my $n = 0;
		    my $default = "$subname.$n";
		    while(-e $default) {
			++$n;
			$default = "$subname.$n";
		    }
		    print "what should $subname be renamed to [$default]: ";
		    my $rename_to = <STDIN>;
		    chomp;
		    $rename_to = $default if $rename_to eq '';
		    die "$rename_to invalid" if $rename_to =~ m!^/!o;
		    $Global::fixup{$subname} = ['rename',$rename_to];
		} else {
		    die "Unrecognized choice '$choice'";
		}
	    } else {
		print "Already chose to $Global::fixup{$subname}->[0] $subname\n";
	    }
	    my $t = $Global::fixup{$subname};
	    if ($t->[0] eq 'delete') {
		unlink("$eccdir/$subname") or die "Can't remove $eccdir/$subname: $!";
	    } elsif ($t->[0] eq 'rename') {
		rename("$eccdir/$subname","$eccdir/$t->[1]")
		    or die "Can't rename $eccdir/$subname to $eccdir/$t->[1]: $!";
	    }
	}
	mkdir("$eccdir/$subname",0777) or die "Can't mkdir $eccdir/$subname: $!";
    }
}

sub usage {
    die "$_[0]\nUsage: $0 <n> <m> <importdir> <eccdirs...>"
}
