#ifndef QUIREWISE_PDF_CFF_H
#define QUIREWISE_PDF_CFF_H

#include <stddef.h>
#include <stdint.h>

/* Rebuild a CFF/OpenType font with .notdef and the requested glyphs.
 *
 * The new CharStrings INDEX, charset and (for CID-keyed fonts) FDSelect are
 * rebuilt for the kept glyphs, the FDArray and Private DICT offsets are
 * rewritten, and the sfnt is reassembled around the smaller CFF table.
 *
 * map must hold font->num_glyphs entries and receives the old glyph id ->
 * subset glyph id mapping (0xFFFF for glyphs that were dropped). */
int qw_pdf_cff_subset(const uint8_t *font, size_t font_size,
                      const uint16_t *gids, size_t count,
                      uint8_t **out_font, size_t *out_size, uint16_t *map,
                      unsigned *out_glyph_count);

/* Convert a static CFF2 OpenType font into a CID-keyed CFF 1 font.
 *
 * CFF2 cannot be embedded in PDF 1.x. A CFF2 font without a variation store
 * has plain Type2 charstrings, so it can be rewritten as CFF 1: the charstrings
 * gain their endchar, and a one-entry FDArray/FDSelect plus an Identity charset
 * make the result a CID-keyed font the PDF writer already understands.
 *
 * Variable CFF2 fonts (a vstore in the Top DICT), CFF2 fonts with global
 * subrs, and CFF2 fonts that carry their own FDArray are rejected: they need a
 * static instance, which is the caller's job. */
int qw_pdf_cff2_to_cff(const uint8_t *font, size_t font_size,
                       uint8_t **out_font, size_t *out_size);

#endif
