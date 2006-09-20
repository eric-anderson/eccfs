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
#include "gflib.h"
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <openssl/sha.h>

#include "header.h"

/* This one is going to be in-core */

main(int argc, char **argv)
{
  int i, j, k, *vdm, *inv, *prod, cache_size;
  int rows, cols, blocksize, orig_size;
  int n, m, sz, *factors, tmp, factor, *exists, *map;
  char *stem, *filename; 
  char **buffer, *buf_file, *block;
  struct stat buf;
  Condensed_Matrix *cm;
  int *mat, *id;
  FILE *f;
  struct header header;
  int ret;
  SHA_CTX ctx;
  unsigned char digest[20];

  if (argc != 2) {
    fprintf(stderr, "usage: rs_decode_file stem\n");
    exit(1);
  }
  
  stem = argv[1];

  buf_file = (char *) malloc(sizeof(char)*(strlen(stem)+30));
  if (buf_file == NULL) { perror("malloc - buf_file"); exit(1); }

  for(i=0; i < 255; ++i) {
      sprintf(buf_file, "%s-%04d.rs", stem, i);
      f = fopen(buf_file, "r");
      if (NULL == f) {
	  continue;
      }
      ret = fread(&header, sizeof(struct header), 1, f);
      if (ret != 1) { perror(buf_file); exit(1); }
      ret = fclose(f);
      if (ret != 0) { perror(buf_file); exit(1); }
      // could verify the file now, the paranoid would do that; we'll verify later.
      break;
  }

  if (stat(buf_file, &buf) != 0) {
      perror(buf_file);
      exit(1);
  }

  n = getn(&header);
  m = getm(&header);
  orig_size = (buf.st_size-sizeof(struct header)) * n - header.under_size;
  rows = n + m;
  cols = n;
  vdm = gf_make_dispersal_matrix(rows, cols);

  fprintf(stderr, "Hello orig=%d n=%d m=%d\n", orig_size, n, m);
  sz = orig_size;
  if (sz % (n*sizeof(unsigned char)) != 0) {
    sz += (n*sizeof(unsigned char) - (sz % (n*sizeof(unsigned char))));
  }
  blocksize = sz/n;
  if (blocksize != buf.st_size - sizeof(struct header)) {
      fprintf(stderr, "huh confused blocksize?\n");
      exit(1);
  }
  exists = (int *) malloc(sizeof(int) * rows);
  if (exists == NULL) { perror("malloc - exists"); exit(1); }
  factors = (int *) malloc(sizeof(int) * rows);
  if (factors == NULL) { perror("malloc - factors"); exit(1); }
  map = (int *) malloc(sizeof(int) * rows);
  if (map == NULL) { perror("malloc - map"); exit(1); }

  buffer = (char **) malloc(sizeof(char *)*n);
  for (i = 0; i < n; i++) {
    buffer[i] = (char *) malloc(blocksize);
    if (buffer[i] == NULL) {
      perror("Allocating buffer to store the whole file");
      exit(1);
    }
  }

  j = 0;

  // The excessively paranoid would keep the first header and compare
  // it exactly to the remaining entries, of course that implies we
  // don't trust SHA1.

  for (i = 0; i < rows && j < cols; i++) {
    sprintf(buf_file, "%s-%04d.rs", stem, i);
    if (stat(buf_file, &buf) != 0) {
      fprintf(stderr, "can't find %s\n", buf_file);
      map[i] = -1;
    } else {
      if ((buf.st_size-sizeof(struct header)) != blocksize) {
	fprintf(stderr, "Ignoring file %s, wrong size\n", buf_file);
        map[i] = -1;
      } else {
        map[i] = j++;
        f = fopen(buf_file, "r");
        if (f == NULL) { perror(buf_file); exit(1); }
	ret = fread(&header, sizeof(struct header), 1, f);
	if (ret != 1) { perror(buf_file); exit(1); }
	if (getn(&header) != n || getm(&header) != m || 
	    blocksize * n - header.under_size != orig_size ||
	    getchunknum(&header) != i || 
	    header.version != 1) {
	    fprintf(stderr,"huh header simple check failed %d != %d || %d != %d || %d * %d - %d != %d || %d != %d || %d != 1?\n",
		    getn(&header), n, getm(&header), m, 
		    blocksize, n, header.under_size, orig_size,
		    getchunknum(&header), i, 
		    header.version);
	    exit(1);
	}
        k = fread(buffer[map[i]], 1, blocksize, f);
        if (k != blocksize) {
          fprintf(stderr, "%s -- stat says %d bytes, but only read %d\n", 
             buf_file, buf.st_size, k);
          exit(1);
        }
	SHA1_Init(&ctx);
	if (sizeof(header) != 4+20+20) {
	    fprintf(stderr, "Header size mismatch\n");
	    abort();
	}
	SHA1_Update(&ctx, &header, 4+20);
	SHA1_Update(&ctx, buffer[map[i]], blocksize);
	SHA1_Final(digest, &ctx);
	if (memcmp(digest, header.sha1_chunk_hash, 20) != 0) {
	    fprintf(stderr, "huh?\n");
	    exit(1);
	}
      }
    }
  }

  if (j < cols) {
    fprintf(stderr, "Only %d fragments -- need %d.  Sorry\n", j, cols);
    exit(1);
  }
  
  j = 0;
  for (i = 0; i < cols; i++) if (map[i] == -1) j++;
  fprintf(stderr, "Blocks to decode: %d\n", j);
  if (j == 0) {
    cache_size = orig_size;
    for (i = 0; i < cols; i++) {
      if (cache_size > 0) {
        fwrite(buffer[i], 1, (cache_size > blocksize) ? blocksize : cache_size, stdout);
        cache_size -= blocksize;
      }
    }
    exit(0);
  } 

  block = (char *) malloc(sizeof(char)*blocksize);
  if (block == NULL) { perror("malloc - block"); exit(1); }
  
  for (i = 0; i < rows; i++) exists[i] = (map[i] != -1);
  cm = gf_condense_dispersal_matrix(vdm, exists, rows, cols);
  mat = cm->condensed_matrix;
  id = cm->row_identities;
  /* Fix it so that map[i] for i = 0 to cols-1 is defined correctly.
     map[i] is the index of buffer[] that holds the blocks for row i in 
     the condensed matrix */

  for (i = 0; i < cols; i++) {
    if (map[i] == -1) map[i] = map[id[i]];
  }

  fprintf(stderr, "Inverting condensed dispersal matrix ... "); fflush(stderr);
  inv = gf_invert_matrix(mat, cols);
  if (inv == NULL) {
    fprintf(stderr, "\n\nError -- matrix unvertible\n");
    exit(1);
  }
  fprintf(stderr, "Done\n"); fflush(stderr);
  
  fprintf(stderr, "\nCondensed matrix:\n\n");
  gf_fprint_matrix(stderr, mat, cols, cols);

  fprintf(stderr, "\nInverted matrix:\n\n");
  gf_fprint_matrix(stderr, inv, cols, cols);

  for(i = 0; i < rows; i++) factors[i] = 1;

  SHA1_Init(&ctx);
  cache_size = orig_size;
  for (i = 0; i < cols && cache_size > 0; i++) {
    int size;
    if (id[i] < cols) {
      fprintf(stderr, "Writing block %d from memory ... ", i); fflush(stderr);
      if (factors[i] != 1) {
        tmp = gf_single_divide(1, factors[i]);
        factors[i] = 1;
        gf_mult_region(buffer[map[i]], blocksize, tmp);
      }
      size = (cache_size > blocksize) ? blocksize : cache_size;
      fwrite(buffer[map[i]], 1, size, stdout);
      SHA1_Update(&ctx, buffer[map[i]], size);
      cache_size -= blocksize;
      fprintf(stderr, "Done\n"); fflush(stderr);
    } else {
      fprintf(stderr, "Decoding block %d ... ", i); fflush(stderr);
      memset(block, 0, blocksize);
      for (j = 0; j < cols; j++) {
        tmp = inv[i*cols+j];
        factor = gf_single_divide(tmp, factors[j]);
        factors[j] = tmp;
        gf_mult_region(buffer[map[j]], blocksize, factor);
        gf_add_parity(buffer[map[j]], block, blocksize);
      }
      fprintf(stderr, "writing ... "); fflush(stderr);
      size = (cache_size > blocksize) ? blocksize : cache_size;
      fwrite(block, 1, size, stdout);
      SHA1_Update(&ctx, block, size);
      cache_size -= blocksize;
      fprintf(stderr, "Done\n"); fflush(stderr);
    }
  }
  SHA1_Final(digest, &ctx);
  if (memcmp(digest, header.sha1_file_hash, 20) != 0) {
      fprintf(stderr, "huh?\n");
      exit(1);
  }
  exit(0);
}
