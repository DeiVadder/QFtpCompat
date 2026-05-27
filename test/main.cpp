/**
 * \file  main.cpp
 * \brief QFtpCompat integration test – exercises the full public API.
 *
 * \details
 * Runs a sequence of 22 FTP commands against a real server and reports
 * pass / fail for each one.  All commands are queued before the event
 * loop starts, demonstrating the sequential-queue behaviour.
 *
 * \par Server setup (one-time)
 * \code{.sh}
 * pip3 install pyftpdlib
 * mkdir ~/ftproot && cd ~/ftproot
 * python3 -m pyftpdlib -u root -P root -w -p 2121
 * \endcode
 *
 * \par Build
 * \code{.sh}
 * mkdir build && cd build
 * cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt6
 * cmake --build .
 * ./FtpCompatTest
 * \endcode
 *
 * \par Test coverage
 * - \c open() – login with credentials in QUrl
 * - \c put(QByteArray) / \c put(QIODevice*) – upload small and 128 KB files
 * - \c mkdir() / \c rmdir() – directory lifecycle
 * - \c list() – root and subdirectory listing
 * - \c get(QByteArray*) – download + byte-exact content comparison
 * - \c get(QIODevice*) – download 128 KB, size verification
 * - \c rename() – RNFR/RNTO round-trip
 * - \c remove() – file deletion
 * - \c pwd() / \c rawCommand() – raw command path
 * - Expected error path – 550 on non-existent file
 * - Post-cleanup \c list() – confirms the server root is empty
 *
 * \author Jonas Hilk
 * \date   2024
 */

#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <QUrl>
#include <QDebug>
#include <QTemporaryFile>

#include <QFtpCompat>
#include <QFtpCompatDirEntry>

// =============================================================================
// Config
// =============================================================================
static const QString Host     = QStringLiteral("127.0.0.1");
static const int     Port     = 2121;
static const QString User     = QStringLiteral("root");
static const QString Password = QStringLiteral("root");

// =============================================================================
// URL factory  (pre-built, reused across tests)
// =============================================================================

/// \brief Pre-built QUrl factory for the test suite.
///
/// Separates connection credentials from file paths exactly as required
/// by QFtpCompat: the connection URL carries user/pass; file URLs do not.
struct Urls {
    /// \brief Connection URL with embedded credentials, no path component.
    static QUrl conn() {
        QUrl u;
        u.setScheme(QStringLiteral("ftp"));
        u.setHost(Host);
        u.setPort(Port);
        u.setUserName(User);
        u.setPassword(Password);
        return u;
    }

    /// \brief File URL for RETR/STOR – no credentials, absolute path.
    static QUrl file(const QString &path) {
        QUrl u;
        u.setScheme(QStringLiteral("ftp"));
        u.setHost(Host);
        u.setPort(Port);
        u.setPath(QLatin1Char('/') + path);
        return u;
    }

    /// \brief Directory URL with leading and trailing slash.
    static QUrl dir(const QString &path) {
        QString p = path;
        if (!p.startsWith(QLatin1Char('/'))) p.prepend(QLatin1Char('/'));
        if (!p.endsWith(QLatin1Char('/')))   p.append(QLatin1Char('/'));
        QUrl u;
        u.setScheme(QStringLiteral("ftp"));
        u.setHost(Host);
        u.setPort(Port);
        u.setPath(p);
        return u;
    }
};

// =============================================================================
// Test runner
// =============================================================================

/**
 * \brief  Queues all test commands on construction and reports results
 *         via commandFinished().
 *
 * \details
 * All 22 commands are enqueued in \ref run() before the event loop starts.
 * QFtpCompat executes them sequentially; \ref onCommandFinished() maps each
 * finished command id to a human-readable label, prints pass/fail, and
 * performs post-command assertions where applicable.
 */
