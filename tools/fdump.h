/* "FOIRDMP1" dump, little endian: fdump_hdr, rdram[rdram_size] (host word order), then ncmds x { uint32_t nwords; uint32_t words[nwords]; };
 * the first nprelude commands re-establish render state */
#ifndef FDUMP_H
#define FDUMP_H
#include <stdint.h>

struct fdump_hdr {
    char magic[8];
    uint32_t rdram_size;
    uint32_t vi[7];      /* status, origin, width, h_start, v_start, x_scale, y_scale */
    uint32_t ncmds;
    uint32_t nprelude;
};
#endif
