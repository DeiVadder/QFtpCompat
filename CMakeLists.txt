cmake_minimum_required(VERSION 3.21)
project(QFtpCompat VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

# QSslSocket requires OpenSSL at runtime. CMake does not need to link it
# directly – Qt handles it – but this check gives a clear error message
# if the development headers are missing on the build machine.
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    message(WARNING "OpenSSL not found. QFtpCompat will build but FTPS "
            "(EncryptionMode::Explicit/Implicit) will not work at runtime "
            "unless OpenSSL is available on the target system.")
endif()

# ------------------------------------------------------------------
# Sources
# All headers must be listed explicitly so that AUTOMOC picks up
# Q_OBJECT in qftpcompat.h and generates the required moc_*.cpp file.
# Without this, signals and slots silently disappear at link time.
# ------------------------------------------------------------------
set(QFTPCOMPAT_PUBLIC_HEADERS
    qftpcompat.h
    qftpcompatdirentry.h
)

# Qt-style convenience headers (no extension).
# Users write: #include <QFtpCompat>
# These are listed separately so the install rule can find them.
set(QFTPCOMPAT_UMBRELLA_HEADERS
    QFtpCompat
    QFtpCompatDirEntry
)

set(QFTPCOMPAT_PRIVATE_HEADERS
    private/qftpcompatcommand_p.h
)

set(QFTPCOMPAT_SOURCES
    qftpcompat.cpp
    qftpcompatdirentry.cpp
)

# qt_add_library is the Qt6-idiomatic replacement for add_library.
# It handles AUTOMOC, AUTORCC, and static/shared selection correctly.
qt_add_library(QFtpCompat STATIC
    ${QFTPCOMPAT_UMBRELLA_HEADERS}
    ${QFTPCOMPAT_PUBLIC_HEADERS}
    ${QFTPCOMPAT_PRIVATE_HEADERS}
    ${QFTPCOMPAT_SOURCES}
)

# Exportable alias: consumers write target_link_libraries(MyApp QFtpCompat::QFtpCompat)
add_library(QFtpCompat::QFtpCompat ALIAS QFtpCompat)

# ------------------------------------------------------------------
# Include directories
#
# PUBLIC  – propagated to any target that links QFtpCompat.
#           Consumers can #include "qftpcompat.h" without extra setup.
#
# PRIVATE – only visible inside this library's own translation units.
#           The private/ subdirectory is never exposed to consumers.
# ------------------------------------------------------------------
target_include_directories(QFtpCompat
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/QFtpCompat>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/private
)

# ------------------------------------------------------------------
# Link dependencies
#
# Both Qt6::Core and Qt6::Network are PUBLIC because qftpcompat.h
# exposes QObject / QUrl (Core) and QAbstractSocket::SocketError in
# its private slot declarations (Network).  Any translation unit that
# includes qftpcompat.h therefore needs both modules to compile.
# ------------------------------------------------------------------
target_link_libraries(QFtpCompat
    PUBLIC  Qt6::Core
    PUBLIC  Qt6::Network
)

target_compile_features(QFtpCompat PUBLIC cxx_std_17)

# Suppress qDebug() output in all non-Debug configurations.
# qWarning() and qCritical() are intentionally kept.
target_compile_definitions(QFtpCompat PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG_OUTPUT>
)

# ------------------------------------------------------------------
# Documentation  (Doxygen)
#
# Build:  cmake -DQFTPCOMPAT_BUILD_DOCS=ON ..
#         cmake --build . --target QFtpCompat_docs
#
# Open:   cmake --build . --target QFtpCompat_open_docs
#         (builds docs first, then opens index.html)
# ------------------------------------------------------------------
option(QFTPCOMPAT_BUILD_DOCS "Build Doxygen HTML documentation" OFF)

if(QFTPCOMPAT_BUILD_DOCS)

    # dot (Graphviz) is optional – enables class-diagram generation.
    find_package(Doxygen OPTIONAL_COMPONENTS dot)

    if(NOT DOXYGEN_FOUND)
        message(WARNING
            "QFtpCompat: Doxygen not found – "
            "documentation targets will not be created. "
            "Install Doxygen and re-run CMake with -DQFTPCOMPAT_BUILD_DOCS=ON.")
    else()
        # Resolve the @DOXYGEN_HAVE_DOT@ placeholder in Doxyfile.in.
        if(DOXYGEN_DOT_FOUND)
            set(DOXYGEN_HAVE_DOT "YES")
        else()
            set(DOXYGEN_HAVE_DOT "NO")
            message(STATUS
                "QFtpCompat docs: Graphviz/dot not found – "
                "class diagrams will be disabled.")
        endif()

        # Primary output (build tree) and GitHub Pages output (source tree docs/)
        set(DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/docs")
        set(DOXYGEN_PAGES_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/docs")
        set(DOXYGEN_INDEX_HTML "${DOXYGEN_OUTPUT_DIR}/html/index.html")

        # Substitute all @VARIABLE@ placeholders and write the Doxyfile.
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in"
            "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            @ONLY
        )

        # Target: build the documentation.
        add_custom_target(QFtpCompat_docs
            COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            BYPRODUCTS "${DOXYGEN_INDEX_HTML}"
            COMMENT "Generating QFtpCompat API documentation with Doxygen..."
            VERBATIM
        )

        # Determine the platform-appropriate command for opening a file.
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
            set(_QFTPCOMPAT_OPEN_CMD open)
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
            set(_QFTPCOMPAT_OPEN_CMD cmd /c start "")
        else()
            # Linux / BSD: prefer xdg-open; fall back to a warning.
            find_program(_QFTPCOMPAT_XDG_OPEN xdg-open)
            if(_QFTPCOMPAT_XDG_OPEN)
                set(_QFTPCOMPAT_OPEN_CMD "${_QFTPCOMPAT_XDG_OPEN}")
            else()
                message(STATUS
                    "QFtpCompat docs: xdg-open not found – "
                    "QFtpCompat_open_docs target will print the path instead.")
                set(_QFTPCOMPAT_OPEN_CMD "${CMAKE_COMMAND}" -E echo
                    "Documentation generated at:")
            endif()
        endif()

        # Target: open the generated HTML in the default browser.
        # Depends on QFtpCompat_docs so the docs are always up-to-date first.
        # Copy HTML output to docs/ for GitHub Pages (served from main branch)
        add_custom_command(TARGET QFtpCompat_docs POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${DOXYGEN_OUTPUT_DIR}/html"
                    "${DOXYGEN_PAGES_DIR}/html"
            COMMENT "Copying docs to docs/html/ for GitHub Pages"
        )

        add_custom_target(QFtpCompat_open_docs
            COMMAND ${_QFTPCOMPAT_OPEN_CMD} "${DOXYGEN_INDEX_HTML}"
            DEPENDS QFtpCompat_docs
            COMMENT "Opening QFtpCompat documentation..."
            VERBATIM
        )

        message(STATUS
            "QFtpCompat docs: targets 'QFtpCompat_docs' and "
            "'QFtpCompat_open_docs' available.")

    endif() # DOXYGEN_FOUND
endif() # QFTPCOMPAT_BUILD_DOCS

# ------------------------------------------------------------------
# Install rules (uncomment when packaging the library standalone)
# ------------------------------------------------------------------
# include(GNUInstallDirs)
#
# install(TARGETS QFtpCompat
#     EXPORT  QFtpCompatTargets
#     ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
#     LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
# )
# install(FILES ${QFTPCOMPAT_UMBRELLA_HEADERS} ${QFTPCOMPAT_PUBLIC_HEADERS}
#     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/QFtpCompat
# )
# install(EXPORT QFtpCompatTargets
#     FILE      QFtpCompatConfig.cmake
#     NAMESPACE QFtpCompat::
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QFtpCompat
# )
