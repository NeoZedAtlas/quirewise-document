cmake_minimum_required(VERSION 3.21)

project(QuirewiseDocument VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET Quirewise::Runtime)
    find_package(QuirewiseRuntime CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Archive)
    find_package(QuirewiseArchive CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Data)
    find_package(QuirewiseData CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Compression)
    find_package(QuirewiseCompression CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Ebook)
    find_package(QuirewiseEbook CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Image)
    find_package(QuirewiseImage CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Crypto)
    find_package(QuirewiseCrypto CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Spreadsheet)
    find_package(QuirewiseSpreadsheet CONFIG REQUIRED)
find_package(QuirewiseText CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Subtitle)
    find_package(QuirewiseSubtitle CONFIG REQUIRED)
endif()
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_document ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Document ALIAS quirewise_document)
set_target_properties(quirewise_document PROPERTIES EXPORT_NAME Document POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_document PUBLIC c_std_11)
target_compile_definitions(quirewise_document PRIVATE
    QUIREWISE_STANDALONE_DOCX_PARSER=1)
target_include_directories(quirewise_document PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
target_link_libraries(quirewise_document PRIVATE Quirewise::Archive Quirewise::Data Quirewise::Compression Quirewise::Ebook Quirewise::Image Quirewise::Crypto Quirewise::Spreadsheet Quirewise::Subtitle Quirewise::Text)
install(TARGETS quirewise_document EXPORT QuirewiseDocumentTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseDocumentTargets FILE QuirewiseDocumentTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseDocument")
configure_package_config_file(cmake/QuirewiseDocumentConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseDocument")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseDocumentConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseDocument")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_document
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(quirewise_document_pdf_jbig2_symbol_render_test
        tests/pdf_jbig2_symbol_render_test.c)
    target_compile_features(quirewise_document_pdf_jbig2_symbol_render_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_jbig2_symbol_render_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_jbig2_symbol_render
        COMMAND quirewise_document_pdf_jbig2_symbol_render_test)
    add_executable(quirewise_document_pdf_jbig2_arith_render_test
        tests/pdf_jbig2_arith_render_test.c)
    target_compile_features(quirewise_document_pdf_jbig2_arith_render_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_jbig2_arith_render_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_jbig2_arith_render
        COMMAND quirewise_document_pdf_jbig2_arith_render_test)
    add_executable(quirewise_document_pdf_jbig2_render_test
        tests/pdf_jbig2_render_test.c)
    target_compile_features(quirewise_document_pdf_jbig2_render_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_jbig2_render_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_jbig2_render
        COMMAND quirewise_document_pdf_jbig2_render_test)
    add_executable(quirewise_document_pdf_jpx_render_test
        tests/pdf_jpx_render_test.c)
    target_compile_features(quirewise_document_pdf_jpx_render_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_jpx_render_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_jpx_render
        COMMAND quirewise_document_pdf_jpx_render_test)
    add_executable(quirewise_document_pdf_ccitt_render_test
        tests/pdf_ccitt_render_test.c)
    target_compile_features(quirewise_document_pdf_ccitt_render_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_ccitt_render_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_ccitt_render
        COMMAND quirewise_document_pdf_ccitt_render_test)
    add_executable(quirewise_document_html_pdf_test tests/html_pdf_writer_test.c)
    target_compile_features(quirewise_document_html_pdf_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_html_pdf_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_html_pdf_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_html_pdf
        COMMAND quirewise_document_html_pdf_test)
    add_executable(quirewise_document_pdf_font_embed_test
        tests/pdf_font_embed_test.c)
    target_compile_features(quirewise_document_pdf_font_embed_test PRIVATE c_std_11)
    target_compile_definitions(quirewise_document_pdf_font_embed_test PRIVATE
        QUIREWISE_FONT_TEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/FileeTest-TrueType.ttf"
        QUIREWISE_OTF_TEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/FileeTest-CID.otf")
    target_include_directories(quirewise_document_pdf_font_embed_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_font_embed_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_font_embed
        COMMAND quirewise_document_pdf_font_embed_test)

    add_executable(quirewise_document_pdf_office_reflow_test
        tests/pdf_office_reflow_test.c)
    target_compile_features(quirewise_document_pdf_office_reflow_test PRIVATE c_std_11)
    target_compile_definitions(quirewise_document_pdf_office_reflow_test PRIVATE
        QUIREWISE_INDENTED_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/indented.pdf"
        QUIREWISE_STYLES_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/styles.pdf"
        QUIREWISE_INLINE_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/inline.pdf"
        QUIREWISE_SPACING_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/spacing.pdf"
        QUIREWISE_SCRIPT_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/script.pdf"
        QUIREWISE_IMAGE_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.pdf"
        QUIREWISE_TABLE_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/table.pdf"
        QUIREWISE_CMYK_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cmyk.pdf")
    target_link_libraries(quirewise_document_pdf_office_reflow_test PRIVATE
        quirewise_document Quirewise::Image)
    add_test(NAME quirewise_document_pdf_office_reflow
        COMMAND quirewise_document_pdf_office_reflow_test)

    add_executable(quirewise_document_odt_pdf_test tests/odt_pdf_end_to_end_test.c)
    target_compile_features(quirewise_document_odt_pdf_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_odt_pdf_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_link_libraries(quirewise_document_odt_pdf_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_odt_pdf
        COMMAND quirewise_document_odt_pdf_test)

    add_executable(quirewise_document_epub_pdf_test
        tests/epub_pdf_end_to_end_test.c)
    target_compile_features(quirewise_document_epub_pdf_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_epub_pdf_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_epub_pdf_test PRIVATE
        QUIREWISE_EPUB_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/reflow.epub"
        QUIREWISE_EPUB_CLASSY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/reflow-classy.epub"
        QUIREWISE_EPUB_UNSUPPORTED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/unsupported-layout.epub")
    target_link_libraries(quirewise_document_epub_pdf_test PRIVATE quirewise_document)
    add_executable(quirewise_document_raster_page_test
        tests/raster_page_test.c)
    target_compile_features(quirewise_document_raster_page_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_raster_page_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_raster_page_test PRIVATE quirewise_document)
    add_executable(quirewise_document_raster_content_test
        tests/raster_content_test.c)
    target_compile_features(quirewise_document_raster_content_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_raster_content_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_raster_content_test PRIVATE quirewise_document)
    add_executable(quirewise_document_raster_image_test
        tests/raster_image_test.c)
    target_compile_features(quirewise_document_raster_image_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_raster_image_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_raster_image_test PRIVATE
        QUIREWISE_JPEG_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../external-consumer-template/fixtures/tiny.jpg")
    target_link_libraries(quirewise_document_raster_image_test PRIVATE quirewise_document)
    add_executable(quirewise_document_raster_path_test
        tests/raster_path_test.c)
    target_compile_features(quirewise_document_raster_path_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_raster_path_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_raster_path_test PRIVATE quirewise_document)
    add_executable(quirewise_document_encrypted_pdf_test
        tests/encrypted_pdf_test.c)
    target_compile_features(quirewise_document_encrypted_pdf_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_encrypted_pdf_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_encrypted_pdf_test PRIVATE
        QUIREWISE_ENCRYPTED_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-rc4.pdf")
    target_link_libraries(quirewise_document_encrypted_pdf_test PRIVATE quirewise_document)
    add_executable(quirewise_document_merge_metadata_test
        tests/merge_metadata_test.c)
    target_compile_features(quirewise_document_merge_metadata_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_merge_metadata_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_merge_metadata_test PRIVATE quirewise_document)
    add_executable(quirewise_document_merge_xref_stream_test
        tests/merge_xref_stream_test.c)
    target_compile_features(quirewise_document_merge_xref_stream_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_merge_xref_stream_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_merge_xref_stream_test PRIVATE quirewise_document)
    add_executable(quirewise_document_merge_objstm_test
        tests/merge_objstm_test.c)
    target_compile_features(quirewise_document_merge_objstm_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_merge_objstm_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_merge_objstm_test PRIVATE quirewise_document)
    add_executable(quirewise_document_fb2_csv_layout_test
        tests/fb2_csv_layout_test.c)
    target_compile_features(quirewise_document_fb2_csv_layout_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_fb2_csv_layout_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_fb2_csv_layout_test PRIVATE quirewise_document)
    add_executable(quirewise_document_caj_jp2_test tests/caj_jp2_test.c)
    target_compile_features(quirewise_document_caj_jp2_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_caj_jp2_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_caj_jp2_test PRIVATE
        QUIREWISE_JP2_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../quirewise-image/tests/data/jp2/gray53_13x17.jp2")
    target_link_libraries(quirewise_document_caj_jp2_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_caj_jp2
        COMMAND quirewise_document_caj_jp2_test)

    add_test(NAME quirewise_document_fb2_csv_layout
        COMMAND quirewise_document_fb2_csv_layout_test)

    add_test(NAME quirewise_document_merge_objstm
        COMMAND quirewise_document_merge_objstm_test)

    add_test(NAME quirewise_document_merge_xref_stream
        COMMAND quirewise_document_merge_xref_stream_test)

    add_test(NAME quirewise_document_merge_metadata
        COMMAND quirewise_document_merge_metadata_test)

    add_test(NAME quirewise_document_encrypted_pdf
        COMMAND quirewise_document_encrypted_pdf_test)

    add_test(NAME quirewise_document_raster_path
        COMMAND quirewise_document_raster_path_test)

    add_test(NAME quirewise_document_raster_image
        COMMAND quirewise_document_raster_image_test)

    add_test(NAME quirewise_document_raster_content
        COMMAND quirewise_document_raster_content_test)

    add_test(NAME quirewise_document_raster_page
        COMMAND quirewise_document_raster_page_test)

    add_test(NAME quirewise_document_epub_pdf
        COMMAND quirewise_document_epub_pdf_test)

    add_executable(quirewise_document_pdf_reader_test tests/pdf_reader_test.c)
    target_compile_features(quirewise_document_pdf_reader_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_reader_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_reader_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_pdf_reader
        COMMAND quirewise_document_pdf_reader_test
            "${CMAKE_CURRENT_SOURCE_DIR}/../../external-consumer-template/fixtures/one-page.pdf")

    add_executable(quirewise_document_pdf_fuzz_smoke tests/pdf_fuzz_smoke.c)
    target_compile_features(quirewise_document_pdf_fuzz_smoke PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_fuzz_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_fuzz_smoke PRIVATE quirewise_document)
    add_test(NAME quirewise_document_pdf_fuzz
        COMMAND quirewise_document_pdf_fuzz_smoke 300
            "${CMAKE_CURRENT_SOURCE_DIR}/../../external-consumer-template/fixtures/one-page.pdf")

    add_executable(quirewise_document_convert_dispatch_test
        tests/convert_dispatch_test.c)
    target_compile_features(quirewise_document_convert_dispatch_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_convert_dispatch_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_convert_dispatch_test PRIVATE
        quirewise_document)
    add_executable(quirewise_document_format_fuzz_smoke
        tests/document_format_fuzz_smoke.c)
    target_compile_features(quirewise_document_format_fuzz_smoke PRIVATE c_std_11)
    target_link_libraries(quirewise_document_format_fuzz_smoke PRIVATE
        quirewise_document
        Quirewise::Data)
    add_test(NAME quirewise_document_format_fuzz
        COMMAND quirewise_document_format_fuzz_smoke)

    add_test(NAME quirewise_document_convert_dispatch
        COMMAND quirewise_document_convert_dispatch_test)

    add_executable(quirewise_document_convert_file_test
        tests/convert_file_test.c)
    target_compile_features(quirewise_document_convert_file_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_convert_file_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_convert_file
        COMMAND quirewise_document_convert_file_test)

    add_executable(quirewise_document_convert_family_test
        tests/convert_family_test.c)
    target_compile_features(quirewise_document_convert_family_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_convert_family_test PRIVATE
        quirewise_document Quirewise::Ebook)
    add_test(NAME quirewise_document_convert_family
        COMMAND quirewise_document_convert_family_test)

    add_executable(quirewise_document_pdf_office_image_test
        tests/pdf_office_image_test.c)
    target_compile_features(quirewise_document_pdf_office_image_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_office_image_test PRIVATE
        quirewise_document Quirewise::Ebook)
    add_test(NAME quirewise_document_pdf_office_image
        COMMAND quirewise_document_pdf_office_image_test)

    add_executable(quirewise_document_format_registry_test
        tests/format_registry_test.c)
    target_compile_features(quirewise_document_format_registry_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_format_registry_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
    target_link_libraries(quirewise_document_format_registry_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_format_registry
        COMMAND quirewise_document_format_registry_test)

    add_executable(quirewise_document_pdf_merge_test tests/pdf_merge_test.c)
    target_compile_features(quirewise_document_pdf_merge_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_merge_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_merge_test PRIVATE quirewise_document)
    add_test(NAME quirewise_document_pdf_merge
        COMMAND quirewise_document_pdf_merge_test)

    add_executable(quirewise_document_pdf_objstm_reader_test
        tests/pdf_objstm_reader_test.c)
    target_compile_features(quirewise_document_pdf_objstm_reader_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_objstm_reader_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_objstm_reader_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_objstm_reader
        COMMAND quirewise_document_pdf_objstm_reader_test)

    add_executable(quirewise_document_pdf_reader_r3_test
        tests/pdf_reader_r3_test.c)
    target_compile_features(quirewise_document_pdf_reader_r3_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_reader_r3_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_pdf_reader_r3_test PRIVATE
        QUIREWISE_R3_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-r3.pdf"
        QUIREWISE_R3_OBJSTM_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-r3-objstm.pdf")
    target_link_libraries(quirewise_document_pdf_reader_r3_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_reader_r3
        COMMAND quirewise_document_pdf_reader_r3_test)

    add_executable(quirewise_document_pdf_reader_aes_test
        tests/pdf_reader_aes_test.c)
    target_compile_features(quirewise_document_pdf_reader_aes_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_reader_aes_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_pdf_reader_aes_test PRIVATE
        QUIREWISE_AES_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-aes.pdf"
        QUIREWISE_AES_OBJSTM_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-aes-objstm.pdf")
    target_link_libraries(quirewise_document_pdf_reader_aes_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_reader_aes
        COMMAND quirewise_document_pdf_reader_aes_test)

    add_executable(quirewise_document_image_edit_test
    tests/image_edit_test.c)
target_compile_features(quirewise_document_image_edit_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_image_edit_test PRIVATE
    QUIREWISE_IMAGE_EDIT_SRC="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vml-red.png")
target_link_libraries(quirewise_document_image_edit_test PRIVATE
    quirewise_document Quirewise::Image)
add_test(NAME quirewise_document_image_edit
    COMMAND quirewise_document_image_edit_test)

add_executable(quirewise_document_pdf_merge_outline_test
    tests/pdf_merge_outline_test.c)
target_compile_features(quirewise_document_pdf_merge_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pdf_merge_outline_test PRIVATE
    QUIREWISE_MERGE_PDF_A="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/info.pdf"
    QUIREWISE_MERGE_PDF_B="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/outline.pdf")
target_link_libraries(quirewise_document_pdf_merge_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_merge_outline
    COMMAND quirewise_document_pdf_merge_outline_test)

add_executable(quirewise_document_pdf_outline_test
    tests/pdf_outline_test.c)
target_compile_features(quirewise_document_pdf_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pdf_outline_test PRIVATE
    QUIREWISE_OUTLINE_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/outline.pdf")
target_link_libraries(quirewise_document_pdf_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_outline
    COMMAND quirewise_document_pdf_outline_test)

add_executable(quirewise_document_msg_rtf_image_test
    tests/msg_rtf_image_test.c)
target_compile_features(quirewise_document_msg_rtf_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_msg_rtf_image_test PRIVATE
    QUIREWISE_RTF_IMAGE_MSG="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rtf-image.msg")
target_include_directories(quirewise_document_msg_rtf_image_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_msg_rtf_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_msg_rtf_image
    COMMAND quirewise_document_msg_rtf_image_test)

add_executable(quirewise_document_msg_rtf_table_test
    tests/msg_rtf_table_test.c)
target_compile_features(quirewise_document_msg_rtf_table_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_msg_rtf_table_test PRIVATE
    QUIREWISE_RTF_TABLE_MSG="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rtf-table.msg")
target_include_directories(quirewise_document_msg_rtf_table_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_msg_rtf_table_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_msg_rtf_table
    COMMAND quirewise_document_msg_rtf_table_test)

add_executable(quirewise_document_msg_rtf_test
    tests/msg_rtf_test.c)
target_compile_features(quirewise_document_msg_rtf_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_msg_rtf_test PRIVATE
    QUIREWISE_RTF_MSG="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rtf-sample.msg")
target_include_directories(quirewise_document_msg_rtf_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_msg_rtf_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_msg_rtf
    COMMAND quirewise_document_msg_rtf_test)

add_executable(quirewise_document_xps_html_test
    tests/xps_html_test.c)
target_compile_features(quirewise_document_xps_html_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_xps_html_test PRIVATE
    QUIREWISE_XPS_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.xps"
    QUIREWISE_XPS_IMAGE_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.xps")
target_include_directories(quirewise_document_xps_html_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_xps_html_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_xps_html
    COMMAND quirewise_document_xps_html_test)

add_executable(quirewise_document_epub_footnote_test
    tests/epub_footnote_test.c)
target_compile_features(quirewise_document_epub_footnote_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_footnote_test PRIVATE
    QUIREWISE_FOOTNOTE_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/footnote.epub")
target_include_directories(quirewise_document_epub_footnote_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_footnote_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_footnote
    COMMAND quirewise_document_epub_footnote_test)

add_executable(quirewise_document_epub_hr_test
    tests/epub_hr_test.c)
target_compile_features(quirewise_document_epub_hr_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_hr_test PRIVATE
    QUIREWISE_HR_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/hr.epub")
target_include_directories(quirewise_document_epub_hr_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_hr_test PRIVATE quirewise_document)
add_test(NAME quirewise_document_epub_hr
    COMMAND quirewise_document_epub_hr_test)

add_executable(quirewise_document_epub_figure_test
    tests/epub_figure_test.c)
target_compile_features(quirewise_document_epub_figure_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_figure_test PRIVATE
    QUIREWISE_FIGURE_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/figure.epub")
target_include_directories(quirewise_document_epub_figure_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_figure_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_figure
    COMMAND quirewise_document_epub_figure_test)

add_executable(quirewise_document_pptx_pdf_outline_test
    tests/pptx_pdf_outline_test.c)
target_compile_features(quirewise_document_pptx_pdf_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_pdf_outline_test PRIVATE
    QUIREWISE_OUTLINE_PPTX="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/outline-slide.pptx")
target_include_directories(quirewise_document_pptx_pdf_outline_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_pptx_pdf_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_pdf_outline
    COMMAND quirewise_document_pptx_pdf_outline_test)

add_executable(quirewise_document_odp_pdf_outline_test
    tests/odp_pdf_outline_test.c)
target_compile_features(quirewise_document_odp_pdf_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_pdf_outline_test PRIVATE
    QUIREWISE_OUTLINE_ODP="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/outline-slide.odp")
target_include_directories(quirewise_document_odp_pdf_outline_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odp_pdf_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_pdf_outline
    COMMAND quirewise_document_odp_pdf_outline_test)

add_executable(quirewise_document_odt_pdf_outline_test
    tests/odt_pdf_outline_test.c)
target_compile_features(quirewise_document_odt_pdf_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odt_pdf_outline_test PRIVATE
    QUIREWISE_OUTLINE_ODT="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/outline.odt")
target_include_directories(quirewise_document_odt_pdf_outline_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odt_pdf_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odt_pdf_outline
    COMMAND quirewise_document_odt_pdf_outline_test)

add_executable(quirewise_document_docx_pdf_outline_test
    tests/docx_pdf_outline_test.c)
target_compile_features(quirewise_document_docx_pdf_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_pdf_outline_test PRIVATE
    QUIREWISE_OUTLINE_DOCX="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/outline.docx")
target_include_directories(quirewise_document_docx_pdf_outline_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_pdf_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_pdf_outline
    COMMAND quirewise_document_docx_pdf_outline_test)

add_executable(quirewise_document_pdf_metadata_test
    tests/pdf_metadata_test.c)
target_compile_features(quirewise_document_pdf_metadata_test PRIVATE c_std_11)
target_include_directories(quirewise_document_pdf_metadata_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_pdf_metadata_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_metadata
    COMMAND quirewise_document_pdf_metadata_test)

add_executable(quirewise_document_pdf_outline_zh_test
    tests/pdf_outline_zh_test.c)
target_compile_features(quirewise_document_pdf_outline_zh_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pdf_outline_zh_test PRIVATE
    QUIREWISE_HEADINGS_ZH_HWPX="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/headings-zh.hwpx")
target_include_directories(quirewise_document_pdf_outline_zh_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_pdf_outline_zh_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_outline_zh
    COMMAND quirewise_document_pdf_outline_zh_test)

add_executable(quirewise_document_pdf_heading_outline_test
    tests/pdf_heading_outline_test.c)
target_compile_features(quirewise_document_pdf_heading_outline_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pdf_heading_outline_test PRIVATE
    QUIREWISE_HEADINGS_HWPX="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/headings.hwpx")
target_include_directories(quirewise_document_pdf_heading_outline_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_pdf_heading_outline_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_heading_outline
    COMMAND quirewise_document_pdf_heading_outline_test)

add_executable(quirewise_document_hwpx_footnotes_test
    tests/hwpx_footnotes_test.c)
target_compile_features(quirewise_document_hwpx_footnotes_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_footnotes_test PRIVATE
    QUIREWISE_HWPX_FOOTNOTES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/footnotes.hwpx")
target_link_libraries(quirewise_document_hwpx_footnotes_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_footnotes
    COMMAND quirewise_document_hwpx_footnotes_test)

add_executable(quirewise_document_hwpx_header_footer_test
    tests/hwpx_header_footer_test.c)
target_compile_features(quirewise_document_hwpx_header_footer_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_header_footer_test PRIVATE
    QUIREWISE_HWPX_HEADER_FOOTER="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/header-footer.hwpx")
target_link_libraries(quirewise_document_hwpx_header_footer_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_header_footer
    COMMAND quirewise_document_hwpx_header_footer_test)

add_executable(quirewise_document_hwpx_sections_test
    tests/hwpx_sections_test.c)
target_compile_features(quirewise_document_hwpx_sections_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_sections_test PRIVATE
    QUIREWISE_HWPX_SECTIONS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/two-sections.hwpx")
target_link_libraries(quirewise_document_hwpx_sections_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_sections
    COMMAND quirewise_document_hwpx_sections_test)

add_executable(quirewise_document_hwpx_hyperlink_test
    tests/hwpx_hyperlink_test.c)
target_compile_features(quirewise_document_hwpx_hyperlink_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_hyperlink_test PRIVATE
    QUIREWISE_HWPX_HYPERLINK="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/hyperlink.hwpx")
target_link_libraries(quirewise_document_hwpx_hyperlink_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_hyperlink
    COMMAND quirewise_document_hwpx_hyperlink_test)

add_executable(quirewise_document_hwpx_margins_test
    tests/hwpx_margins_test.c)
target_compile_features(quirewise_document_hwpx_margins_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_margins_test PRIVATE
    QUIREWISE_HWPX_MARGINS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/page-margins.hwpx")
target_link_libraries(quirewise_document_hwpx_margins_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_margins
    COMMAND quirewise_document_hwpx_margins_test)

add_executable(quirewise_document_hwpx_columns_test
    tests/hwpx_columns_test.c)
target_compile_features(quirewise_document_hwpx_columns_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_columns_test PRIVATE
    QUIREWISE_HWPX_COLUMNS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/columns.hwpx")
target_link_libraries(quirewise_document_hwpx_columns_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_columns
    COMMAND quirewise_document_hwpx_columns_test)

add_executable(quirewise_document_hwpx_cell_margin_test
    tests/hwpx_cell_margin_test.c)
target_compile_features(quirewise_document_hwpx_cell_margin_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_cell_margin_test PRIVATE
    QUIREWISE_HWPX_CELL_MARGIN="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cell-margin.hwpx")
target_link_libraries(quirewise_document_hwpx_cell_margin_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_cell_margin
    COMMAND quirewise_document_hwpx_cell_margin_test)

add_executable(quirewise_document_hwpx_cell_align_test
    tests/hwpx_cell_align_test.c)
target_compile_features(quirewise_document_hwpx_cell_align_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_cell_align_test PRIVATE
    QUIREWISE_HWPX_CELL_ALIGN="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cell-align.hwpx")
target_link_libraries(quirewise_document_hwpx_cell_align_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_cell_align
    COMMAND quirewise_document_hwpx_cell_align_test)

add_executable(quirewise_document_hwpx_cell_border_test
    tests/hwpx_cell_border_test.c)
target_compile_features(quirewise_document_hwpx_cell_border_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_cell_border_test PRIVATE
    QUIREWISE_HWPX_CELL_BORDER="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cell-border.hwpx")
target_link_libraries(quirewise_document_hwpx_cell_border_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_cell_border
    COMMAND quirewise_document_hwpx_cell_border_test)

add_executable(quirewise_document_hwpx_cell_background_test
    tests/hwpx_cell_background_test.c)
target_compile_features(quirewise_document_hwpx_cell_background_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_cell_background_test PRIVATE
    QUIREWISE_HWPX_CELL_BACKGROUND="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cell-background.hwpx")
target_link_libraries(quirewise_document_hwpx_cell_background_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_cell_background
    COMMAND quirewise_document_hwpx_cell_background_test)

add_executable(quirewise_document_hwpx_page_break_test
    tests/hwpx_page_break_test.c)
target_compile_features(quirewise_document_hwpx_page_break_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_page_break_test PRIVATE
    QUIREWISE_HWPX_PAGE_BREAK="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/page-break.hwpx")
target_include_directories(quirewise_document_hwpx_page_break_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_hwpx_page_break_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_page_break
    COMMAND quirewise_document_hwpx_page_break_test)

add_executable(quirewise_document_hwpx_headings_test
    tests/hwpx_headings_test.c)
target_compile_features(quirewise_document_hwpx_headings_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_headings_test PRIVATE
    QUIREWISE_HWPX_HEADINGS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/headings.hwpx")
target_link_libraries(quirewise_document_hwpx_headings_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_headings
    COMMAND quirewise_document_hwpx_headings_test)

add_executable(quirewise_document_hwpx_para_style_test
    tests/hwpx_para_style_test.c)
target_compile_features(quirewise_document_hwpx_para_style_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_para_style_test PRIVATE
    QUIREWISE_HWPX_PARA_STYLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/paragraph-style.hwpx")
target_link_libraries(quirewise_document_hwpx_para_style_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_para_style
    COMMAND quirewise_document_hwpx_para_style_test)

add_executable(quirewise_document_hwpx_styles_test
    tests/hwpx_styles_test.c)
target_compile_features(quirewise_document_hwpx_styles_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_styles_test PRIVATE
    QUIREWISE_HWPX_STYLED="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/styled.hwpx")
target_link_libraries(quirewise_document_hwpx_styles_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_styles
    COMMAND quirewise_document_hwpx_styles_test)

add_executable(quirewise_document_hwpx_position_test
    tests/hwpx_position_test.c)
target_compile_features(quirewise_document_hwpx_position_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_position_test PRIVATE
    QUIREWISE_HWPX_POSITION="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/positioned-image.hwpx")
target_link_libraries(quirewise_document_hwpx_position_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_position
    COMMAND quirewise_document_hwpx_position_test)

add_executable(quirewise_document_hwpx_align_test
    tests/hwpx_align_test.c)
target_compile_features(quirewise_document_hwpx_align_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_align_test PRIVATE
    QUIREWISE_HWPX_ALIGN="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/align.hwpx")
target_link_libraries(quirewise_document_hwpx_align_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_align
    COMMAND quirewise_document_hwpx_align_test)

add_executable(quirewise_document_hwpx_flip_test
    tests/hwpx_flip_test.c)
target_compile_features(quirewise_document_hwpx_flip_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_flip_test PRIVATE
    QUIREWISE_HWPX_FLIPPED="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/flipped-image.hwpx")
target_link_libraries(quirewise_document_hwpx_flip_test PRIVATE
    quirewise_document Quirewise::Image)
add_test(NAME quirewise_document_hwpx_flip
    COMMAND quirewise_document_hwpx_flip_test)

add_executable(quirewise_document_hwpx_table_spans_test
    tests/hwpx_table_spans_test.c)
target_compile_features(quirewise_document_hwpx_table_spans_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_table_spans_test PRIVATE
    QUIREWISE_HWPX_TABLE_SPANS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/table-spans.hwpx")
target_link_libraries(quirewise_document_hwpx_table_spans_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_table_spans
    COMMAND quirewise_document_hwpx_table_spans_test)

add_executable(quirewise_document_hwpx_rotation_test
    tests/hwpx_rotation_test.c)
target_compile_features(quirewise_document_hwpx_rotation_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_rotation_test PRIVATE
    QUIREWISE_HWPX_ROTATED="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rotated-image.hwpx")
target_link_libraries(quirewise_document_hwpx_rotation_test PRIVATE quirewise_document)
add_test(NAME quirewise_document_hwpx_rotation
    COMMAND quirewise_document_hwpx_rotation_test)

add_executable(quirewise_document_hwpx_crop_test
    tests/hwpx_crop_test.c)
target_compile_features(quirewise_document_hwpx_crop_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_crop_test PRIVATE
    QUIREWISE_HWPX_CROP="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/crop-image.hwpx")
target_link_libraries(quirewise_document_hwpx_crop_test PRIVATE quirewise_document)
add_test(NAME quirewise_document_hwpx_crop
    COMMAND quirewise_document_hwpx_crop_test)

add_executable(quirewise_document_epub_table_caption_test
    tests/epub_table_caption_test.c)
target_compile_features(quirewise_document_epub_table_caption_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_table_caption_test PRIVATE
    QUIREWISE_TABLE_CAPTION_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/table-caption.epub")
target_include_directories(quirewise_document_epub_table_caption_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_table_caption_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_table_caption
    COMMAND quirewise_document_epub_table_caption_test)

add_executable(quirewise_document_epub_definition_list_test
    tests/epub_definition_list_test.c)
target_compile_features(quirewise_document_epub_definition_list_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_definition_list_test PRIVATE
    QUIREWISE_DEFINITION_LIST_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/definition-list.epub")
target_include_directories(quirewise_document_epub_definition_list_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_definition_list_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_definition_list
    COMMAND quirewise_document_epub_definition_list_test)

add_executable(quirewise_document_epub_blockquote_test
    tests/epub_blockquote_test.c)
target_compile_features(quirewise_document_epub_blockquote_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_blockquote_test PRIVATE
    QUIREWISE_BLOCKQUOTE_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/blockquote.epub")
target_include_directories(quirewise_document_epub_blockquote_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_blockquote_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_blockquote
    COMMAND quirewise_document_epub_blockquote_test)

add_executable(quirewise_document_htmlz_image_test
    tests/htmlz_image_test.c)
target_compile_features(quirewise_document_htmlz_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_htmlz_image_test PRIVATE
    QUIREWISE_HTMLZ_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.htmlz")
target_include_directories(quirewise_document_htmlz_image_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_htmlz_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_htmlz_image
    COMMAND quirewise_document_htmlz_image_test)

add_executable(quirewise_document_epub_ordered_list_test
    tests/epub_ordered_list_test.c)
target_compile_features(quirewise_document_epub_ordered_list_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_ordered_list_test PRIVATE
    QUIREWISE_ORDERED_LIST_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ordered-list.epub")
target_include_directories(quirewise_document_epub_ordered_list_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_ordered_list_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_ordered_list
    COMMAND quirewise_document_epub_ordered_list_test)

add_executable(quirewise_document_epub_table_spans_test
    tests/epub_table_spans_test.c)
target_compile_features(quirewise_document_epub_table_spans_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_table_spans_test PRIVATE
    QUIREWISE_TABLE_SPANS_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/table-spans.epub")
target_include_directories(quirewise_document_epub_table_spans_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_table_spans_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_table_spans
    COMMAND quirewise_document_epub_table_spans_test)

add_executable(quirewise_document_epub_nested_list_test
    tests/epub_nested_list_test.c)
target_compile_features(quirewise_document_epub_nested_list_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_nested_list_test PRIVATE
    QUIREWISE_NESTED_LIST_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/nested-list.epub")
target_include_directories(quirewise_document_epub_nested_list_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_nested_list_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_nested_list
    COMMAND quirewise_document_epub_nested_list_test)

add_executable(quirewise_document_epub_formatting_test
    tests/epub_formatting_test.c)
target_compile_features(quirewise_document_epub_formatting_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_formatting_test PRIVATE
    QUIREWISE_FORMATTING_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/formatting.epub")
target_include_directories(quirewise_document_epub_formatting_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_formatting_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_formatting
    COMMAND quirewise_document_epub_formatting_test)

add_executable(quirewise_document_odp_crop_test
    tests/odp_crop_test.c)
target_compile_features(quirewise_document_odp_crop_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_crop_test PRIVATE
    QUIREWISE_CROP_ODP="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/crop-slide.odp")
target_include_directories(quirewise_document_odp_crop_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odp_crop_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_crop
    COMMAND quirewise_document_odp_crop_test)

add_executable(quirewise_document_pptx_crop_test
    tests/pptx_crop_test.c)
target_compile_features(quirewise_document_pptx_crop_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_crop_test PRIVATE
    QUIREWISE_CROP_PPTX="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/crop-slide.pptx")
target_include_directories(quirewise_document_pptx_crop_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_pptx_crop_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_crop
    COMMAND quirewise_document_pptx_crop_test)

add_executable(quirewise_document_pdf_info_test
    tests/pdf_info_test.c)
target_compile_features(quirewise_document_pdf_info_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pdf_info_test PRIVATE
    QUIREWISE_INFO_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/info.pdf")
target_link_libraries(quirewise_document_pdf_info_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_info
    COMMAND quirewise_document_pdf_info_test)

add_executable(quirewise_document_docx_rowspan_write_test
    tests/docx_rowspan_write_test.c)
target_compile_features(quirewise_document_docx_rowspan_write_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_rowspan_write_test PRIVATE
    QUIREWISE_ROWSPAN_HTML="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rowspan.html")
target_include_directories(quirewise_document_docx_rowspan_write_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_rowspan_write_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_rowspan_write
    COMMAND quirewise_document_docx_rowspan_write_test)

add_executable(quirewise_document_docx_table_write_test
    tests/docx_table_write_test.c)
target_compile_features(quirewise_document_docx_table_write_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_table_write_test PRIVATE
    QUIREWISE_TABLE_HTML="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/table.html")
target_include_directories(quirewise_document_docx_table_write_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_table_write_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_table_write
    COMMAND quirewise_document_docx_table_write_test)

add_executable(quirewise_document_docx_link_write_test
    tests/docx_link_write_test.c)
target_compile_features(quirewise_document_docx_link_write_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_link_write_test PRIVATE
    QUIREWISE_LINK_ODT_SRC="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/link.odt")
target_include_directories(quirewise_document_docx_link_write_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_link_write_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_link_write
    COMMAND quirewise_document_docx_link_write_test)

add_executable(quirewise_document_epub_image_write_test
    tests/epub_image_write_test.c)
target_compile_features(quirewise_document_epub_image_write_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_image_write_test PRIVATE
    QUIREWISE_EPUB_IMAGE_SRC="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.epub")
target_include_directories(quirewise_document_epub_image_write_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_epub_image_write_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_image_write
    COMMAND quirewise_document_epub_image_write_test)

add_executable(quirewise_document_docx_image_write_test
    tests/docx_image_write_test.c)
target_compile_features(quirewise_document_docx_image_write_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_image_write_test PRIVATE
    QUIREWISE_DOCX_IMAGE_EPUB="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.epub")
target_include_directories(quirewise_document_docx_image_write_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_image_write_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_image_write
    COMMAND quirewise_document_docx_image_write_test)

add_executable(quirewise_document_eml_rfc2231_test
    tests/eml_rfc2231_test.c)
target_compile_features(quirewise_document_eml_rfc2231_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_eml_rfc2231_test PRIVATE
    QUIREWISE_RFC2231_EML="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rfc2231.eml")
target_include_directories(quirewise_document_eml_rfc2231_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_eml_rfc2231_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_eml_rfc2231
    COMMAND quirewise_document_eml_rfc2231_test)

add_executable(quirewise_document_spreadsheet_merge_test
    tests/spreadsheet_merge_test.c)
target_compile_features(quirewise_document_spreadsheet_merge_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_spreadsheet_merge_test PRIVATE
    QUIREWISE_MERGED_XLSX="${CMAKE_CURRENT_SOURCE_DIR}/../quirewise-spreadsheet/tests/data/merged.xlsx"
    QUIREWISE_MERGED_FODS="${CMAKE_CURRENT_SOURCE_DIR}/../quirewise-spreadsheet/tests/data/merged.fods"
    QUIREWISE_MERGED_XLS="${CMAKE_CURRENT_SOURCE_DIR}/../quirewise-spreadsheet/tests/data/merged.xls")
target_include_directories(quirewise_document_spreadsheet_merge_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_spreadsheet_merge_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_spreadsheet_merge
    COMMAND quirewise_document_spreadsheet_merge_test)

add_executable(quirewise_document_odp_image_size_test
    tests/odp_image_size_test.c)
target_compile_features(quirewise_document_odp_image_size_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_image_size_test PRIVATE
    QUIREWISE_ODP_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image-slide.odp")
target_include_directories(quirewise_document_odp_image_size_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odp_image_size_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_image_size
    COMMAND quirewise_document_odp_image_size_test)

add_executable(quirewise_document_odt_image_size_test
    tests/odt_image_size_test.c)
target_compile_features(quirewise_document_odt_image_size_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odt_image_size_test PRIVATE
    QUIREWISE_SIZED_ODT="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sized-image.odt")
target_include_directories(quirewise_document_odt_image_size_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odt_image_size_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odt_image_size
    COMMAND quirewise_document_odt_image_size_test)

add_executable(quirewise_document_vml_image_test
    tests/vml_image_test.c)
target_compile_features(quirewise_document_vml_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_vml_image_test PRIVATE
    QUIREWISE_VML_DOCX="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vml-image.docx")
target_include_directories(quirewise_document_vml_image_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_vml_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_vml_image
    COMMAND quirewise_document_vml_image_test)

add_executable(quirewise_document_link_test
    tests/link_test.c)
target_compile_features(quirewise_document_link_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_link_test PRIVATE
    QUIREWISE_LINK_ODT="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/link.odt"
    QUIREWISE_LINK_ODP="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/link-slide.odp")
target_include_directories(quirewise_document_link_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_link_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_link
    COMMAND quirewise_document_link_test)

add_executable(quirewise_document_msg_test
    tests/msg_test.c)
target_compile_features(quirewise_document_msg_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_msg_test PRIVATE
    QUIREWISE_MSG_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.msg")
target_include_directories(quirewise_document_msg_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_msg_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_msg
    COMMAND quirewise_document_msg_test)

add_executable(quirewise_document_docx_merge_test
    tests/docx_merge_test.c)
target_compile_features(quirewise_document_docx_merge_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_merge_test PRIVATE
    QUIREWISE_DOCX_MERGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/vertical-merge.docx")
target_include_directories(quirewise_document_docx_merge_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_merge_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_merge
    COMMAND quirewise_document_docx_merge_test)

add_executable(quirewise_document_fodt_image_test
    tests/fodt_image_test.c)
target_compile_features(quirewise_document_fodt_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_fodt_image_test PRIVATE
    QUIREWISE_FODT_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image-data.fodt")
target_link_libraries(quirewise_document_fodt_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_fodt_image
    COMMAND quirewise_document_fodt_image_test)

add_executable(quirewise_document_odp_notes_test
    tests/odp_notes_test.c)
target_compile_features(quirewise_document_odp_notes_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_notes_test PRIVATE
    QUIREWISE_ODP_NOTES_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/notes-slide.odp")
target_link_libraries(quirewise_document_odp_notes_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_notes
    COMMAND quirewise_document_odp_notes_test)

add_executable(quirewise_document_markup_export_test
    tests/markup_export_test.c)
target_compile_features(quirewise_document_markup_export_test PRIVATE c_std_11)
target_link_libraries(quirewise_document_markup_export_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_markup_export
    COMMAND quirewise_document_markup_export_test)

add_executable(quirewise_document_epub_image_test
    tests/epub_image_test.c)
target_compile_features(quirewise_document_epub_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_epub_image_test PRIVATE
    QUIREWISE_EPUB_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.epub")
target_link_libraries(quirewise_document_epub_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_epub_image
    COMMAND quirewise_document_epub_image_test)

add_executable(quirewise_document_hwpx_image_test
    tests/hwpx_image_test.c)
target_compile_features(quirewise_document_hwpx_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_hwpx_image_test PRIVATE
    QUIREWISE_HWPX_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.hwpx")
target_link_libraries(quirewise_document_hwpx_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_hwpx_image
    COMMAND quirewise_document_hwpx_image_test)

add_executable(quirewise_document_xps_pdf_test
    tests/xps_pdf_test.c)
target_compile_features(quirewise_document_xps_pdf_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_xps_pdf_test PRIVATE
    QUIREWISE_XPS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.xps"
    QUIREWISE_XPS_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image.xps")
target_link_libraries(quirewise_document_xps_pdf_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_xps_pdf
    COMMAND quirewise_document_xps_pdf_test)

add_executable(quirewise_document_eml_attachment_test
    tests/eml_attachment_test.c)
target_compile_features(quirewise_document_eml_attachment_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_eml_attachment_test PRIVATE
    QUIREWISE_EML_ATTACHMENT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/attachments.eml")
target_link_libraries(quirewise_document_eml_attachment_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_eml_attachment
    COMMAND quirewise_document_eml_attachment_test)

add_executable(quirewise_document_odt_header_footer_test
    tests/odt_header_footer_test.c)
target_compile_features(quirewise_document_odt_header_footer_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odt_header_footer_test PRIVATE
    QUIREWISE_ODT_HF_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/header-footer.odt")
target_link_libraries(quirewise_document_odt_header_footer_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odt_header_footer
    COMMAND quirewise_document_odt_header_footer_test)

add_executable(quirewise_document_pptx_notes_test
    tests/pptx_notes_test.c)
target_compile_features(quirewise_document_pptx_notes_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_notes_test PRIVATE
    QUIREWISE_PPTX_NOTES_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/notes-slide.pptx")
target_link_libraries(quirewise_document_pptx_notes_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_notes
    COMMAND quirewise_document_pptx_notes_test)

add_executable(quirewise_document_docx_header_style_test
    tests/docx_header_style_test.c)
target_compile_features(quirewise_document_docx_header_style_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_header_style_test PRIVATE
    QUIREWISE_DOCX_STYLED_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/styled-header.docx")
target_include_directories(quirewise_document_docx_header_style_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_header_style_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_header_style
    COMMAND quirewise_document_docx_header_style_test)

add_executable(quirewise_document_odt_indent_test
    tests/odt_indent_test.c)
target_compile_features(quirewise_document_odt_indent_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odt_indent_test PRIVATE
    QUIREWISE_ODT_INDENT="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/indent.odt")
target_include_directories(quirewise_document_odt_indent_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odt_indent_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odt_indent
    COMMAND quirewise_document_odt_indent_test)

add_executable(quirewise_document_odt_endnote_test
    tests/odt_endnote_test.c)
target_compile_features(quirewise_document_odt_endnote_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odt_endnote_test PRIVATE
    QUIREWISE_ODT_ENDNOTE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/endnote.odt")
target_include_directories(quirewise_document_odt_endnote_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odt_endnote_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odt_endnote
    COMMAND quirewise_document_odt_endnote_test)

add_executable(quirewise_document_docx_endnotes_test
    tests/docx_endnotes_test.c)
target_compile_features(quirewise_document_docx_endnotes_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_endnotes_test PRIVATE
    QUIREWISE_DOCX_ENDNOTES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/endnotes.docx")
target_include_directories(quirewise_document_docx_endnotes_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_endnotes_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_endnotes
    COMMAND quirewise_document_docx_endnotes_test)

add_executable(quirewise_document_odt_footnote_test
    tests/odt_footnote_test.c)
target_compile_features(quirewise_document_odt_footnote_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odt_footnote_test PRIVATE
    QUIREWISE_ODT_FOOTNOTE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/footnote.odt")
target_include_directories(quirewise_document_odt_footnote_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_odt_footnote_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odt_footnote
    COMMAND quirewise_document_odt_footnote_test)

add_executable(quirewise_document_docx_footnotes_test
    tests/docx_footnotes_test.c)
target_compile_features(quirewise_document_docx_footnotes_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_footnotes_test PRIVATE
    QUIREWISE_DOCX_FOOTNOTES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/footnotes.docx")
target_include_directories(quirewise_document_docx_footnotes_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_footnotes_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_footnotes
    COMMAND quirewise_document_docx_footnotes_test)

add_executable(quirewise_document_docx_indent_test
    tests/docx_indent_test.c)
target_compile_features(quirewise_document_docx_indent_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_indent_test PRIVATE
    QUIREWISE_DOCX_INDENT="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/indent.docx")
target_include_directories(quirewise_document_docx_indent_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
target_link_libraries(quirewise_document_docx_indent_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_indent
    COMMAND quirewise_document_docx_indent_test)

add_executable(quirewise_document_docx_header_footer_test
    tests/docx_header_footer_test.c)
target_compile_features(quirewise_document_docx_header_footer_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_docx_header_footer_test PRIVATE
    QUIREWISE_DOCX_HF_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/header-footer.docx")
target_link_libraries(quirewise_document_docx_header_footer_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_docx_header_footer
    COMMAND quirewise_document_docx_header_footer_test)

add_executable(quirewise_document_odp_image_test
    tests/odp_image_test.c)
target_compile_features(quirewise_document_odp_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_image_test PRIVATE
    QUIREWISE_ODP_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image-slide.odp")
target_link_libraries(quirewise_document_odp_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_image
    COMMAND quirewise_document_odp_image_test)

add_executable(quirewise_document_odp_position_test
    tests/odp_position_test.c)
target_compile_features(quirewise_document_odp_position_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_position_test PRIVATE
    QUIREWISE_ODP_POSITION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/position-slide.odp")
target_link_libraries(quirewise_document_odp_position_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_position
    COMMAND quirewise_document_odp_position_test)

add_executable(quirewise_document_pptx_position_test
    tests/pptx_position_test.c)
target_compile_features(quirewise_document_pptx_position_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_position_test PRIVATE
    QUIREWISE_PPTX_POSITION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image-slide.pptx")
target_link_libraries(quirewise_document_pptx_position_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_position
    COMMAND quirewise_document_pptx_position_test)

add_executable(quirewise_document_pptx_text_rotation_test
    tests/pptx_text_rotation_test.c)
target_compile_features(quirewise_document_pptx_text_rotation_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_text_rotation_test PRIVATE
    QUIREWISE_PPTX_TEXT_ROTATION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rotated-text-slide.pptx")
target_link_libraries(quirewise_document_pptx_text_rotation_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_text_rotation
    COMMAND quirewise_document_pptx_text_rotation_test)

add_executable(quirewise_document_odp_text_rotation_test
    tests/odp_text_rotation_test.c)
target_compile_features(quirewise_document_odp_text_rotation_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_text_rotation_test PRIVATE
    QUIREWISE_ODP_TEXT_ROTATION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rotated-text.odp")
target_link_libraries(quirewise_document_odp_text_rotation_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_text_rotation
    COMMAND quirewise_document_odp_text_rotation_test)

add_executable(quirewise_document_odp_rotation_test
    tests/odp_rotation_test.c)
target_compile_features(quirewise_document_odp_rotation_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_odp_rotation_test PRIVATE
    QUIREWISE_ODP_ROTATION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rotated-slide.odp")
target_link_libraries(quirewise_document_odp_rotation_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_odp_rotation
    COMMAND quirewise_document_odp_rotation_test)

add_executable(quirewise_document_pptx_rotation_test
    tests/pptx_rotation_test.c)
target_compile_features(quirewise_document_pptx_rotation_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_rotation_test PRIVATE
    QUIREWISE_PPTX_ROTATION_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/rotated-slide.pptx")
target_link_libraries(quirewise_document_pptx_rotation_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_rotation
    COMMAND quirewise_document_pptx_rotation_test)

add_executable(quirewise_document_pptx_image_test
    tests/pptx_image_test.c)
target_compile_features(quirewise_document_pptx_image_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pptx_image_test PRIVATE
    QUIREWISE_PPTX_IMAGE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/image-slide.pptx")
target_link_libraries(quirewise_document_pptx_image_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pptx_image
    COMMAND quirewise_document_pptx_image_test)

add_executable(quirewise_document_pdf_password_test
    tests/pdf_password_test.c)
target_compile_features(quirewise_document_pdf_password_test PRIVATE c_std_11)
target_compile_definitions(quirewise_document_pdf_password_test PRIVATE
    QUIREWISE_USERPW_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-mutool-aes256-userpw.pdf")
target_link_libraries(quirewise_document_pdf_password_test PRIVATE
    quirewise_document)
add_test(NAME quirewise_document_pdf_password
    COMMAND quirewise_document_pdf_password_test)

add_executable(quirewise_document_pdf_reader_mutool_test
        tests/pdf_reader_mutool_encryption_test.c)
    target_compile_features(quirewise_document_pdf_reader_mutool_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_reader_mutool_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_pdf_reader_mutool_test PRIVATE
        QUIREWISE_MUTOOL_AES128_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-mutool-aes128.pdf"
        QUIREWISE_MUTOOL_AES256_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-mutool-aes256.pdf"
        QUIREWISE_MUTOOL_AES256_USERPW_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-mutool-aes256-userpw.pdf")
    target_link_libraries(quirewise_document_pdf_reader_mutool_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_reader_mutool
        COMMAND quirewise_document_pdf_reader_mutool_test)

    add_executable(quirewise_document_pdf_tounicode_test
        tests/pdf_tounicode_test.c)
    target_compile_features(quirewise_document_pdf_tounicode_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_tounicode_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_tounicode_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_tounicode
        COMMAND quirewise_document_pdf_tounicode_test)

    add_executable(quirewise_document_pdf_xref_advanced_test
        tests/pdf_xref_advanced_test.c)
    target_compile_features(quirewise_document_pdf_xref_advanced_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_xref_advanced_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_xref_advanced_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_xref_advanced
        COMMAND quirewise_document_pdf_xref_advanced_test)

    add_executable(quirewise_document_pdf_stream_length_test
        tests/pdf_stream_length_test.c)
    target_compile_features(quirewise_document_pdf_stream_length_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_stream_length_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_stream_length_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_stream_length
        COMMAND quirewise_document_pdf_stream_length_test)

    add_executable(quirewise_document_pdf_flate_image_test
        tests/pdf_flate_image_test.c)
    target_compile_features(quirewise_document_pdf_flate_image_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_flate_image_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_flate_image_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_flate_image
        COMMAND quirewise_document_pdf_flate_image_test)

    add_executable(quirewise_document_pdf_merge_encrypted_test
        tests/pdf_merge_encrypted_test.c)
    target_compile_features(quirewise_document_pdf_merge_encrypted_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_merge_encrypted_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_compile_definitions(quirewise_document_pdf_merge_encrypted_test PRIVATE
        QUIREWISE_ENCRYPTED_PDF="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/encrypted-rc4.pdf")
    target_link_libraries(quirewise_document_pdf_merge_encrypted_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_merge_encrypted
        COMMAND quirewise_document_pdf_merge_encrypted_test)

    add_executable(quirewise_document_pdf_public_api_test
        tests/pdf_public_api_test.c)
    target_compile_features(quirewise_document_pdf_public_api_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_public_api_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_public_api
        COMMAND quirewise_document_pdf_public_api_test)

    add_executable(quirewise_document_pdf_convert_route_test
        tests/pdf_convert_route_test.c)
    target_compile_features(quirewise_document_pdf_convert_route_test PRIVATE c_std_11)
    target_link_libraries(quirewise_document_pdf_convert_route_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_convert_route
        COMMAND quirewise_document_pdf_convert_route_test)

    add_executable(quirewise_document_pdf_corpus_test tests/pdf_corpus_test.c)
    target_compile_features(quirewise_document_pdf_corpus_test PRIVATE c_std_11)
    target_compile_definitions(quirewise_document_pdf_corpus_test PRIVATE
        QUIREWISE_PDF_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../app/testfile/pdf")
    target_link_libraries(quirewise_document_pdf_corpus_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_corpus
        COMMAND quirewise_document_pdf_corpus_test)

    add_executable(quirewise_document_pdf_page_order_test
        tests/pdf_page_order_test.c)
    target_compile_features(quirewise_document_pdf_page_order_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_page_order_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_page_order_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_page_order
        COMMAND quirewise_document_pdf_page_order_test)

    add_executable(quirewise_document_pdf_text_render_test
        tests/pdf_text_render_test.c)
    target_compile_features(quirewise_document_pdf_text_render_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_text_render_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_text_render_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_text_render
        COMMAND quirewise_document_pdf_text_render_test)

    add_executable(quirewise_document_pdf_xref_test tests/pdf_xref_test.c)
    target_compile_features(quirewise_document_pdf_xref_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_xref_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_xref_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_xref
        COMMAND quirewise_document_pdf_xref_test)

    add_executable(quirewise_document_pdf_merge_incremental_test
        tests/pdf_merge_incremental_test.c)
    target_compile_features(quirewise_document_pdf_merge_incremental_test PRIVATE c_std_11)
    target_include_directories(quirewise_document_pdf_merge_incremental_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backends")
    target_link_libraries(quirewise_document_pdf_merge_incremental_test PRIVATE
        quirewise_document)
    add_test(NAME quirewise_document_pdf_merge_incremental
        COMMAND quirewise_document_pdf_merge_incremental_test)
endif()
target_link_libraries(quirewise_document PUBLIC Quirewise::Runtime)