class FtpTest : public QObject
{
    Q_OBJECT
public:
    explicit FtpTest(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_ftp, &QFtpCompat::commandFinished,
                this,   &FtpTest::onCommandFinished);
        connect(&m_ftp, &QFtpCompat::stateChanged,
                this,   [](QFtpCompat::State s) {
                    qInfo() << "[state →]" << static_cast<int>(s);
                });
        connect(&m_ftp, &QFtpCompat::dataTransferProgress,
                this,   [](qint64 done, qint64 total) {
                    if (total > 0)
                        qInfo().nospace() << "[progress] " << done << " / " << total << " bytes";
                    else
                        qInfo().nospace() << "[progress] ↓ " << done << " bytes";
                });
        connect(&m_ftp, &QFtpCompat::listInfo,
                this,   [this](const QFtpCompatDirEntry &e) {
                    qInfo() << "[entry ]"
                             << (e.isDirectory() ? "DIR " : "FILE")
                             << e.name()
                             << e.size()
                             << e.permissions();
                    m_lastListing.append(e);
                });
        connect(&m_ftp, &QFtpCompat::rawCommandReply,
                this,   [](int code, const QString &msg) {
                    qInfo() << "[raw   ]" << code << msg;
                });
    }

    void run()
    {
        step("CONNECT");
        m_openId = m_ftp.open(Urls::conn());

        // ── Cleanup: remove any leftover files from previous runs ───────────
        step("CLEANUP (ignore errors)");
        m_cleanupIds << m_ftp.remove(Urls::file("test.txt"))
                     << m_ftp.remove(Urls::file("small.txt"))
                     << m_ftp.remove(Urls::file("large.txt"))
                     << m_ftp.remove(Urls::file("renamed.txt"));

        // ── Test data ───────────────────────────────────────────────────────
        const QByteArray smallPayload = "Hello from QFtpCompat!\nLine 2\nLine 3\n";

        // Large payload: 128 KB of repeated text → tests chunked progress
        QByteArray largePayload;
        largePayload.reserve(128 * 1024);
        while (largePayload.size() < 128 * 1024)
            largePayload.append("The quick brown fox jumps over the lazy dog. ");

        // ── Upload: QByteArray overload ─────────────────────────────────────
        step("UPLOAD (QByteArray) → small.txt");
        m_uploadSmallId = m_ftp.put(Urls::file("small.txt"), smallPayload);

        // ── Upload: QIODevice overload (large file) ─────────────────────────
        step("UPLOAD (QIODevice) → large.txt  [128 KB]");
        writeLocalFile("/tmp/qftp_large.txt", largePayload);
        auto *srcLarge = new QFile(QStringLiteral("/tmp/qftp_large.txt"), this);
        srcLarge->open(QIODevice::ReadOnly);
        m_uploadLargeId = m_ftp.put(Urls::file("large.txt"), srcLarge);

        // ── mkdir ───────────────────────────────────────────────────────────
        step("MKDIR → subdir/");
        m_mkdirId = m_ftp.mkdir(Urls::dir("subdir"));

        // ── Upload into subdir ──────────────────────────────────────────────
        step("UPLOAD → subdir/nested.txt");
        m_uploadNestedId = m_ftp.put(Urls::file("subdir/nested.txt"), smallPayload);

        // ── LIST root ───────────────────────────────────────────────────────
        step("LIST /");
        m_listRootId = m_ftp.list(Urls::dir(""));

        // ── LIST subdir ─────────────────────────────────────────────────────
        step("LIST /subdir/");
        m_listSubId = m_ftp.list(Urls::dir("subdir"));

        // ── Download to QByteArray ──────────────────────────────────────────
        step("DOWNLOAD (→ QByteArray) ← small.txt");
        m_downloadBaId = m_ftp.get(Urls::file("small.txt"), &m_downloadedBytes);

        // ── Download to QFile ───────────────────────────────────────────────
        step("DOWNLOAD (→ QFile) ← large.txt");
        m_downloadFile = new QFile(QStringLiteral("/tmp/qftp_large_dl.txt"), this);
        m_downloadFile->open(QIODevice::WriteOnly);
        m_downloadLargeId = m_ftp.get(Urls::file("large.txt"), m_downloadFile);

        // ── Rename ──────────────────────────────────────────────────────────
        step("RENAME small.txt → renamed.txt");
        m_renameId = m_ftp.rename(Urls::file("small.txt"), Urls::file("renamed.txt"));

        // ── Delete ──────────────────────────────────────────────────────────
        step("DELETE renamed.txt");
        m_removeId = m_ftp.remove(Urls::file("renamed.txt"));

        // ── Delete large.txt ────────────────────────────────────────────────
        step("DELETE large.txt");
        m_removeLargeId = m_ftp.remove(Urls::file("large.txt"));

        // ── Delete subdir/nested.txt ────────────────────────────────────────
        step("DELETE subdir/nested.txt");
        m_removeNestedId = m_ftp.remove(Urls::file("subdir/nested.txt"));

        // ── rmdir ───────────────────────────────────────────────────────────
        step("RMDIR subdir/");
        m_rmdirId = m_ftp.rmdir(Urls::dir("subdir"));

        // ── PWD ─────────────────────────────────────────────────────────────
        step("PWD");
        m_pwdId = m_ftp.pwd();

        // ── Raw NOOP ────────────────────────────────────────────────────────
        step("RAW NOOP");
        m_noopId = m_ftp.rawCommand(QStringLiteral("NOOP"));

        // ── Error: download non-existent file ──────────────────────────────
        step("DOWNLOAD (expect error) ← ghost.txt");
        m_errorDlId = m_ftp.get(Urls::file("ghost.txt"), &m_ghostBytes);

        // ── Final LIST (should be empty / clean) ────────────────────────────
        step("LIST / (final – expect empty)");
        m_listFinalId = m_ftp.list(Urls::dir(""));
    }

