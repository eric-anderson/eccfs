// Chunk hash verifies everything in this chunk except for itself

// Cross-chunk hash verifies everything in all chunks except for the
// chunk hash and the crosschunk hash

// File hash verifies the underlying data of the file

struct header {
    unsigned char version;
    unsigned char under_size;
    unsigned char n_m_chunknum_a; // 5 bits: n, 5 bits: m, 6 bits: chunknum
    unsigned char n_m_chunknum_b;
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
    return h->n_m_chunknum_a & 0x1F;
}

inline unsigned getm(struct header *h) {
    return (h->n_m_chunknum_a >> 5) & 0x07 + 
	((h->n_m_chunknum_b & 0x03) << 3);
}

inline unsigned getchunknum(struct header *h) {
    return (h->n_m_chunknum_b >> 2) & 0x3F;
}

inline void setnmchunknum(struct header *h, unsigned n, 
			 unsigned m, unsigned chunknum) {
    if (n > 31 || m > 31 || chunknum > n + m) {
	fprintf(stderr,"internal\n");
	abort();
    }
    h->n_m_chunknum_a = n | ((m & 0x07) << 5);
    h->n_m_chunknum_b = ((m >> 3) & 0x3) | (chunknum << 2);
    if (getn(h) != n || getm(h) != m || getchunknum(h) != chunknum) {
	fprintf(stderr,"internal\n");
	abort();
    }
}

