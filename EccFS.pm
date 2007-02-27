package EccFS;
use strict;
use warnings;

require Exporter;
use vars qw/@ISA @EXPORT/;

@ISA = ('Exporter');
@EXPORT = qw/verifyEccSplitup/;

my $rs_decode_file = "$ENV{HOME}/projects/eccfs/gflib/rs_decode_file";
die "$rs_decode_file not executable" unless -x $rs_decode_file;

sub verifyEccSplitup {
    my($decodedir, $dataname, $files, $n, $m, $verify_level) = @_;

    print "verifyEccSplitup($dataname, [ " . join(", ", @$files) . "], $n, $m)\n"
	if $GLOBAL::debug;
    my $size = -s $dataname;

    my $fh = new FileHandle($dataname) 
	or die "Unable to open $dataname for read: $!";
    my $sha1 = Digest::SHA1->new;
    my $bytes_read = 0;
    while (1) {
	my $buffer;
	my $amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	$bytes_read += $amt;
    }
    die "Size mismatch?? $size != $bytes_read" 
	unless $size == $bytes_read;
    $fh->close();

    my $file_digest = $sha1->digest();
    
    my $rounded_size = $n * POSIX::ceil($size/$n);
    my $under_size = $rounded_size - $size;
    die "??" unless $under_size >= 0 && $under_size < 256;

    my $sha1_crosschunk = new Digest::SHA1;
    my $sha1_filehash = new Digest::SHA1;
    my $crosschunk_hash;
    for(my $i=0; $i < @$files; ++$i) {
	my $tmp = verifyFile($i, $under_size, $rounded_size / $n, $n, $m, 
			     $file_digest, $files->[$i], $sha1_crosschunk, $sha1_filehash);
	$crosschunk_hash = $tmp unless defined $crosschunk_hash;
	die "Crosschunk hash mismatch" unless $crosschunk_hash eq $tmp;
    }

    my $file_digest_over_chunks = $sha1_filehash->digest();
    die "Mismatch between digest calculated over file ecc chunks and file digest: " .
	unpack("H*",$file_digest_over_chunks) . " != " . unpack("H*",$file_digest)
	unless $file_digest eq $file_digest_over_chunks;
    my $crosschunk_digest_over_chunks = $sha1_crosschunk->digest();
    die "Mismatch between crosschunk_digest calculated over chunks and stored digest: " .
	unpack("H*",$crosschunk_digest_over_chunks) . " != " . unpack("H*",$crosschunk_hash)
	unless $crosschunk_digest_over_chunks eq $crosschunk_hash;
    
    return if $verify_level == 0;

    for(my $remove_start = 0; $remove_start < $n; ++$remove_start) {
	my @recover_from = @$files;
	for (my $j = $remove_start; $j < $remove_start + $m; ++$j) {
	    $recover_from[$j] = undef;
	}
	print "verifyRecover(skip $remove_start for $m)\n"
	    if $GLOBAL::debug;
	verifyRecover($decodedir, \@recover_from, $size, $file_digest);
    }
}

sub verifyFile {
    my($chunknum, $under_size, $chunk_size, $n, $m, $file_digest, $chunkname,
       $sha1_crosschunk, $sha1_filehash) = @_;

    print "    verifyFile($chunknum, $chunkname)..." if $GLOBAL::debug;
    my $fh = new FileHandle($chunkname) 
	or die "Unable to open $chunkname for read: $!";
    
    my $header;
    my $amt = sysread($fh, $header, 4+3*20);

    die "read bad" unless 4+3*20 == $amt;
    my($version, $f_under_size, $f_infoa, $f_infob) = unpack("CCCC", $header);
    
    die "Bad version $version != 1" 
	unless 1 == $version;
    die "Bad under size $f_under_size != $under_size" 
	unless $f_under_size == $under_size;

    my $f_n = $f_infoa & 0x1F;
    my $f_m = (($f_infoa >> 5) & 0x7) + (($f_infob & 0x3) << 3);
    my $f_chunknum = (($f_infob >> 2) & 0x3F);
    
    die "Bad file_n $f_n != $n" 
	unless $f_n == $n;
    die "Bad file_m $f_m != $m"
	unless $f_m == $m;
    die "Bad file_chunknum $f_chunknum != $chunknum"
	unless $f_chunknum == $chunknum;

    my $f_file_digest = substr($header, 4, 20);
    die "Bad file hash " . unpack("H*",$f_file_digest) . " != " . unpack("H*", $file_digest)
	unless $f_file_digest eq $file_digest;
    my $f_crosschunk_hash = substr($header, 4+20, 20);
    my $f_chunk_digest = substr($header, 4+2*20, 20);

    my $sha1 = new Digest::SHA1;

    my $bytes_read = 0;
    my $filesize = $n * $chunk_size - $under_size;
    my $filedata_remain = $filesize - $chunknum * $chunk_size;
    while (1) {
	my $buffer;
	$amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	if ($filedata_remain > length($buffer)) {
	    $sha1_filehash->add($buffer);
	} elsif ($filedata_remain > 0) {
	    $sha1_filehash->add(substr($buffer, 0, $filedata_remain));
	} else {
	    # ignore, not part of file data...
	}
	$bytes_read += $amt;
	$filedata_remain -= $amt;
    }
    die "Didn't get proper number of bytes from reading chunk; $bytes_read != $chunk_size"
	unless $bytes_read == $chunk_size;

    my $filechunk_digest = $sha1->digest();
    
    $sha1->reset();
    $sha1->add(substr($header, 0, 4+2*20), $filechunk_digest);
    my $chunk_digest = $sha1->digest();
    die "Bad chunk hash " . unpack("H*",$f_chunk_digest) . " != " . unpack("H*",$chunk_digest)
	unless $f_chunk_digest eq $chunk_digest;
    print "ok\n" if $GLOBAL::debug;

    $sha1_crosschunk->add(substr($header, 0, 4+20), $filechunk_digest);

    return $f_crosschunk_hash;
}

sub verifyRecover {
    my($decodedir, $recover_from, $expected_size, $file_digest) = @_;

    for(my $i=0;$i<@$recover_from; ++$i) {
	my $file = $recover_from->[$i];
	next unless defined $file;
	die "$file doesn't exist??" unless -f $file;
	my $target = sprintf("%s/decode-%04d.rs",$decodedir, $i);
	die "$target already exists" if -e $target || -l $target;
	symlink($file, $target) 
	    || die "Unable to symlink $file to $target";
    }
    my $fh = new FileHandle "$rs_decode_file $decodedir/decode 2>/dev/null |"
	or die "Can't run $rs_decode_file $decodedir/decode: $!";
    my $sha1 = Digest::SHA1->new;
    my $bytes_read = 0;
    while (1) {
	my $buffer;
	my $amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	$bytes_read += $amt;
    }
    close($fh)
	or die "close failed: $!";
    die "exit code of '$rs_decode_file $decodedir/decode' not 0" 
	unless $? == 0;
    die "Unexpected size of decode: $bytes_read != $expected_size"
	unless $bytes_read == $expected_size;
    my $digest = $sha1->digest();
    die "Invalid digest " . unpack("H*", $file_digest) . " != " . unpack("H*", $digest)
	unless $file_digest eq $digest;

    for(my $i=0;$i<@$recover_from; ++$i) {
	my $file = $recover_from->[$i];
	next unless defined $file;
	my $target = sprintf("%s/decode-%04d.rs",$decodedir, $i);
	unlink($target)
	    or die "Unable to unlink $target: $!";
    }
}

1;
