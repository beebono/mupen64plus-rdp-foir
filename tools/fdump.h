/* foir command-list dump file format ("FOIRDMP1"), little endian.
 *
 *   struct fdump_hdr
 *   uint8_t  rdram[rdram_size]     RDRAM exactly as seen by the plugin (host word order)
 *   repeat ncmds times: uint32_t nwords; uint32_t words[nwords];
 *
 * The first `nprelude` commands re-establish RDP render state (set_* commands
 * seen earlier in the run); the rest are the commands executed during the frames.
 */
#ifndef FDUMP_H
#define FDUMP_H
#include <stdint.h>

struct fdump_hdr {
    char magic[8];       /* "FOIRDMP1" */
    uint32_t rdram_size;
    uint32_t vi[7];      /* status, origin, width, h_start, v_start, x_scale, y_scale */
    uint32_t ncmds;
    uint32_t nprelude;
};
#endif
