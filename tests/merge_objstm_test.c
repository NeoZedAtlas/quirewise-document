/* Merging a PDF whose page object lives in an object stream: the merge must
   expand the compressed object before copying, and the result must reopen with
   both pages. The fixture (tests/data/merge_objstm.pdf) is a well-formed PDF
   1.5 file with an xref table and a type-2 entry for object 6. */
#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    FILE *file = fopen(QUIREWISE_MERGE_OBJSTM_PDF, "rb");
    long length;
    uint8_t *bytes = NULL;
    size_t byte_count = 0u;
    const uint8_t *pdfs[2];
    size_t sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t pages = 0u;
    QwConvertReport report;
    QwPdfReader *reader = NULL;
    size_t reader_pages = 0u;
    int rc;

    if (!file) {
        fprintf(stderr, "cannot open fixture %s\n", QUIREWISE_MERGE_OBJSTM_PDF);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 1; }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 1; }
    bytes = (uint8_t *)malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    byte_count = fread(bytes, 1u, (size_t)length, file);
    fclose(file);
    if (byte_count != (size_t)length) { free(bytes); return 1; }

    pdfs[0] = bytes;
    pdfs[1] = bytes;
    sizes[0] = byte_count;
    sizes[1] = byte_count;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_merge_documents(pdfs, sizes, 2u, &merged, &merged_size,
                                &pages, &report);
    if (rc != 0 || !merged || pages != 2u) {
        fprintf(stderr,
                "ObjStm merge failed: rc=%d pages=%zu note=%s size=%zu\n", rc,
                pages, report.note, byte_count);
        free(merged);
        free(bytes);
        return 3;
    }
    rc = qw_pdf_reader_open(merged, merged_size, &reader, &reader_pages,
                            &report);
    if (rc != 0 || reader_pages != 2u) {
        fprintf(stderr, "merged ObjStm PDF does not open: rc=%d pages=%zu\n",
                rc, reader_pages);
        qw_pdf_reader_close(reader);
        free(merged);
        free(bytes);
        return 4;
    }
    qw_pdf_reader_close(reader);
    free(merged);
    free(bytes);
    puts("PDF merge expands compressed object streams before copying");
    return 0;
}
