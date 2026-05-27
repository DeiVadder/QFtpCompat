/**
 * \file qftpcompatdirentry.h
 * \brief FTP directory entry returned by QFtpCompat::list().
 */

// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

class QByteArray;

/**
 * \class QFtpCompatDirEntry
 * \brief Represents a single entry from an FTP LIST response.
 *
 * Populated by \ref QFtpCompatDirEntry::parseList(), which attempts to parse
 * the three most common server listing formats in the following order:
 *
 *  1. **MLSD** (RFC 3659) – \c "Type=file;Size=12345;Modify=20210101120000; name"
 *  2. **Unix** (POSIX ls) – \c "-rw-r--r-- 1 owner group 12345 Jan  1 12:00 name"
 *  3. **Windows** (MS-DOS) – \c "01-01-21  12:00PM <DIR> name"
 *
 * Lines that cannot be parsed in any format are silently skipped;
 * check the returned vector's size if a complete listing is required.
 */
class QFtpCompatDirEntry
{
public:
    QFtpCompatDirEntry() = default;

    // -----------------------------------------------------------------
    // Factory
    // -----------------------------------------------------------------

    /**
     * \brief Parses a full LIST / MLSD response body into directory entries.
     *
     * \param rawListing Raw bytes received on the FTP data channel.
     * \return All successfully parsed entries; unrecognised lines are dropped.
     */
    [[nodiscard]] static QVector<QFtpCompatDirEntry> parseList(const QByteArray &rawListing);

    // -----------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------

    /// File or directory name (never includes the path).
    [[nodiscard]] QString   name()          const noexcept { return m_name; }

    /// Unix permission string, e.g. \c "-rw-r--r--". Empty for Windows listings.
    [[nodiscard]] QString   permissions()   const noexcept { return m_permissions; }

    /// Owning user name. Empty when not available.
    [[nodiscard]] QString   owner()         const noexcept { return m_owner; }

    /// Owning group name. Empty when not available.
    [[nodiscard]] QString   group()         const noexcept { return m_group; }

    /// File size in bytes. 0 for directories.
    [[nodiscard]] qint64    size()          const noexcept { return m_size; }

    /// Last-modification timestamp. May be approximate (year inferred when absent).
    [[nodiscard]] QDateTime lastModified()  const noexcept { return m_lastModified; }

    /// \c true for directory entries.
    [[nodiscard]] bool      isDirectory()   const noexcept { return m_isDirectory; }

    /// \c true for symbolic links.
    [[nodiscard]] bool      isSymLink()     const noexcept { return m_isSymLink; }

    /// Symlink target path. Empty when \ref isSymLink() is \c false.
    [[nodiscard]] QString   linkTarget()    const noexcept { return m_linkTarget; }

    /// Returns a human-readable summary for debug output.
    [[nodiscard]] QString   toString()      const;

private:
    // -----------------------------------------------------------------
    // Per-format parsers (return default-constructed entry on failure)
    // -----------------------------------------------------------------
    [[nodiscard]] static QFtpCompatDirEntry parseMlsd (const QString &line);
    [[nodiscard]] static QFtpCompatDirEntry parseUnix (const QString &line);
    [[nodiscard]] static QFtpCompatDirEntry parseWindows(const QString &line);

    // -----------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------
    QString   m_name;
    QString   m_permissions;
    QString   m_owner;
    QString   m_group;
    qint64    m_size{0};
    QDateTime m_lastModified;
    bool      m_isDirectory{false};
    bool      m_isSymLink{false};
    QString   m_linkTarget;
};
