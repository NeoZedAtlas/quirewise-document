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

#endif
