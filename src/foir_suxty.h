/*
 * mupen64plus-rdp-foir - foir_suxty.h
 * The optional, one-way interface that the suxty in-game menu (mupen64plus-input-suxty) uses to list and change foir's
 * options.  foir never calls back into suxty and behaves identically when nobody asks for the interface.
 *
 * Discovery: the shared object exports one function,
 *
 *     const struct foir_suxty_interface *FoirSuxtyGetInterface(void);
 *
 * which returns a pointer to a static, const table (valid for as long as the plugin is loaded).  A client must check
 * `abi` before using anything else; `size` allows later versions to append fields to the end of the struct.
 *
 * Every option is a plain int: bool 0/1, an int range, or an enum index (the labels are in the descriptor).
 * See README.md, "suxty interface".
 *
 * THREADING.  get() and set() must be called on the emulation thread (the thread that runs ProcessRDPList /
 * UpdateScreen), but not from inside one of those calls.  The suxty menu runs inside the video-extension buffer swap,
 * which foir issues at the very end of UpdateScreen, after the RDP workers have been drained: nothing else touches the
 * live options while the menu is open.  Once the ROM is open a call from another thread is refused (FOIR_SUXTY_E_THREAD).
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FOIR_SUXTY_H
#define FOIR_SUXTY_H

#include <stdint.h>

#define FOIR_SUXTY_ABI 1

enum foir_suxty_type { FOIR_OPT_BOOL = 0, FOIR_OPT_INT = 1, FOIR_OPT_ENUM = 2 };

/* flags */
#define FOIR_OPTF_LIVE 1u    /* a change takes effect at once; without it the option is read when the ROM is opened */

/* set() results (>= 0) and errors (< 0) */
#define FOIR_SUXTY_APPLIED      0   /* in effect now */
#define FOIR_SUXTY_NEXT_LAUNCH  1   /* remembered, but the running game keeps its value; used by the next RomOpen */
#define FOIR_SUXTY_E_INDEX     (-1) /* no such option */
#define FOIR_SUXTY_E_RANGE     (-2)
#define FOIR_SUXTY_E_THREAD    (-3) /* not the emulation thread */
#define FOIR_SUXTY_E_STATE     (-4) /* plugin not started */
#define FOIR_SUXTY_E_CONFIG    (-5) /* save_default: the core's config could not be written */

struct foir_suxty_option {
    const char *key;                  /* stable identifier = the parameter name in [Video-Foir] ("Widescreen") */
    const char *label;                /* short menu label */
    const char *help;                 /* one line */
    int         type;                 /* enum foir_suxty_type */
    unsigned    flags;                /* FOIR_OPTF_* */
    int         min, max, step;       /* INT: range and step; ENUM: 0 .. max; BOOL: 0 .. 1 */
    const char *const *enum_labels;   /* ENUM: max + 1 labels */
    int         def;                  /* the built-in default */
};

struct foir_suxty_interface {
    uint32_t abi;                     /* FOIR_SUXTY_ABI */
    uint32_t size;                    /* sizeof(struct foir_suxty_interface) */
    uint32_t foir_version;            /* FOIR_VERSION */
    uint32_t option_count;
    const struct foir_suxty_option *options;     /* option_count descriptors; the index is the option id */

    /* The value in effect: for a running game its running value; before RomOpen what the game will start with
     * (a set() value if there is one, else [Video-Foir]).  0 = ok. */
    int (*get)(unsigned index, int *value);
    /* Sets a value.  Never writes mupen64plus.cfg.  Before RomOpen it overrides the config for the coming launch (what
     * suxty does at game start for its per-game values).  FOIR_SUXTY_APPLIED / FOIR_SUXTY_NEXT_LAUNCH, or an error. */
    int (*set)(unsigned index, int value);
    /* Writes the value to [Video-Foir] and saves that section, i.e. makes it the user's global default.  Does not
     * touch the running game or the values set() holds. */
    int (*save_default)(unsigned index, int value);
};

#ifdef __cplusplus
extern "C" {
#endif
const struct foir_suxty_interface *FoirSuxtyGetInterface(void);
#ifdef __cplusplus
}
#endif

#endif
