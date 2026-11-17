#ifndef QUIREWISE_PDFA_PROFILE_H
#define QUIREWISE_PDFA_PROFILE_H

#include <stddef.h>
#include <stdint.h>

/* Return the built-in sRGB ICC profile used by the PDF/A output intent. */
const uint8_t *qw_pdfa_srgb_profile(size_t *size);

#endif
