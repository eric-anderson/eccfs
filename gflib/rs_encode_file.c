/*
Procedures and Programs for Galois-Field Arithmetic and Reed-Solomon Coding.  
Copyright (C) 2003 James S. Plank

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

---------------------------------------------------------------------------
Please see http://www.cs.utk.edu/~plank/plank/gflib
for instruction on how to use this library.

Jim Plank
plank@cs.utk.edu
http://www.cs.utk.edu/~plank

Associate Professor
Department of Computer Science
University of Tennessee
203 Claxton Complex
1122 Volunteer Blvd.
Knoxville, TN 37996-3450

     865-974-4397
Fax: 865-974-4404

$Revision: 1.2 $
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "gflib.h"
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <openssl/sha.h>

#include "header.h"

FILE *
openFile(char *stem, int i)
{
    char buf_file[PATH_MAX]; 

    FILE *f;
    sprintf(buf_file, "%s-%04d.rs", stem, i);

    f = fopen(buf_file, "w+");
    if (f == NULL) { perror(buf_file); exit(1); }
    int ret = fseek(f, 0, SEEK_SET);
    return f;
}

void 
writeBuffer(FILE *f, int i, struct header *header, 
	    char *buffer, int blocksize)
{
    SHA_CTX ctx;
    int ret;

    printf("Writing buffer for fragment %d ...", i);
    fflush(stdout);

    SHA1_Init(&ctx);
    // SHA1_Update(&ctx, header, offsetof(struct header, sha1_chunk_hash));
    SHA1_Update(&ctx, buffer, blocksize);
    // Not the final chunk hash, see header.h for explanation
    SHA1_Final(header->sha1_chunk_hash, &ctx); 
    
    // eliminate valgrind warning; we will fix this later, but it's
    // better to get a clean run
    memset(header->sha1_crosschunk_hash, '\0', 20);

    ret = fseek(f, 0, SEEK_SET);
    if (ret != 0) { abort(); };
    ret = fwrite(header, 1, sizeof(*header), f);
    if (ret != sizeof(*header)) { perror("header write failed"); exit(1); }
    ret = fwrite(buffer, 1, blocksize, f);
    if (ret != blocksize) { perror("buffer write failed"); exit(1); }
    printf(" Done\n");
}

/* This one is going to be in-core */

