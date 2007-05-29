// Chunk hash verifies everything in this chunk except for itself

// Cross-chunk hash verifies everything in all chunks except for the
// chunk hash and the crosschunk hash

// File hash verifies the underlying data of the file

struct header {
    unsigned char version;
    unsigned char under_size;
    // 5 bits: n, 5 bits: m, 6 bits: chunknum
    unsigned char n_m_chunknum_a; // high 5 bits = n, low 3 bits = high bits of m
    unsigned char n_m_chunknum_b; // high 2 bits = low bits of m, low 6 bits = chunknum
    // sha hash of the reconstructed file
    unsigned char sha1_file_hash[20];
    // crosschunk_hash = SHA1(header_incl_file_hash[0], SHA1(chunk_data[0]), ...)
    unsigned char sha1_crosschunk_hash[20]; 
    // chunk_hash calculated as SHA1(header,SHA1(data))
    // This is done because the ecc calculation destroys the underlying data
    // buffers
    unsigned char sha1_chunk_hash[20];
};

// no worries about bit field ordering if we do this...
inline unsigned getn(struct header *h) {
    return (h->n_m_chunknum_a >> 3) & 0x1F;
}

inline unsigned getm(struct header *h) {
    return ((h->n_m_chunknum_a & 0x07) << 2) |
	((h->n_m_chunknum_b >> 6) & 0x03);
}

inline unsigned getchunknum(struct header *h) {
    return h->n_m_chunknum_b & 0x3F;
}

inline void setnmchunknum(struct header *h, unsigned n, 
			 unsigned m, unsigned chunknum) {
    if (n > 31 || m > 31 || chunknum > n + m) {
	fprintf(stderr,"internal\n");
	abort();
    }
    h->n_m_chunknum_a = (n << 3) | (((m >> 2) & 0x07) << 5);
    h->n_m_chunknum_b = ((m & 0x3) << 6) | chunknum;
    if (getn(h) != n || getm(h) != m || getchunknum(h) != chunknum) {
	fprintf(stderr,"internal\n");
	abort();
    }
}

