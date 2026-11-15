/* User-password PDF decryption tests.
 *
 * encrypted-mutool-aes256-userpw.pdf was produced by `mutool clean -E
 * aes-256 -O owner -U secret`, so the user password is "secret". The reader
 * must reject it without a password, accept it with the right one, and expose
 * the decrypted text through the convert options as well.
 */

#include <quirewise/convert.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_USERPW_PDF
#error "QUIREWISE_USERPW_PDF must be defined"
#endif

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return 0;
    }
    if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

static int contains(const char *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main(void)
{
    uint8_t *data = NULL;
    size_t size = 0u;
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_USERPW_PDF, &data, &size) || !data || !size)
        fail("cannot read the user-password fixture");

    /* Without a password the full reader must refuse the document. */
    memset(&report, 0, sizeof(report));
    if (qw_pdf_open(data, size, &document, &pages, &report) == QW_STATUS_OK)
        fail("encrypted PDF opened without a password");
    document = NULL;

    /* A wrong password must also be rejected. */
    memset(&report, 0, sizeof(report));
    if (qw_pdf_open_with_password(data, size, "wrong", &document, &pages,
                                  &report) == QW_STATUS_OK)
        fail("wrong user password was accepted");
    document = NULL;

    /* The correct user password unlocks the page tree and text layer. */
    memset(&report, 0, sizeof(report));
    if (qw_pdf_open_with_password(data, size, "secret", &document, &pages,
                                  &report) != QW_STATUS_OK ||
        !document || !pages)
        fail("correct user password was rejected");
    {
        uint8_t *text = NULL;
        size_t text_size = 0u;

        if (qw_pdf_extract_text(document, &text, &text_size, &report) !=
                QW_STATUS_OK ||
            !text || !text_size || !contains((const char *)text, text_size,
                                             "AES256"))
            fail("decrypted text layer is missing");
        qw_pdf_free(text);
    }
    qw_pdf_close(document);

    /* The convert options carry the password into the PDF routes. */
    {
        QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
        uint8_t *text = NULL;
        size_t text_size = 0u;

        options.pdf_password = "secret";
        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(data, size, QW_FORMAT_TXT, &options,
                                        &text, &text_size, &report) !=
                QW_STATUS_OK ||
            !text || !text_size || !contains((const char *)text, text_size,
                                             "AES256"))
            fail("convert-to-TXT did not use the PDF password");
        qw_buffer_free(text);

        options.pdf_password = NULL;
        text = NULL;
        text_size = 0u;
        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(data, size, QW_FORMAT_TXT, &options,
                                        &text, &text_size, &report) ==
            QW_STATUS_OK) {
            qw_buffer_free(text);
            fail("convert-to-TXT accepted the PDF without a password");
        }
    }

    /* PDF -> PNG also honours the password (raster route). */
    {
        QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
        uint8_t *png = NULL;
        size_t png_size = 0u;

        options.pdf_password = "secret";
        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(data, size, QW_FORMAT_PNG, &options,
                                        &png, &png_size, &report) !=
                QW_STATUS_OK ||
            !png || png_size < 8u || png[0] != 0x89u)
            fail("password-protected PDF did not render to PNG");
        qw_buffer_free(png);
    }

    free(data);
    puts("Quirewise PDF password tests passed");
    return 0;
}
