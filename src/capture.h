/* Frame capture for offline replay / comparison against angrylion (see tools/). */
#ifndef FOIR_CAPTURE_H
#define FOIR_CAPTURE_H

#include "rdp.h"
#include "vi.h"

/* Reads FOIR_DUMP (path prefix), FOIR_DUMP_FRAME (default 200) and FOIR_DUMP_FRAMES (default 2) */
void capture_init(struct rdp *r);
/* call once per UpdateScreen with the current VI registers */
void capture_frame(struct rdp *r, const struct vi_regs *vi);
/* call after the frame was scanned out; writes PPM screenshots at FOIR_SHOT_FRAMES (comma list) to FOIR_SHOT_DIR */
void capture_screenshot(const struct vi_frame *f);
void capture_shutdown(struct rdp *r);

#endif
