// SPDX-License-Identifier: MIT

#include "qftpcompatdirentry.h"

#include <QByteArray>
#include <QDebug>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QTimeZone>

// =============================================================================
// Public factory
// =============================================================================

QVector<QFtpCompatDirEntry> QFtpCompatDirEntry::parseList(const QByteArray &rawListing)
{
    QVector<QFtpCompatDirEntry> result;

    const QString listing = QString::fromUtf8(rawListing);
    const QStringList lines = listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        // Try each format in order of specificity.
        QFtpCompatDirEntry entry;

        if (line.contains(QLatin1Char('='))) {
            // MLSD lines always contain '=' (e.g. "Type=file;Size=…")
            entry = parseMlsd(line);
        } else if (!line.isEmpty() && line.at(0).isLetter()
                   && line.size() > 1 && line.at(1) == QLatin1Char('-')) {
            // Windows: "01-01-21  12:00PM …"  – digit, dash, digit pattern
            entry = parseWindows(line);
        } else {
            // Fall back to Unix ls-style
            entry = parseUnix(line);
        }

        if (!entry.name().isEmpty()) {
            result.append(entry);
        } else {
            qDebug() << "QFtpCompatDirEntry: could not parse listing line:" << line;
        }
    }

    return result;
}

// =============================================================================
// MLSD parser  (RFC 3659)
// Format: "Type=file;Size=12345;Modify=20210101120000; filename"
// =============================================================================

QFtpCompatDirEntry QFtpCompatDirEntry::parseMlsd(const QString &line)
{
    // Mandatory space separates facts from the file name
    const int spaceIdx = line.indexOf(QLatin1Char(' '));
    if (spaceIdx < 0) {
        return {};
    }

    QFtpCompatDirEntry e;
    e.m_name = line.mid(spaceIdx + 1);

    const QStringList facts = line.left(spaceIdx).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &fact : facts) {
        const int eq = fact.indexOf(QLatin1Char('='));
        if (eq < 0) {
            continue;
        }
        const QString key   = fact.left(eq).toLower();
        const QString value = fact.mid(eq + 1);

        if (key == QLatin1String("type")) {
            const QString vl = value.toLower();
            e.m_isDirectory = (vl == QLatin1String("dir") || vl == QLatin1String("cdir")
                               || vl == QLatin1String("pdir"));
        } else if (key == QLatin1String("size")) {
            e.m_size = value.toLongLong();
        } else if (key == QLatin1String("modify")) {
            // YYYYMMDDHHmmss[.fraction]
            e.m_lastModified = QDateTime::fromString(value.left(14),
                                                     QStringLiteral("yyyyMMddHHmmss"));
            e.m_lastModified.setTimeZone(QTimeZone::UTC);
        } else if (key == QLatin1String("unix.mode")) {
            e.m_permissions = value;
        } else if (key == QLatin1String("unix.owner")) {
            e.m_owner = value;
        } else if (key == QLatin1String("unix.group")) {
            e.m_group = value;
        }
    }

    return e;
}

// =============================================================================
// Unix/POSIX parser
// Format: "-rw-r--r-- 1 owner group 12345 Jan  1 12:00 name"
//         "lrwxrwxrwx 1 owner group    11 Jan  1 12:00 link -> target"
// =============================================================================

