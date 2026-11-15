/*
 * Cross-family conversion tests for the public document API.
 *
 * A structured-data file, a subtitle file and a worksheet are converted to a
 * second format in the same family through qw_convert_file_to_format(), and
 * the same JSON and SRT payloads are converted through the memory entry point
 * (which has no path and therefore sniffs the content).
 */

#include <quirewise/convert.h>
#include <quirewise/docx.h>
#include <quirewise/epub.h>
#include <quirewise/htmlz.h>
#include <quirewise/hwpx.h>
#include <quirewise/text_archive.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");

    if (!file) return 0;
    if (fputs(text, file) == EOF) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int file_contains(const char *path, const char *needle)
{
    char buffer[4096];
    size_t length;
    FILE *file = fopen(path, "rb");

    if (!file) return 0;
    length = fread(buffer, 1u, sizeof(buffer) - 1u, file);
    fclose(file);
    buffer[length] = '\0';
    return strstr(buffer, needle) != NULL;
}

static int convert_file(const char *input, const char *output,
                        QwOutputFormat target, QwConvertReport *report)
{
    remove(output);
    return qw_convert_file_to_format(input, output, target, NULL, report);
}

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *bytes;

    if (!file) return NULL;
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (unsigned char *)malloc((size_t)length);
    if (!bytes ||
        fread(bytes, 1u, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return bytes;
}

static int contains(const unsigned char *data, size_t size,
                    const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static int html_contains(const char *html, size_t size, const char *needle)
{
    if (!html) return 0;
    return size >= strlen(needle) && strstr(html, needle) != NULL;
}

int main(void)
{
    static const char srt_text[] =
        "1\n00:00:01,000 --> 00:00:03,500\nHello, world!\n\n"
        "2\n00:00:04,000 --> 00:00:06,000\nSecond cue\n";
    static const char json_text[] =
        "{\"name\":\"quirewise\",\"count\":3,\"ok\":true}";
    static const char csv_text[] = "a,b\n1,\"two, three\"\n";
    QwConvertReport report;
    int rc;

    if (!write_text("qw_convert_family.srt", srt_text) ||
        !write_text("qw_convert_family.json", json_text) ||
        !write_text("qw_convert_family.csv", csv_text)) {
        fputs("FAIL: cannot create inputs\n", stderr);
        return 1;
    }

    /* Subtitle family: SRT -> WebVTT. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.srt", "qw_convert_family.vtt",
                      QW_FORMAT_VTT, &report);
    if (rc != QW_STATUS_OK || !file_contains("qw_convert_family.vtt", "WEBVTT") ||
        !file_contains("qw_convert_family.vtt", "Hello, world!")) {
        fprintf(stderr, "FAIL: SRT to WebVTT (%s)\n", report.note);
        return 1;
    }

    /* Data family: JSON -> YAML -> JSON. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.json", "qw_convert_family.yaml",
                      QW_FORMAT_YAML, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.yaml", "quirewise")) {
        fprintf(stderr, "FAIL: JSON to YAML (%s)\n", report.note);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.yaml", "qw_convert_family.json2",
                      QW_FORMAT_JSON, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.json2", "quirewise")) {
        fprintf(stderr, "FAIL: YAML to JSON (%s)\n", report.note);
        return 1;
    }

    /* Worksheet text family: CSV -> TSV. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.csv", "qw_convert_family.tsv",
                      QW_FORMAT_TSV, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.tsv", "two, three")) {
        fprintf(stderr, "FAIL: CSV to TSV (%s)\n", report.note);
        return 1;
    }

    /* Layout text writers: markdown -> HTML, HTML -> markdown. */
    if (!write_text("qw_convert_family.md",
                    "# Title\n\nHello **bold** and *italic*.\n"))
        return 1;
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.md", "qw_convert_family.md.html",
                      QW_FORMAT_HTML, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.md.html", "<h1>Title</h1>")) {
        fprintf(stderr, "FAIL: Markdown to HTML (%s)\n", report.note);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.md.html", "qw_convert_family.out.md",
                      QW_FORMAT_MARKDOWN, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.out.md", "# Title") ||
        !file_contains("qw_convert_family.out.md", "**bold**")) {
        fprintf(stderr, "FAIL: HTML to Markdown (%s)\n", report.note);
        return 1;
    }

    /* Worksheets and plain text also have an HTML target. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.csv", "qw_convert_family.csv.html",
                      QW_FORMAT_HTML, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.csv.html", "<table>") ||
        !file_contains("qw_convert_family.csv.html", "two, three")) {
        fprintf(stderr, "FAIL: CSV to HTML (%s)\n", report.note);
        return 1;
    }
    if (!write_text("qw_convert_family.txt", "Plain text body\n"))
        return 1;
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.txt", "qw_convert_family.txt.html",
                      QW_FORMAT_HTML, &report);
    if (rc != QW_STATUS_OK ||
        !file_contains("qw_convert_family.txt.html", "Plain text body")) {
        fprintf(stderr, "FAIL: TXT to HTML (%s)\n", report.note);
        return 1;
    }

    /* Structured data has no layout route. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.json", "qw_convert_family.json.html",
                      QW_FORMAT_HTML, &report);
    if (rc == QW_STATUS_OK) {
        fputs("FAIL: JSON to HTML was accepted\n", stderr);
        return 1;
    }

    /* Binary office writers: DOCX and EPUB round-trip through the readers. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.md", "qw_convert_family.docx",
                      QW_FORMAT_DOCX, &report);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "FAIL: Markdown to DOCX (%s)\n", report.note);
        return 1;
    }
    {
        unsigned char *bytes;
        size_t bytes_size = 0u;
        QwDocxDocument *document = NULL;
        const uint8_t *docx_html;
        size_t docx_html_size = 0u;

        bytes = read_file("qw_convert_family.docx", &bytes_size);
        if (!bytes) {
            fputs("FAIL: cannot read the DOCX output\n", stderr);
            return 1;
        }
        rc = (int)qw_docx_parse(bytes, bytes_size, &document);
        free(bytes);
        if (rc != QW_DOCX_OK || !document) {
            fprintf(stderr, "FAIL: DOCX output did not parse (%d)\n", rc);
            return 1;
        }
        docx_html = qw_docx_document_html(document, &docx_html_size);
        if (!html_contains((const char *)docx_html, docx_html_size, "Title") ||
            !html_contains((const char *)docx_html, docx_html_size, "bold")) {
            fputs("FAIL: DOCX output lost its text\n", stderr);
            return 1;
        }
        qw_docx_document_free(document);
    }

    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.md", "qw_convert_family.epub",
                      QW_FORMAT_EPUB, &report);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "FAIL: Markdown to EPUB (%s)\n", report.note);
        return 1;
    }
    {
        unsigned char *bytes;
        size_t bytes_size = 0u;
        QwEpubDocument *document = NULL;

        bytes = read_file("qw_convert_family.epub", &bytes_size);
        if (!bytes) {
            fputs("FAIL: cannot read the EPUB output\n", stderr);
            return 1;
        }
        rc = (int)qw_epub_parse(bytes, bytes_size, &document);
        free(bytes);
        if (rc != QW_EPUB_OK || !document) {
            fprintf(stderr, "FAIL: EPUB output did not parse (%d)\n", rc);
            return 1;
        }
        if (qw_epub_section_count(document) < 1u) {
            fputs("FAIL: EPUB output has no sections\n", stderr);
            return 1;
        }
        {
            QwEpubSectionView section;
            size_t paragraph_index;
            int saw_title = 0;
            int saw_text = 0;

            memset(&section, 0, sizeof(section));
            if (qw_epub_section_at(document, 0u, &section) == QW_EPUB_OK) {
                for (paragraph_index = 0u;
                     paragraph_index < section.paragraph_count &&
                     (!saw_title || !saw_text);
                     ++paragraph_index) {
                    QwEpubParagraphView paragraph;
                    size_t run_index;

                    memset(&paragraph, 0, sizeof(paragraph));
                    if (qw_epub_paragraph_at(document, 0u, paragraph_index,
                                             &paragraph) != QW_EPUB_OK)
                        continue;
                    for (run_index = 0u; run_index < paragraph.run_count;
                         ++run_index) {
                        QwEpubRunView run;
                        char text[256];
                        size_t copy;

                        memset(&run, 0, sizeof(run));
                        if (qw_epub_run_at(document, 0u, paragraph_index,
                                           run_index, &run) != QW_EPUB_OK)
                            continue;
                        copy = run.text_size < sizeof(text) - 1u
                                   ? run.text_size : sizeof(text) - 1u;
                        memcpy(text, run.utf8_text, copy);
                        text[copy] = '\0';
                        if (strstr(text, "Title")) saw_title = 1;
                        if (strstr(text, "bold")) saw_text = 1;
                    }
                }
            }
            if (!saw_title || !saw_text) {
                fputs("FAIL: EPUB output lost its text\n", stderr);
                return 1;
            }
        }
        qw_epub_document_free(document);
    }

    /* Container writers: HTMLZ, TXTZ and HWPX round-trip through the readers. */
    {
        unsigned char *bytes;
        size_t bytes_size = 0u;
        QwHtmlzDocument htmlz;
        const char *error = NULL;

        memset(&report, 0, sizeof(report));
        rc = convert_file("qw_convert_family.md", "qw_convert_family.htmlz",
                          QW_FORMAT_HTMLZ, &report);
        if (rc != QW_STATUS_OK) {
            fprintf(stderr, "FAIL: Markdown to HTMLZ (%s)\n", report.note);
            return 1;
        }
        bytes = read_file("qw_convert_family.htmlz", &bytes_size);
        memset(&htmlz, 0, sizeof(htmlz));
        if (!bytes ||
            qw_htmlz_extract_primary_document(bytes, bytes_size, &htmlz,
                                              &error) != QW_HTMLZ_OK ||
            !contains(htmlz.html_bytes, htmlz.html_size,
                      "Title")) {
            free(bytes);
            fputs("FAIL: HTMLZ output lost its text\n", stderr);
            return 1;
        }
        qw_htmlz_document_free(&htmlz);
        free(bytes);
    }
    {
        unsigned char *bytes;
        size_t bytes_size = 0u;
        QwTextArchiveDocument archive;
        const char *error = NULL;

        memset(&report, 0, sizeof(report));
        rc = convert_file("qw_convert_family.md", "qw_convert_family.txtz",
                          QW_FORMAT_TXTZ, &report);
        if (rc != QW_STATUS_OK) {
            fprintf(stderr, "FAIL: Markdown to TXTZ (%s)\n", report.note);
            return 1;
        }
        bytes = read_file("qw_convert_family.txtz", &bytes_size);
        memset(&archive, 0, sizeof(archive));
        if (!bytes ||
            qw_text_archive_extract_primary(bytes, bytes_size,
                                            QW_TEXT_ARCHIVE_TXTZ, &archive,
                                            &error) != QW_TEXT_ARCHIVE_OK ||
            !contains(archive.text_bytes, archive.text_size,
                      "Title")) {
            free(bytes);
            fputs("FAIL: TXTZ output lost its text\n", stderr);
            return 1;
        }
        qw_text_archive_document_free(&archive);
        free(bytes);
    }
    {
        unsigned char *bytes;
        size_t bytes_size = 0u;
        QwHwpxDocument *document = NULL;
        const uint8_t *html;
        size_t html_size = 0u;

        memset(&report, 0, sizeof(report));
        rc = convert_file("qw_convert_family.md", "qw_convert_family.hwpx",
                          QW_FORMAT_HWPX, &report);
        if (rc != QW_STATUS_OK) {
            fprintf(stderr, "FAIL: Markdown to HWPX (%s)\n", report.note);
            return 1;
        }
        bytes = read_file("qw_convert_family.hwpx", &bytes_size);
        if (!bytes ||
            qw_hwpx_parse(bytes, bytes_size, &document) != QW_HWPX_OK ||
            !document) {
            free(bytes);
            fputs("FAIL: HWPX output did not parse\n", stderr);
            return 1;
        }
        free(bytes);
        html = qw_hwpx_document_html(document, &html_size);
        if (!contains(html, html_size, "Title")) {
            qw_hwpx_document_free(document);
            fputs("FAIL: HWPX output lost its text\n", stderr);
            return 1;
        }
        qw_hwpx_document_free(document);
    }

    /* PDF has no layout route, so neither office target is available. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.json", "qw_convert_family.docx2",
                      QW_FORMAT_DOCX, &report);
    if (rc == QW_STATUS_OK) {
        fputs("FAIL: JSON to DOCX was accepted\n", stderr);
        return 1;
    }

    /* A memory HTML payload sniffs into Markdown without a path. */
    {
        static const char html_text[] =
            "<html><body><h2>Memory</h2><p>body</p></body></html>";
        uint8_t *output = NULL;
        size_t output_size = 0u;

        memset(&report, 0, sizeof(report));
        rc = qw_convert_to_format_memory(html_text, strlen(html_text),
                                         QW_FORMAT_MARKDOWN, NULL, &output,
                                         &output_size, &report);
        if (rc != QW_STATUS_OK || !output || !output_size ||
            strstr((const char *)output, "## Memory") == NULL) {
            fprintf(stderr, "FAIL: memory HTML to Markdown (%s)\n",
                    report.note);
            return 1;
        }
        qw_buffer_free(output);
    }

    /* A family mismatch is refused with a family-specific note. */
    memset(&report, 0, sizeof(report));
    rc = convert_file("qw_convert_family.json", "qw_convert_family.vtt2",
                      QW_FORMAT_SRT, &report);
    if (rc == QW_STATUS_OK || !report.note[0]) {
        fputs("FAIL: JSON to SRT was accepted\n", stderr);
        return 1;
    }

    /* The memory entry point has no extension and sniffs the payload. */
    {
        uint8_t *output = NULL;
        size_t output_size = 0u;

        memset(&report, 0, sizeof(report));
        rc = qw_convert_to_format_memory(json_text, strlen(json_text),
                                         QW_FORMAT_YAML, NULL, &output,
                                         &output_size, &report);
        if (rc != QW_STATUS_OK || !output || !output_size) {
            fprintf(stderr, "FAIL: memory JSON to YAML (%s)\n", report.note);
            return 1;
        }
        if (!strstr((const char *)output, "quirewise")) {
            fputs("FAIL: memory YAML lost the value\n", stderr);
            return 1;
        }
        qw_buffer_free(output);

        output = NULL;
        output_size = 0u;
        memset(&report, 0, sizeof(report));
        rc = qw_convert_to_format_memory(srt_text, strlen(srt_text),
                                         QW_FORMAT_VTT, NULL, &output,
                                         &output_size, &report);
        if (rc != QW_STATUS_OK || !output || !output_size ||
            !strstr((const char *)output, "WEBVTT")) {
            fprintf(stderr, "FAIL: memory SRT to WebVTT (%s)\n", report.note);
            return 1;
        }
        qw_buffer_free(output);
    }

    remove("qw_convert_family.srt");
    remove("qw_convert_family.json");
    remove("qw_convert_family.csv");
    remove("qw_convert_family.vtt");
    remove("qw_convert_family.yaml");
    remove("qw_convert_family.json2");
    remove("qw_convert_family.tsv");
    remove("qw_convert_family.vtt2");
    remove("qw_convert_family.md");
    remove("qw_convert_family.md.html");
    remove("qw_convert_family.out.md");
    remove("qw_convert_family.csv.html");
    remove("qw_convert_family.txt");
    remove("qw_convert_family.txt.html");
    remove("qw_convert_family.json.html");
    remove("qw_convert_family.docx");
    remove("qw_convert_family.docx2");
    remove("qw_convert_family.epub");
    remove("qw_convert_family.htmlz");
    remove("qw_convert_family.txtz");
    remove("qw_convert_family.hwpx");
    puts("Quirewise cross-family conversion tests passed");
    return 0;
}
