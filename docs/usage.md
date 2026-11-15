# quirewise-document usage guide

This guide covers the workflows of `Quirewise::Document`. Public declarations
are in the nine headers under `include/quirewise/`, plus the
`Quirewise::Runtime` types (`QwStatus`, `QwFormatId`, `QwConvertOptions`,
`QwConvertReport`) reachable through `quirewise/convert.h`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Consumer CMake:

```cmake
find_package(QuirewiseDocument CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Document)
```

Include `<quirewise/convert.h>` for conversion and `<quirewise/pdf.h>` for the
reader/renderer/merger. `<quirewise/document.h>` (Runtime) defines
`QwPageImage`. There is no init/teardown call; shared builds use
`-DQUIREWISE_BUILD_SHARED=ON`.

## 2. Core model and ownership

Memory conversion borrows input bytes and returns a new output buffer. PDF
reading borrows document bytes and returns an opaque handle that keeps
borrowing them until close.

| Producer | Result ownership | Release with |
|---|---|---|
| `qw_convert_to_pdf_memory`, `qw_convert_to_format_memory`, `qw_raster_pages_to_pdf*` | caller | `qw_buffer_free` |
| `qw_pdf_extract_text`, `qw_pdf_merge` | caller | `qw_pdf_free` |
| `qw_pdf_open*` | caller | `qw_pdf_close` |
| `qw_pdf_render_page` | caller | `qw_pdf_page_free` |
| typed `qw_*_parse` / `*_open` | caller | matching `qw_*_free` / `close` |
| `qw_xps_document_read_page` | caller | `qw_xps_data_free` |

Release functions accept `NULL`. On failure, output pointers are `NULL` and
`report.note` normally explains why. A report is optional.

## 3. Convert a file to PDF

`qw_convert_file` reads one input, dispatches on content and extension, and
writes one PDF. Existing output paths are never overwritten.

```c
#include <quirewise/convert.h>
#include <stdio.h>
#include <string.h>

static int convert_to_pdf(const char *input, const char *output)
{
    QwConvertReport report;
    int rc;
    memset(&report, 0, sizeof report);
    rc = qw_convert_file(input, output, NULL, &report);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "%s: %d (%s)\n", input, rc,
                report.note[0] ? report.note : "no detail");
        return 0;
    }
    printf("%zu page(s), %zu byte(s)\n",
           report.pages_written, report.output_size);
    return 1;
}
```

`qw_convert_file_to_format` adds a target format. The output extension should
match that target; the input extension distinguishes CSV, text and spreadsheet
files.

## 4. Convert memory to PDF

`qw_convert_to_pdf_memory` works without a filename for content-recognizable
inputs: PDF, images, EML, MOBI/AZW3, RTF, ZIP office packages, EPUB, FB2, flat
ODF/XML and HTML. Plain text, CSV and spreadsheets need the file API's
extension hint.

```c
#include <quirewise/convert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const char html[] =
        "<html><body><h1>Invoice</h1><p>Converted in memory.</p></body></html>";
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;

    memset(&report, 0, sizeof report);
    if (qw_convert_to_pdf_memory(html, sizeof html - 1u, NULL, &pdf,
                                 &pdf_size, &report) != QW_STATUS_OK) {
        fprintf(stderr, "failed: %s\n", report.note);
        return 1;
    }
    printf("PDF is %zu byte(s)\n", pdf_size);
    qw_buffer_free(pdf);
    return 0;
}
```

The result is `malloc`-owned; release it with `qw_buffer_free` on every
success path. A PDF input is copied through unchanged and the report says so.
Only `options->page_mode` is consumed here, for image inputs.

## 5. Convert PDF to TXT and PNG

For PDF input, `qw_convert_to_format_memory` supports two non-PDF outputs:

- `QW_FORMAT_TXT`: UTF-8 text from every page; the note says no OCR ran.
- `QW_FORMAT_PNG`: first page only, fixed 144 dpi, RGB24. Use
  `qw_pdf_render_page` for another page or resolution.

```c
static int pdf_text_and_png(const uint8_t *pdf, size_t pdf_size)
{
    QwConvertReport report;
    uint8_t *text = NULL, *png = NULL;
    size_t text_size = 0u, png_size = 0u;
    int rc;

    memset(&report, 0, sizeof report);
    rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_TXT, NULL,
                                     &text, &text_size, &report);
    if (rc != QW_STATUS_OK) return 0;
    fwrite(text, 1u, text_size, stdout);
    qw_buffer_free(text);

    rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_PNG, NULL,
                                     &png, &png_size, &report);
    if (rc != QW_STATUS_OK) return 0;
    qw_buffer_free(png);
    return 1;
}
```

`QW_FORMAT_PDF` is a byte-identical passthrough. `QW_FORMAT_JPG` is refused
with `QW_STATUS_ERR_UNSUPPORTED` because this package has no JPEG encoder. On
non-PDF input the same function supports only PDF output.

## 6. Check a file

