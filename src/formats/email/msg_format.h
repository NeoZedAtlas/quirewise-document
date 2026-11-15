#ifndef QUIREWISE_MSG_FORMAT_H
#define QUIREWISE_MSG_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "document_backend.h"

/* Outlook MSG: OLE compound file with MAPI string/attachment streams. */
int qw_msg_probe(const uint8_t *data, size_t size);
int qw_msg_to_html(const uint8_t *data, size_t size, uint8_t **out_html,
                   size_t *out_size, QwConvertReport *report);

#endif
