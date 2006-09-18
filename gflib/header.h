struct header {
    unsigned char version;
    unsigned char under_size;
    unsigned char n_m_filenum_a; // 5 bits: n, 5 bits: m, 6 bits: filenum
    unsigned char n_m_filenum_b;
    unsigned char sha1_file_hash[20];
    unsigned char sha1_chunk_hash[20];
};

// no worries about bit field ordering if we do this...
inline unsigned getn(struct header *h) {
    return h->n_m_filenum_a & 0x1F;
}

inline unsigned getm(struct header *h) {
    return (h->n_m_filenum_a >> 5) & 0x07 + 
	((h->n_m_filenum_b & 0x03) << 3);
}

inline unsigned getfilenum(struct header *h) {
    return (h->n_m_filenum_b >> 2) & 0x3F;
}

inline void setnmfilenum(struct header *h, unsigned n, 
			 unsigned m, unsigned filenum) {
    if (n > 31 || m > 31 || filenum > n + m) {
	fprintf(stderr,"internal\n");
	abort();
    }
    h->n_m_filenum_a = n | ((m & 0x07) << 5);
    h->n_m_filenum_b = ((m >> 3) & 0x3) | (filenum << 2);
    if (getn(h) != n || getm(h) != m || getfilenum(h) != filenum) {
	fprintf(stderr,"internal\n");
	abort();
    }
}