`qw_check_file` validates without writing output. PDFs are opened and every
page rendered at 72 dpi; other non-audio inputs are converted in memory and
discarded. Peak memory can resemble a real conversion; audio is unsupported.

```c
QwConvertReport report;
int rc;

memset(&report, 0, sizeof report);
rc = qw_check_file("suspicious.docx", NULL, &report);
if (rc == QW_STATUS_OK)
    printf("ok: %s, %zu page(s)\n",
           qw_format_name(report.format), report.pages_written);
else
    fprintf(stderr, "cannot read: %d (%s)\n", rc, report.note);
```

Output- and page-selection options do not apply to a check; all pages are
covered.

## 7. Read PDF pages and merge

`qw_pdf_open` returns an opaque document and a page count. Boxes are PDF points
(`x0/y0/x1/y1`); a rendered `QwPageImage` has `kind == QW_PAGE_IMAGE_RGB24`.
The input bytes must stay valid until `qw_pdf_close`.

```c
QwConvertReport report;
QwPdfDocument *document = NULL;
size_t pages = 0u, i;

memset(&report, 0, sizeof report);
if (qw_pdf_open(pdf, pdf_size, &document, &pages, &report) == QW_STATUS_OK) {
    for (i = 0u; i < pages; ++i) {
        double media[4], crop[4];
        int rotation = 0;
        QwPageImage image;

        if (qw_pdf_page_geometry(document, i, media, crop, &rotation) !=
            QW_STATUS_OK)
            continue;
        memset(&image, 0, sizeof image);
        if (qw_pdf_render_page(document, i, 144u, &image, &report) ==
            QW_STATUS_OK) {
            /* image.data is RGB24: image.width * 3 bytes per row. */
            qw_pdf_page_free(&image);
        }
    }
    qw_pdf_close(document);
}
```

`qw_pdf_extract_text` returns one UTF-8 buffer for the whole document; pages
with no text contribute nothing. `qw_pdf_open_structure` keeps page count and
geometry readable when content needs an unsupported user password; extraction
and rendering then fail with `QW_STATUS_ERR_UNSUPPORTED`.

Merging accepts one or more unencrypted PDFs and preserves input page order:

```c
const uint8_t *inputs[2] = { pdf_a, pdf_b };
size_t input_sizes[2] = { size_a, size_b };
uint8_t *merged = NULL;
size_t merged_size = 0u, merged_pages = 0u;
int rc = qw_pdf_merge(inputs, input_sizes, 2u, &merged, &merged_size,
                      &merged_pages, &report);
if (rc == QW_STATUS_OK) {
    printf("%zu page(s), %zu byte(s)\n", merged_pages, merged_size);
    qw_pdf_free(merged);
}
```

The merger caps the result at 100,000 pages and 512 MiB per input. It is a
structural merge, not an optimiser, so the output can exceed its inputs.

## 8. Office and e-book dispatch

`qw_convert_file` and `qw_convert_to_pdf_memory` share one dispatcher. These
families produce PDF through the package's layout writers:

| Input | Route and fidelity |
|---|---|
| DOCX family, ODT family | native layout; text, paragraphs, simple tables |
| RTF | RTF-to-HTML conversion, then layout |
| EML | selected body; attachments listed, not exported |
| HWPX | paragraph text and simple tables |
| PPTX family, ODP family | slide text; shapes/images/notes/styles omitted |
| EPUB | reflowed HTML; images and CSS approximated |
| FB2 | FictionBook HTML; images approximated |
| MOBI/AZW3 | PalmDOC text; markup, styles, images and DRM not preserved |
| HTML | block-level subset; inline markup flattened |
| TXT, `.md`, `.markdown` | plain-text paragraphs; Markdown is not rendered |
| XLSX/XLSM/XLTX, XLS/XLT, ODS/OTS/FODS | cells rendered as tables |
| CSV/TSV | cells rendered as an HTML table |
| HTMLZ, TXTZ, PMLZ | primary member rendered; other members dropped |
| Probed images (PNG, JPEG, GIF, BMP, WebP, ...) | one image page per input |

`options->page_mode` selects `QW_RASTER_PAGE_MODE_IMAGE` (page box equals pixel
size) or `QW_RASTER_PAGE_MODE_A4` for image input. Unknown ZIP packages, SVG
and TTML are refused with an explanatory note; other Quirewise packages own
those routes.

## 9. Reports and error handling

Every fallible entry point returns `int`. Conversion, PDF and `qw_check_file`
use `QwStatus`; XPS uses `QwXpsStatus` with the same numeric layout:

