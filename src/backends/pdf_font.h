#ifndef QUIREWISE_PDF_FONT_H
#define QUIREWISE_PDF_FONT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal font reader used by the PDF writer to embed a caller-supplied font.
 * TrueType (glyf) and CFF/OpenType (OTTO) fonts are supported; the tables
 * needed for CIDFontType2/CIDFontType0 embedding and for measuring text are
 * parsed: head, maxp, hhea, hmtx, cmap (formats 4 and 12), loca/glyf, or the
 * CFF table. CFF2 variable-font outlines are rejected with a reason because
 * PDF 1.x has no way to embed them.
 */

typedef struct QwPdfFont QwPdfFont;

/* Load a font from disk. Returns NULL for unreadable, malformed or
 * unembeddable fonts; `reason` (when non-NULL) receives a static description
 * of the problem. */
QwPdfFont *qw_pdf_font_load_ex(const char *path, const char **reason);

/* Convenience wrapper around qw_pdf_font_load_ex without a reason. */
QwPdfFont *qw_pdf_font_load(const char *path);

void qw_pdf_font_free(QwPdfFont *font);

/* Map a Unicode code point to a glyph id; 0 (.notdef) when unmapped. */
uint16_t qw_pdf_font_gid(const QwPdfFont *font, unsigned int codepoint);

/* Advance width in 1000/em units for a glyph id. */
uint16_t qw_pdf_font_advance(const QwPdfFont *font, uint16_t gid);

/* Raw font program bytes, embedded as the FontFile2 stream. */
const uint8_t *qw_pdf_font_data(const QwPdfFont *font, size_t *size);

/* Return 1 when the font program is CFF/OpenType (OTTO). */
int qw_pdf_font_is_cff(const QwPdfFont *font);

/* Font metrics in 1000/em units. Returns 0 for a NULL font. */
int qw_pdf_font_metrics(const QwPdfFont *font, int *ascent, int *descent,
                        int *cap_height);

typedef struct QwPdfFontSubset {
    uint8_t *data;      /* Rebuilt TrueType program */
    size_t size;
    uint16_t *map;      /* Original glyph id -> subset glyph id (0xFFFF absent) */
    unsigned num_glyphs;
} QwPdfFontSubset;

/* Build a subset containing .notdef, the given glyphs and their composite
 * components. The returned program is a complete sfnt with rebuilt
 * cmap/loca/hmtx/glyf tables. */
int qw_pdf_font_subset(const QwPdfFont *font, const uint16_t *gids,
                       const unsigned int *codepoints, size_t count,
                       QwPdfFontSubset *out);

void qw_pdf_font_subset_free(QwPdfFontSubset *subset);
/* Font bounding box in 1000/em units. Returns 0 for a NULL font. */
int qw_pdf_font_bbox(const QwPdfFont *font, int *x_min, int *y_min,
                     int *x_max, int *y_max);

#endif
