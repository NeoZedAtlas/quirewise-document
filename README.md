# quirewise-document

PDF reading, rasterisation, text extraction and merging, plus the Quirewise
document-conversion dispatch for office files, e-books, email and images.

`Quirewise::Document` (CMake package `QuirewiseDocument`) is an independent,
MIT-licensed C11 library. It is installed as a CMake package and can be
consumed without the Quirewise application.

## What it does

- Converts a file or an in-memory buffer to PDF. The dispatch covers HTML,
  DOCX/DOCM/DOTX/DOTM, ODT/OTT/FODT, RTF, EML, HWPX, PPTX and ODP families,
  XLSX/XLS/ODS and CSV/TSV, EPUB, FB2, MOBI/AZW3, plain text, common image
  formats, HTMLZ, TXTZ and PMLZ.
- Converts PDF input to plain UTF-8 text or to a PNG of the first page, and
  passes PDF-to-PDF requests through byte for byte.
- Reads PDFs: classic cross-reference tables, cross-reference streams, object
  streams, incremental updates, and RC4/AES standard security with an empty
  user password. It exposes page count, media/crop boxes and rotation.
- Rasterises PDF pages to RGB24 at a chosen resolution, and extracts the text
  of every page as UTF-8.
- Merges unencrypted PDFs into one classic-cross-reference PDF.
- Checks whether a file can be read through its registered route without
  writing an output file.
- Renders caller-supplied gray or RGB raster pages into a PDF.
- Exposes typed readers for DOCX, ODT, ODP, PPTX, HWPX, EML and XPS when a
  caller needs HTML, layout metadata or attachment/page data instead of PDF.

The file API supplies the extension hint that plain text, CSV and spreadsheet
inputs need; the memory API relies on content detection.

Everything is implemented in this package and its Quirewise dependencies. No
MuPDF, Ghostscript, OCR engine or external PDF toolkit is involved.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- CTest for the test suite.
- Quirewise CMake packages: `QuirewiseRuntime`, `QuirewiseArchive`,
  `QuirewiseData`, `QuirewiseCompression`, `QuirewiseEbook`, `QuirewiseImage`,
  `QuirewiseCrypto`, `QuirewiseSpreadsheet` and `QuirewiseText`.
- No third-party runtime libraries beyond those packages.

`Quirewise::Runtime` is a public dependency because `convert.h` exposes
`QwConvertOptions`, `QwConvertReport`, `QwStatus` and `QwFormatId` from it. The
other packages are linked privately.

## Build, test and install

Install the dependency packages first, or point CMake at their install prefix:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