QFtpCompatDirEntry QFtpCompatDirEntry::parseUnix(const QString &line)
{
    static const QRegularExpression re(
        QStringLiteral(
            // 1: permissions  2: links  3: owner  4: group  5: size
            R"(^([a-zA-Z\-]{10})\s+(\d+)\s+(\S+)\s+(\S+)\s+(\d+)\s+)"
            // 6: month  7: day  8: year-or-time  9: name
            R"((\w{3})\s+(\d{1,2})\s+(\d{4}|\d{2}:\d{2})\s+(.+)$)"
        )
    );

    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch()) {
        return {};
    }

    QFtpCompatDirEntry e;
    e.m_permissions  = m.captured(1);
    e.m_owner        = m.captured(3);
    e.m_group        = m.captured(4);
    e.m_size         = m.captured(5).toLongLong();

    const QChar typeChar = e.m_permissions.isEmpty() ? QLatin1Char('-')
                                                       : e.m_permissions.at(0);
    e.m_isDirectory = (typeChar == QLatin1Char('d'));
    e.m_isSymLink   = (typeChar == QLatin1Char('l'));

    // Date parsing
    const QString month   = m.captured(6);
    const int     day     = m.captured(7).toInt();
    const QString yearOrTime = m.captured(8);

    static const QStringList monthNames = {
        QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("Mar"),
        QStringLiteral("Apr"), QStringLiteral("May"), QStringLiteral("Jun"),
        QStringLiteral("Jul"), QStringLiteral("Aug"), QStringLiteral("Sep"),
        QStringLiteral("Oct"), QStringLiteral("Nov"), QStringLiteral("Dec")
    };
    const int monthNum = monthNames.indexOf(month) + 1; // 0-based → 1-based

    if (yearOrTime.contains(QLatin1Char(':'))) {
        // Time without year: infer year (use previous year if date is in the future)
        const QStringList parts = yearOrTime.split(QLatin1Char(':'));
        const int hour   = parts.value(0).toInt();
        const int minute = parts.value(1).toInt();
        int year = QDate::currentDate().year();
        if (QDate(year, monthNum, day) > QDate::currentDate()) {
            year--;
        }
        e.m_lastModified = QDateTime(QDate(year, monthNum, day),
                                     QTime(hour, minute),
                                     QTimeZone::LocalTime);
    } else {
        const int year = yearOrTime.toInt();
        e.m_lastModified = QDateTime(QDate(year, monthNum, day),
                                     QTime(0, 0),
                                     QTimeZone::LocalTime);
    }

    // Handle symlink "name -> target"
    QString name = m.captured(9);
    if (e.m_isSymLink) {
        const int arrowIdx = name.indexOf(QStringLiteral(" -> "));
        if (arrowIdx >= 0) {
            e.m_linkTarget = name.mid(arrowIdx + 4);
            name           = name.left(arrowIdx);
        }
    }
    e.m_name = name;

    return e;
}

// =============================================================================
// Windows/MS-DOS parser
// Format: "01-01-21  12:00PM <DIR> dirname"
//         "01-01-21  12:00PM       12345 filename"
// =============================================================================

QFtpCompatDirEntry QFtpCompatDirEntry::parseWindows(const QString &line)
{
    static const QRegularExpression re(
        QStringLiteral(
            // 1: date (MM-DD-YY)  2: time  3: <DIR> or size  4: name
            R"(^(\d{2}-\d{2}-\d{2})\s+(\d{2}:\d{2}[AP]M)\s+(<DIR>|\d+)\s+(.+)$)"
        )
    );

    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch()) {
        return {};
    }

    QFtpCompatDirEntry e;

    const QString dateStr = m.captured(1); // MM-DD-YY
    const QString timeStr = m.captured(2); // HH:MMxM
    const QString sizeOrDir = m.captured(3);
    e.m_name = m.captured(4);

    e.m_isDirectory = (sizeOrDir == QLatin1String("<DIR>"));
    if (!e.m_isDirectory) {
        e.m_size = sizeOrDir.toLongLong();
    }

    // Parse date MM-DD-YY
    const QStringList dateParts = dateStr.split(QLatin1Char('-'));
    if (dateParts.size() == 3) {
        int year = dateParts.at(2).toInt();
        year += (year < 70) ? 2000 : 1900; // pivot at 1970
        const int month = dateParts.at(0).toInt();
        const int day   = dateParts.at(1).toInt();

        // Parse time HH:MMxM
        const bool isPm = timeStr.endsWith(QLatin1String("PM"), Qt::CaseInsensitive);
        const QStringList tp = timeStr.left(5).split(QLatin1Char(':'));
        int hour = tp.value(0).toInt();
        if (isPm && hour != 12) { hour += 12; }
        if (!isPm && hour == 12) { hour = 0; }
        const int minute = tp.value(1).toInt();

        e.m_lastModified = QDateTime(QDate(year, month, day),
                                     QTime(hour, minute),
                                     QTimeZone::LocalTime);
    }

    return e;
}

// =============================================================================
// toString
// =============================================================================

QString QFtpCompatDirEntry::toString() const
{
    return QStringLiteral("[%1] %2  size=%3  modified=%4  owner=%5:%6")
        .arg(m_isDirectory ? QLatin1String("DIR ") : QLatin1String("FILE"),
             m_name,
             QString::number(m_size),
             m_lastModified.toString(Qt::ISODate),
             m_owner,
             m_group);
}
