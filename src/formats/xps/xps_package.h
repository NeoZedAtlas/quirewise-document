#ifndef QUIREWISE_XPS_PACKAGE_H
#define QUIREWISE_XPS_PACKAGE_H

#include "quirewise/convert.h"
#include "../../core/layout_document.h"

#include <stddef.h>
#include <stdint.h>

typedef struct QwXpsPackage QwXpsPackage;

int qw_xps_package_open(QwXpsPackage **out_package,
                        const void *data,
                        size_t size,
                        QwConvertReport *report);
size_t qw_xps_package_page_count(const QwXpsPackage *package);
int qw_xps_package_read_page(QwXpsPackage *package,
                             size_t page_index,
                             uint8_t **out_data,
                             size_t *out_size,
                             double *out_width,
                             double *out_height,
                             QwConvertReport *report);
/* Read an image/resource referenced by a page, resolving the source path
 * relative to that page part. */
int qw_xps_package_read_page_resource(QwXpsPackage *package,
                                      size_t page_index,
                                      const char *source,
                                      uint8_t **out_data,
                                      size_t *out_size);
void qw_xps_package_close(QwXpsPackage *package);

/* Layout adapter used by the unified PDF route (xps_layout.c). */
int qw_xps_native_open_layout(const void *data, size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report);

#endif