If all dependencies are already in a default CMake prefix, omit
`-DCMAKE_PREFIX_PATH`.

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`. On Windows,
shared builds export all symbols through `WINDOWS_EXPORT_ALL_SYMBOLS`.

## Use it from CMake

```cmake
find_package(QuirewiseDocument CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Document)
```

Pass `-DCMAKE_PREFIX_PATH=/path/to/prefix` when configuring your project if the
package is installed outside a default prefix. The installed package config
finds the Quirewise dependencies listed above.

## Quick start

Convert one file to PDF. Output paths are never overwritten, and the report
carries a human-readable note on both success and failure.

```c
#include <quirewise/convert.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    QwConvertReport report;
    int rc;

    memset(&report, 0, sizeof report);
    rc = qw_convert_file("book.odt", "book.pdf", NULL, &report);
    if (rc != QW_STATUS_OK) {
        fprintf(stderr, "conversion failed: %d (%s)\n", rc,
                report.note[0] ? report.note : "no detail");
        return 1;
    }
    printf("wrote %zu byte(s), %zu page(s)\n",
           report.output_size, report.pages_written);
    return 0;
}
```

For in-memory input, call `qw_convert_to_pdf_memory` and release the result
with `qw_buffer_free`. See [docs/usage.md](docs/usage.md) for that and the PDF
reader recipes.

`qw_convert_file_to_format` and `qw_convert_to_format_memory` accept more than
PDF/TXT/PNG: JSON, JSON Lines, YAML, TOML, vCard, XML (structured data), SRT,
WebVTT, LRC, MicroDVD, ASS, SSA, SAMI, SCC (subtitles) and CSV/TSV (worksheets)
are routed to the package that owns the writer. A file is parsed by its own
family parser (selected by extension, or by an unambiguous content sniff for the
memory entry point); a file from another family is refused with a
family-specific note. PDF, TXT and PNG keep the native document routes. `QW_FORMAT_HTML` returns the
readers' HTML intermediate (every layout reader also exposes its own
`*_document_html`); `QW_FORMAT_MARKDOWN` converts that HTML through
`quirewise-text`'s `qw_markdown_from_html`; `QW_FORMAT_DOCX`/`QW_FORMAT_EPUB`
emit minimal office packages; `QW_FORMAT_HTMLZ`/`QW_FORMAT_TXTZ` build the
container writers from the ebook package; and `QW_FORMAT_HWPX` emits a minimal
OWPML package (`qw_hwpx_create`). PDF, image and subtitle inputs have no layout
HTML route and are refused with a specific note.

## API overview

| Header | Purpose |
|---|---|
| `quirewise/convert.h` | file/memory conversion, format detection, raster-to-PDF, `qw_buffer_free` |
| `quirewise/pdf.h` | PDF open, structure, geometry, render, text extraction, merge, release |
| `quirewise/docx.h` | DOCX-family probe, parse, HTML and page layout |
| `quirewise/odt.h` | ODT/OTT/FODT parse, HTML |
| `quirewise/odf_presentation.h` | ODP/OTP/FODP parse, HTML, slide count |
| `quirewise/pptx.h` | PPTX-family parse, HTML |
| `quirewise/hwpx.h` | HWPX parse, HTML, section/omitted-object counts |
| `quirewise/email.h` | EML probe, parse, HTML, attachments |
| `quirewise/xps.h` | XPS open, page count, page XML, page geometry |

| Task | Functions |
|---|---|
| File conversion | `qw_convert_file`, `qw_convert_file_to_format` |
| Memory to PDF | `qw_convert_to_pdf_memory` |
| Memory to a chosen format | `qw_convert_to_format_memory` |
| Validate a file | `qw_check_file` |
| Detect / name a format | `qw_detect_format`, `qw_format_name` |
| Raster pages to PDF | `qw_raster_pages_to_pdf`, `qw_raster_pages_to_pdf_mode` |
| Open / inspect a PDF | `qw_pdf_open`, `qw_pdf_open_structure`, `qw_pdf_page_geometry` |
| Render / extract / merge | `qw_pdf_render_page`, `qw_pdf_extract_text`, `qw_pdf_merge` |
| Release | `qw_buffer_free`, `qw_pdf_close`, `qw_pdf_free`, `qw_pdf_page_free` |

## Behavior and limits

- Conversion and PDF functions return `QW_STATUS_OK` (0) or a negative
  `QwStatus` error code. The compact format readers use their own enums; see
  the usage guide.
- Inputs are borrowed only for the call, except PDF and XPS reader inputs,
  which must stay valid until their close call.
- Conversion outputs are `malloc`-owned and released with `qw_buffer_free`.
  PDF text and merged PDFs use `qw_pdf_free`; page rasters use
  `qw_pdf_page_free`.
- A failed conversion may leave `report.note` explaining the refusal. Output
  pointers are set to `NULL` on failure.
- Family targets (JSON/YAML/TOML/vCard/XML, SRT/VTT/LRC/MicroDVD/ASS/SSA/SAMI/
  SCC, CSV/TSV) are not layout conversions: the owning package's writer decides
  what it can represent. XML only accepts the ordered XML document shape, TTML
  has no writer, and spreadsheet text output writes the first worksheet.
- The file API reads at most 512 MiB per input, never overwrites an existing
  output path, and returns `QW_STATUS_ERR_IO` for filesystem failures.
- The PDF reader accepts at most 100,000 pages and 64 MiB per stream; text
  extraction is capped at 256 MiB.
- Rendering accepts 1-1200 dpi (0 means 72) and RGB24 output up to 100,000
  pixels per side. Unsupported drawing operators are noted in the report; a
  successful raster may therefore be partial.
- PDF merging rejects encrypted inputs, caps the result at 100,000 pages, and
  writes a classic cross-reference table.
- Deliberate boundaries: no PDF/A creation or verification, no OCR, no
  embedded-font or CJK outline rendering, no HTML/Markdown output, and XPS is
  available only through the reader API, not the conversion dispatch.

## Testing

`ctest` runs 41 tests. They cover PDF xref tables/streams, object streams,
incremental documents, stream lengths, page order, RC4 and AES decryption,
ToUnicode extraction, page rendering (paths, images, CCITT G4, JPEG 2000 and
JBIG2), merging (classic, xref-stream, object-stream, incremental, metadata,
encrypted-input rejection), the public PDF API and PDF convert routes.

Office and e-book coverage includes ODT and EPUB to PDF, HTML to PDF, FB2/CSV
layout, CAJ JP2, format registry validation, file conversion, dispatch
refusals, and fuzz smoke runs for PDF and document formats. The PDF corpus test
skips itself when `app/testfile/pdf` is not present.

## Documentation

See [docs/usage.md](docs/usage.md) for build integration, task recipes, error
handling, ownership, threading and resource limits.

## License

MIT. See [LICENSE](LICENSE).