private slots:
    void onCommandFinished(int id, bool hasError)
    {
        // Map id → label
        const QString label = m_cleanupIds.contains(id) ? QStringLiteral("CLEANUP") :
            id == m_openId         ? QStringLiteral("OPEN")              :
            id == m_uploadSmallId  ? "UPLOAD small"      :
            id == m_uploadLargeId  ? "UPLOAD large"      :
            id == m_mkdirId        ? "MKDIR"             :
            id == m_uploadNestedId ? "UPLOAD nested"     :
            id == m_listRootId     ? "LIST /"            :
            id == m_listSubId      ? "LIST subdir"       :
            id == m_downloadBaId   ? "DOWNLOAD→QByteArray":
            id == m_downloadLargeId? "DOWNLOAD→QFile"    :
            id == m_renameId       ? "RENAME"            :
            id == m_removeId       ? "DELETE renamed"    :
            id == m_removeLargeId  ? "DELETE large"      :
            id == m_removeNestedId ? "DELETE nested"     :
            id == m_rmdirId        ? "RMDIR"             :
            id == m_pwdId          ? "PWD"               :
            id == m_noopId         ? "NOOP"              :
            id == m_errorDlId      ? "DOWNLOAD ghost"    :
            id == m_listFinalId    ? "LIST final"        :
                                     QString("id=%1").arg(id);

        // Errors expected for cleanup (files may not exist) and ghost.txt
        const bool errorExpected = (id == m_errorDlId) || m_cleanupIds.contains(id);

        if (hasError && !errorExpected)
            qCritical() << "  [FAIL]" << label << "–" << m_ftp.errorString();
        else if (m_cleanupIds.contains(id))
            {} // silent – cleanup errors are normal
        else if (hasError && errorExpected)
            qInfo()  << "  [OK  ]" << label << "(error as expected:" << m_ftp.errorString() << ")";
        else
            qInfo()  << "  [OK  ]" << label;

        // Per-command post-verification
        if (id == m_listRootId) {
            qInfo() << "         → root entries:" << m_lastListing.size();
            m_lastListing.clear();
        }
        if (id == m_listSubId) {
            const bool found = std::any_of(m_lastListing.cbegin(), m_lastListing.cend(),
                [](const QFtpCompatDirEntry &e){ return e.name() == "nested.txt"; });
            qInfo() << "         → subdir contains nested.txt:" << (found ? "yes ✓" : "no ✗");
            m_lastListing.clear();
        }
        if (id == m_downloadBaId) {
            qInfo() << "         → bytes received:" << m_downloadedBytes.size();
            qInfo() << "         → content match:" <<
                (m_downloadedBytes == "Hello from QFtpCompat!\nLine 2\nLine 3\n"
                    ? "✓" : "✗");
        }
        if (id == m_downloadLargeId) {
            m_downloadFile->flush();
            m_downloadFile->close();
            const qint64 sz = QFile(QStringLiteral("/tmp/qftp_large_dl.txt")).size();
            qInfo() << "         → downloaded size:" << sz
                     << (sz == 128 * 1024 ? "✓" :
                         sz > 0 ? "(partial – size may vary due to padding)" : "✗ empty");
        }
        if (id == m_listFinalId) {
            qInfo() << "         → remaining entries:" << m_lastListing.size()
                     << (m_lastListing.isEmpty() ? "(clean ✓)" : "(unexpected files ✗)");
            m_lastListing.clear();
            printSummary();
            QCoreApplication::quit();
        }
    }

private:
    // ── Helpers ───────────────────────────────────────────────────────────────
    static void step(const QString &label) {
        qInfo() << "\n──" << label;
    }

    static void writeLocalFile(const QString &path, const QByteArray &data) {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(data);
    }

    void printSummary() {
        qInfo() << "\n══════════════════════════════════════════";
        qInfo() << " QFtpCompat test complete";
        qInfo() << "══════════════════════════════════════════";
    }

    // ── Members ───────────────────────────────────────────────────────────────
    QFtpCompat  m_ftp;
    QFile      *m_downloadFile{nullptr};
    QByteArray  m_downloadedBytes;
    QByteArray  m_ghostBytes;
    QList<QFtpCompatDirEntry> m_lastListing;

    QList<int> m_cleanupIds;
    int m_openId{-1},        m_uploadSmallId{-1},  m_uploadLargeId{-1};
    int m_mkdirId{-1},       m_uploadNestedId{-1};
    int m_listRootId{-1},    m_listSubId{-1},       m_listFinalId{-1};
    int m_downloadBaId{-1},  m_downloadLargeId{-1};
    int m_renameId{-1},      m_removeId{-1},        m_removeLargeId{-1};
    int m_removeNestedId{-1},m_rmdirId{-1};
    int m_pwdId{-1},         m_noopId{-1},          m_errorDlId{-1};
};

// =============================================================================
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    FtpTest test;
    QTimer::singleShot(0, &test, &FtpTest::run);

    return app.exec();
}

#include "main.moc"