int
main(int argc, char **argv)
{
  int i, j, *vdm, *inv, *prod, cache_size;
  int rows, cols, blocksize, orig_size;
  int n, m, sz, *factors, tmp, factor;
  char *stem, *filename; 
  char **buffer;
  struct header *headers;
  struct stat buf;
  FILE **outfiles;
  FILE *f;
  SHA_CTX ctx;

  if (argc != 5) {
    fprintf(stderr, "usage: rs_encode_file filename n m stem\n");
    exit(1);
  }
  
  n = atoi(argv[2]);
  m = atoi(argv[3]);
  stem = argv[4];
  filename = argv[1];

  rows = n+m;
  cols = n;

  if (stat(filename, &buf) != 0) {
    perror(filename);
    exit(1);
  }

  sz = buf.st_size;
  orig_size = buf.st_size;
  if (sz % (n*sizeof(unit)) != 0) {
    sz += (n*sizeof(unit) - (sz % (n*sizeof(unit))));
  }
  blocksize = sz/n;

  if ((sz - orig_size) < 0 || (sz - orig_size) > 255 || n > 255 || m > 255) {
      fprintf(stderr, "Huh\n");
      abort();
  }

  buffer = (char **) malloc(sizeof(char *)*rows);
  if (buffer == NULL) abort();
  headers = (struct header *)malloc(sizeof(struct header)*rows);

  for(i = 0; i < rows; ++i) {
      headers[i].version = 1;
      headers[i].under_size = sz - orig_size;
      setnmchunknum(headers+i, n, m, i);
  }
      
  for (i = 0; i < n+1; i++) { // one extra buffer for all ecc calculations
      buffer[i] = (char *) malloc(blocksize);
      if (buffer[i] == NULL) {
	  perror("Allocating buffer to store the whole file");
	  exit(1);
      }
  }

  f = fopen(filename, "r");
  if (f == NULL) { perror(filename); }
  cache_size = orig_size;

  SHA1_Init(&ctx);
  for (i = 0; i < n; i++) {
      if (cache_size < blocksize) memset(buffer[i], 0, blocksize);
      if (cache_size > 0) {
	  int amt = (cache_size > blocksize) ? blocksize : cache_size;
	  if (fread(buffer[i], 1, amt, f) <= 0) {
	      fprintf(stderr, "Couldn't read the right bytes into the buffer\n");
	      exit(1);
	  }
	  SHA1_Update(&ctx, buffer[i], amt);
      }
      cache_size -= blocksize;
  }
  fclose(f);
  SHA1_Final(headers[0].sha1_file_hash, &ctx);
  for(i = 1; i < rows; ++i) {
      memcpy(headers[i].sha1_file_hash, headers[0].sha1_file_hash, 
	     sizeof(headers[0].sha1_file_hash));
  }

  outfiles = malloc(sizeof(FILE *)*rows);
  for(i=0; i < n; ++i) {
      outfiles[i] = openFile(stem, i);
      writeBuffer(outfiles[i], i, headers+i, buffer[i], blocksize);
  }

  factors = (int *) malloc(sizeof(int)*n);
  if (factors == NULL) { perror("malloc - factors"); exit(1); }

  for (i = 0; i < n; i++) factors[i] = 1;
  
  vdm = gf_make_dispersal_matrix(rows, cols);

  for (i = cols; i < rows; i++) {
      printf("Calculating parity fragment %d ...", i); fflush(stdout);
      memset(buffer[n], 0, blocksize); 
      for (j = 0; j < cols; j++) {
	  tmp = vdm[i*cols+j]; 
	  if (tmp != 0) {
	      factor = gf_single_divide(tmp, factors[j]);
	      factors[j] = tmp;
	      gf_mult_region(buffer[j], blocksize, factor);
	      gf_add_parity(buffer[j], buffer[n], blocksize);
	  }
      }
      printf("done.\n");
      outfiles[i] = openFile(stem, i);
      writeBuffer(outfiles[i], i, headers+i, buffer[n], blocksize);
  }

  printf("Calculating final hashes and updating files...\n");
  // calculate cross-chunk hash...

  SHA1_Init(&ctx);
  for(i=0; i<rows; ++i) {
      SHA1_Update(&ctx, &headers[i], offsetof(struct header, sha1_crosschunk_hash));
      SHA1_Update(&ctx, headers[i].sha1_chunk_hash, 20);
  }
  SHA1_Final(headers[0].sha1_crosschunk_hash, &ctx);
  for(i=1; i<rows; ++i) {
      memcpy(headers[i].sha1_crosschunk_hash, headers[0].sha1_crosschunk_hash, 20);
  }

  // calculate chunk hash, update header, close...
  for(i=0; i<rows; ++i) {
      int ret;

      SHA1_Init(&ctx);
      // SHA1(data) is in sha1_chunk_hash, so include it...
      SHA1_Update(&ctx, &headers[i], sizeof(struct header));
      SHA1_Final(headers[i].sha1_chunk_hash, &ctx);
      
      ret = fseek(outfiles[i], 0, SEEK_SET);
      if (ret != 0) {
	  perror("seek failed"); 
	  exit(1); 
      }	  
      ret = fwrite(&headers[i], 1, sizeof(struct header), outfiles[i]);
      if (ret != sizeof(struct header)) { 
	  perror("header write failed"); 
	  exit(1); 
      }
      ret = fclose(outfiles[i]);
      if (ret != 0) { perror("error closing file??"); exit(1); }
  }

  exit(0);
}
