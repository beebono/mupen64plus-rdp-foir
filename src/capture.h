#ifndef FOIR_CAPTURE_H
#define FOIR_CAPTURE_H

#include "rdp.h"
#include "vi.h"

void capture_init(struct rdp *r);
void capture_frame(struct rdp *r, const struct vi_regs *vi);
void capture_screenshot(const struct vi_frame *f);
void capture_shutdown(struct rdp *r);

#endif