| Status | Meaning |
|---|---|
| `QW_STATUS_OK` (0) | success |
| `QW_STATUS_ERR_INVALID_ARGUMENT` (-1) | null or invalid argument |
| `QW_STATUS_ERR_OUT_OF_MEMORY` (-2) | allocation failure |
| `QW_STATUS_ERR_IO` (-3) | file open/read/write failure |
| `QW_STATUS_ERR_FORMAT` (-4) | recognized input, malformed contents |
| `QW_STATUS_ERR_UNSUPPORTED` (-5) | no route, unsupported feature or encryption |
| `QW_STATUS_ERR_DECODE` (-6) | content stream could not be decoded |
| `QW_STATUS_ERR_EMPTY` (-8) | reserved; not emitted by these routes |
| `QW_STATUS_ERR_EXTERNAL_DECODER_REQUIRED` (-9) | reserved for routes owned elsewhere |
| `QW_STATUS_ERR_RESOURCE_LIMIT` (-11) | a documented cap was exceeded |

`UNSUPPORTED` often means the package deliberately does not own the format;
`FORMAT` means the bytes claim a supported format but are malformed.
`QwDocxStatus` is positive 0-7. `QwEmailStatus` is -1, -2, -4, -5, -6 and -11
with `QW_EMAIL_ERROR_UNKNOWN = -100`. ODT/ODP/PPTX/HWPX/XPS use 0 and -1..-6.

`QwConvertReport` is diagnostic only; the return value is authoritative:

- `format` — detected input format where detection ran;
- `pages_written` — pages emitted, or pages checked by `qw_check_file`;
- `output_size` — bytes produced;
- `skipped_pages` — pages the route omitted;
- `note[192]` — short NUL-terminated diagnostic;
- `output_format` — requested target;
- `stage` — last active `QwConvertStage` (`PROBE`, `OPEN`, `PLAN`, `EXECUTE`,
  `PUBLISH`).

Initialize with `memset` or `{0}`.

## 10. Memory, lifetime and threading

- No init/teardown. Inputs and outputs are caller-owned; `QwPdfDocument` and
  `QwXpsDocument` borrow their input until close. Conversion never frees input.
- Use the matching release: `qw_buffer_free` for conversion/raster outputs,
  `qw_pdf_free` for text/merge, `qw_pdf_page_free` for rasters.
- Separate calls, documents and buffers are independent and may run
  concurrently. Do not share one `QwPdfDocument` between threads:
  `qw_pdf_extract_text` caches its result inside the handle. Lock it or open
  one document per thread.
- `QwConvertOptions` and `QwConvertReport` are per-call and never retained.

## 11. Resource limits

| Area | Limit |
|---|---|
| File conversion input | 512 MiB per file |
| PDF pages | 100,000 |
| PDF stream | 64 MiB |
| Extracted PDF text | 256 MiB |
| Render resolution | 1-1200 dpi; 0 selects 72 dpi |
| Render dimensions | 100,000 pixels per side; RGB24 |
| Merge | 100,000 pages, 1,000,000 objects, 512 MiB per input |
| Office package archives | 10,000 entries, 128 MiB per member, 512 MiB total |
| Image side | 32,768 pixels |

Readers still allocate whole inputs and outputs in memory; these limits bound
malformed files, not peak memory. Budget roughly input + output + render
buffers.

## 12. Deliberate boundaries

- **No PDF/A.** `options->pdfa_level` is not interpreted here; there
  is no PDF/A writer or verifier. PDF/A files can still be read as PDFs.
- **No OCR.** PDF-to-TXT extracts text operators already present; image inputs
  become image-only PDFs.
- **No CJK ToUnicode synthesis or embedded-font rendering.** The PDF writer
  embeds no font file and emits no `/ToUnicode`; non-ASCII uses the predefined
  `STSong-Light` CID font by name. The rasteriser uses a built-in 3x5 ASCII
  approximation font and does not load embedded programs, so CJK glyph shapes
  are not reproduced. Extraction consumes an existing `/ToUnicode` CMap.
- **No HTML or Markdown layout output target.** PDF is still the only layout
  output for non-PDF inputs. Structured data, subtitles and worksheets are the
  exception: `qw_convert_file_to_format` / `qw_convert_to_format_memory` route
  JSON/JSONL/YAML/TOML/vCard/XML, SRT/WebVTT/LRC/MicroDVD/ASS/SSA/SAMI/SCC and
  CSV/TSV targets to the owning package's writer. For PDF inputs only PDF, TXT
  and PNG are available.
- **XPS is reader-only.** `qw_xps_document_open` and
  `qw_xps_document_read_page` work, but the conversion dispatch and the
  application CLI do not route `.xps`, so `qw_convert_file("a.xps", ...)`
  returns `QW_STATUS_ERR_UNSUPPORTED`.
- **No external PDF engine.** Rendering covers the implemented subset;
  unsupported operators are noted and may leave a page partially drawn.
- **Encryption requires the empty user password.** RC4, AES-128 and AES-256
  are supported only in that mode; merging encrypted PDFs is refused.

## 13. Related packages

`Quirewise::Runtime` supplies the shared status, format and report types.
`Quirewise::Archive`, `Compression`, `Data`, `Ebook`, `Image`, `Crypto`,
`Spreadsheet` and `Text` are private format dependencies; `Quirewise::Pipeline`
and the application consume this package.

## 14. License
MIT. See `LICENSE`.
