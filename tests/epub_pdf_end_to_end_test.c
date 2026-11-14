#include <quirewise/convert.h>
#include <quirewise/document.h>

#include "../src/backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_EPUB_FIXTURE
#define QUIREWISE_EPUB_FIXTURE "tests/data/reflow.epub"
#endif
#ifndef QUIREWISE_EPUB_CLASSY_FIXTURE
#define QUIREWISE_EPUB_CLASSY_FIXTURE "tests/data/reflow-classy.epub"
#endif
#ifndef QUIREWISE_EPUB_UNSUPPORTED_FIXTURE
#define QUIREWISE_EPUB_UNSUPPORTED_FIXTURE "tests/data/unsupported-layout.epub"
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}


static int contains_text(const uint8_t *haystack, size_t haystack_size,
                         const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t i;
    if (needle_size == 0u || needle_size > haystack_size) return 0;
    for (i = 0u; i + needle_size <= haystack_size; ++i) {
        if (memcmp(haystack + i, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static int extract_first_chapter_text(uint8_t *pdf, size_t pdf_size,
                                      const char *needle)
{
    QwConvertReport report;
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;
    memset(&report, 0, sizeof(report));
    rc = qw_pdf_reader_open(pdf, pdf_size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages < 1u) {
        qw_pdf_reader_close(reader);
        return 0;
    }
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    qw_pdf_reader_close(reader);
    if (rc != 0 || !text) {
        free(text);
        return 0;
    }
    rc = contains_text(text, text_size, needle) ? 1 : 0;
    free(text);
    return rc;
}

/* Real EPUBs carry a class on most nodes and a stylesheet link. Those are
 * styling, not content; the reader must accept them instead of rejecting the
 * whole chapter (the previous behaviour). */
static int check_classy_epub(void)
{
    uint8_t *epub = NULL;
    size_t epub_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    int rc;

    memset(&report, 0, sizeof(report));
    epub = read_file(QUIREWISE_EPUB_CLASSY_FIXTURE, &epub_size);
    if (!epub) {
        fprintf(stderr, "cannot read classy EPUB fixture: %s\n",
                QUIREWISE_EPUB_CLASSY_FIXTURE);
        return 1;
    }
    /* The memory entry point has no extension: it must detect the EPUB
     * container by content and use the reflow route. */
    rc = qw_convert_to_pdf_memory(epub, epub_size, NULL, &pdf, &pdf_size,
                                  &report);
    free(epub);
    if (rc != 0 || !pdf || pdf_size < 5u) {
        fprintf(stderr, "class/style EPUB rejected: rc=%d note=%s\n", rc,
                report.note);
        free(pdf);
        return 2;
    }
    if (memcmp(pdf, "%PDF-", 5u) != 0 ||
        !extract_first_chapter_text(pdf, pdf_size, "Classy paragraph")) {
        fprintf(stderr, "class/style EPUB text missing\n");
        free(pdf);
        return 3;
    }
    free(pdf);
    return 0;
}

/* A book whose only chapter needs structural layout (a table) has no readable
 * section; that is UNSUPPORTED, not a format error. */
static int check_unsupported_epub(void)
{
    uint8_t *epub = NULL;
    size_t epub_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    int rc;

    memset(&report, 0, sizeof(report));
    epub = read_file(QUIREWISE_EPUB_UNSUPPORTED_FIXTURE, &epub_size);
    if (!epub) {
        fprintf(stderr, "cannot read unsupported EPUB fixture: %s\n",
                QUIREWISE_EPUB_UNSUPPORTED_FIXTURE);
        return 1;
    }
    rc = qw_doc_convert_memory(epub, epub_size, "epub", 1, &pdf, &pdf_size,
                               &report);
    free(epub);
    if (rc != QW_STATUS_ERR_UNSUPPORTED || pdf != NULL) {
        fprintf(stderr, "unsupported EPUB mapped to rc=%d (want %d) note=%s\n",
                rc, QW_STATUS_ERR_UNSUPPORTED, report.note);
        free(pdf);
        return 2;
    }
    if (!strstr(report.note, "EPUB parse failed")) {
        fprintf(stderr, "unsupported EPUB note is not specific: %s\n",
                report.note);
        return 3;
    }
    return 0;
}

int main(void)
{
    uint8_t *epub = NULL;
    size_t epub_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc;
    memset(&report, 0, sizeof(report));
    epub = read_file(QUIREWISE_EPUB_FIXTURE, &epub_size);
    if (!epub) {
        fprintf(stderr, "cannot read EPUB fixture: %s\n", QUIREWISE_EPUB_FIXTURE);
        return 1;
    }
    rc = qw_doc_convert_memory(epub, epub_size, "epub", 1, &pdf, &pdf_size,
                                 &report);
    free(epub);
    if (rc != 0 || !pdf || pdf_size < 5u) {
        fprintf(stderr, "EPUB reflow failed: rc=%d size=%zu note=%s\n", rc,
                pdf_size, report.note);
        free(pdf);
        return 2;
    }
    if (memcmp(pdf, "%PDF-", 5u) != 0) {
        fprintf(stderr, "EPUB reflow output is not a PDF\n");
        free(pdf);
        return 3;
    }
    if (!strstr(report.note, "EPUB reflow")) {
        fprintf(stderr, "loss note missing: %s\n", report.note);
        free(pdf);
        return 4;
    }
    rc = qw_pdf_reader_open(pdf, pdf_size, &reader, &pages, &report);
    if (rc != 0 || !reader || pages < 1u) {
        fprintf(stderr, "rendered PDF has no pages: rc=%d pages=%zu\n", rc, pages);
        free(pdf);
        return 5;
    }
    rc = qw_pdf_reader_extract_text(reader, &text, &text_size, &report);
    qw_pdf_reader_close(reader);
    if (rc != 0 || !text) {
        fprintf(stderr, "text extraction failed: rc=%d\n", rc);
        free(pdf);
        return 6;
    }
    if (!contains_text(text, text_size, "Alpha text") ||
        !contains_text(text, text_size, "Beta text")) {
        fprintf(stderr, "chapter text missing from merged PDF\n");
        free(text);
        free(pdf);
        return 7;
    }
    printf("EPUB reflow: pages=%zu pdf=%zu bytes text=%zu bytes, chapters merged\n",
           pages, pdf_size, text_size);
    free(text);
    free(pdf);

    if (check_classy_epub() != 0) return 8;
    if (check_unsupported_epub() != 0) return 9;
    printf("EPUB compatibility: class/style accepted, unsupported chapter "
           "reported as UNSUPPORTED\n");
    return 0;
}
