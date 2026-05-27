/**
 * \file  main.cpp
 * \brief QFtpCompat integration test – plain FTP + Explicit FTPS.
 *
 * \par Server setup
 * \code{.sh}
 * # Plain FTP (port 2121)
 * mkdir ~/ftproot && cd ~/ftproot
 * python3 -m pyftpdlib -u root -P root -w -p 2121
 *
 * # Explicit FTPS (port 2122) – separate terminal
 * openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
 *             -days 365 -nodes -subj "/CN=localhost"
 * pip3 install pyopenssl
 * python3 server_tls.py
 * \endcode
 *
 * \author Jonas Hilk
 * \date   2024 Created
 * \date   2024 Updated
 */

#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <QUrl>
#include <QDebug>

#include <QFtpCompat>
#include <QFtpCompatDirEntry>

// ── Config ────────────────────────────────────────────────────────────────────
static const QString Host         = QStringLiteral("127.0.0.1");
static const int     PlainPort    = 2121;
static const int     TlsPort      = 2122;
static const QString User         = QStringLiteral("root");
static const QString Password     = QStringLiteral("root");

// ── URL factory ───────────────────────────────────────────────────────────────
struct Urls {
    static QUrl conn(int port = PlainPort) {
        QUrl u;
        u.setScheme(QStringLiteral("ftp"));
        u.setHost(Host);
        u.setPort(port);
        u.setUserName(User);
        u.setPassword(Password);
        return u;
    }
    static QUrl file(const QString &path, int port = PlainPort) {
        QUrl u;
        u.setScheme(QStringLiteral("ftp"));
        u.setHost(Host);
        u.setPort(port);
        u.setPath(QLatin1Char('/') + path);
        return u;
    }
    static QUrl dir(const QString &path, int port = PlainPort) {
        QString p = path;
        if (!p.startsWith(QLatin1Char('/'))) p.prepend(QLatin1Char('/'));
        if (!p.endsWith(QLatin1Char('/')))   p.append(QLatin1Char('/'));
        QUrl u;
        u.setScheme(QStringLiteral("ftp"));
        u.setHost(Host);
        u.setPort(port);
        u.setPath(p);
        return u;
    }
};

// =============================================================================
// Plain FTP test (port 2121) – full API coverage
// =============================================================================
class FtpTest : public QObject
{
    Q_OBJECT
public:
    explicit FtpTest(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_ftp, &QFtpCompat::commandFinished,
                this,   &FtpTest::onCommandFinished);
        connect(&m_ftp, &QFtpCompat::stateChanged, this,
                [](QFtpCompat::State s){ qInfo() << "[state →]" << static_cast<int>(s); });
        connect(&m_ftp, &QFtpCompat::dataTransferProgress, this,
                [](qint64 done, qint64 total){
                    if (total > 0) qInfo().nospace() << "[progress] " << done << "/" << total;
                    else           qInfo().nospace() << "[progress] ↓ " << done << " bytes";
                });
        connect(&m_ftp, &QFtpCompat::listInfo, this,
                [this](const QFtpCompatDirEntry &e){
                    qInfo() << "[entry ]" << (e.isDirectory() ? "DIR" : "FILE")
                    << e.name() << e.size();
                    m_lastListing.append(e);
                });
        connect(&m_ftp, &QFtpCompat::rawCommandReply, this,
                [](int code, const QString &msg){ qInfo() << "[raw]" << code << msg; });
    }

public slots:
    void run()
    {
        step("CONNECT (plain)");
        m_openId = m_ftp.open(Urls::conn(PlainPort));

        step("CLEANUP");
        m_cleanupIds << m_ftp.remove(Urls::file("test.txt"))
                     << m_ftp.remove(Urls::file("small.txt"))
                     << m_ftp.remove(Urls::file("large.txt"))
                     << m_ftp.remove(Urls::file("renamed.txt"));

        const QByteArray small = "Hello from QFtpCompat!\nLine 2\nLine 3\n";
        QByteArray large; large.reserve(128*1024);
        while (large.size() < 128*1024) large.append("The quick brown fox jumps over the lazy dog. ");

        step("UPLOAD small (QByteArray)");
        m_uploadSmallId = m_ftp.put(Urls::file("small.txt"), small);

        step("UPLOAD large (QIODevice, 128 KB)");
        writeFile("/tmp/qftp_large.txt", large);
        auto *src = new QFile("/tmp/qftp_large.txt", this);
        src->open(QIODevice::ReadOnly);
        m_uploadLargeId = m_ftp.put(Urls::file("large.txt"), src);

        step("MKDIR subdir/");
        m_mkdirId = m_ftp.mkdir(Urls::dir("subdir"));

        step("UPLOAD subdir/nested.txt");
        m_uploadNestedId = m_ftp.put(Urls::file("subdir/nested.txt"), small);

        step("LIST /");
        m_listRootId = m_ftp.list(Urls::dir(""));

        step("LIST /subdir/");
        m_listSubId = m_ftp.list(Urls::dir("subdir"));

        step("DOWNLOAD small → QByteArray");
        m_downloadBaId = m_ftp.get(Urls::file("small.txt"), &m_downloadedBytes);

        step("DOWNLOAD large → QFile");
        m_downloadFile = new QFile("/tmp/qftp_large_dl.txt", this);
        m_downloadFile->open(QIODevice::WriteOnly);
        m_downloadLargeId = m_ftp.get(Urls::file("large.txt"), m_downloadFile);

        step("RENAME small.txt → renamed.txt");
        m_renameId = m_ftp.rename(Urls::file("small.txt"), Urls::file("renamed.txt"));

        step("DELETE renamed / large / nested");
        m_removeId       = m_ftp.remove(Urls::file("renamed.txt"));
        m_removeLargeId  = m_ftp.remove(Urls::file("large.txt"));
        m_removeNestedId = m_ftp.remove(Urls::file("subdir/nested.txt"));

        step("RMDIR subdir/");
        m_rmdirId = m_ftp.rmdir(Urls::dir("subdir"));

        step("PWD");
        m_pwdId = m_ftp.pwd();

        step("RAW NOOP");
        m_noopId = m_ftp.rawCommand("NOOP");

        step("DOWNLOAD ghost.txt (expect 550)");
        m_errorDlId = m_ftp.get(Urls::file("ghost.txt"), &m_ghostBytes);

        step("LIST / final");
        m_listFinalId = m_ftp.list(Urls::dir(""));
    }

signals:
    void plainDone();

private slots:
    void onCommandFinished(int id, bool hasError)
    {
        const QString label = m_cleanupIds.contains(id) ? QStringLiteral("CLEANUP") :
                                  id==m_openId         ? "OPEN"               : id==m_uploadSmallId  ? "UPLOAD small"  :
                                  id==m_uploadLargeId  ? "UPLOAD large"       : id==m_mkdirId        ? "MKDIR"         :
                                  id==m_uploadNestedId ? "UPLOAD nested"      : id==m_listRootId     ? "LIST /"        :
                                  id==m_listSubId      ? "LIST subdir"        : id==m_downloadBaId   ? "DL→QByteArray" :
                                  id==m_downloadLargeId? "DL→QFile"           : id==m_renameId       ? "RENAME"        :
                                  id==m_removeId       ? "DELETE renamed"     : id==m_removeLargeId  ? "DELETE large"  :
                                  id==m_removeNestedId ? "DELETE nested"      : id==m_rmdirId        ? "RMDIR"         :
                                  id==m_pwdId          ? "PWD"                : id==m_noopId         ? "NOOP"          :
                                  id==m_errorDlId      ? "DL ghost"           : id==m_listFinalId    ? "LIST final"    :
                                  QString("id=%1").arg(id);

        const bool errExpected = (id == m_errorDlId) || m_cleanupIds.contains(id);

        if      (hasError && !errExpected)  qCritical() << "  [FAIL]" << label << m_ftp.errorString();
        else if (m_cleanupIds.contains(id)) {}
        else if (hasError)                  qInfo() << "  [OK  ]" << label << "(error expected)";
        else                                qInfo() << "  [OK  ]" << label;

        if (id == m_listRootId) {
            qInfo() << "         → entries:" << m_lastListing.size();
            m_lastListing.clear();
        }
        if (id == m_listSubId) {
            const bool found = std::any_of(m_lastListing.cbegin(), m_lastListing.cend(),
                                           [](const QFtpCompatDirEntry &e){ return e.name() == "nested.txt"; });
            qInfo() << "         → nested.txt:" << (found ? "yes ✓" : "no ✗");
            m_lastListing.clear();
        }
        if (id == m_downloadBaId) {
            qInfo() << "         → content match:"
                    << (m_downloadedBytes == "Hello from QFtpCompat!\nLine 2\nLine 3\n" ? "✓" : "✗");
        }
        if (id == m_downloadLargeId) {
            m_downloadFile->flush(); m_downloadFile->close();
            qInfo() << "         → size:" << QFile("/tmp/qftp_large_dl.txt").size();
        }
        if (id == m_listFinalId) {
            // Filter out permanent ftproot files (.DS_Store, *.pem)
            const auto unexpected = std::count_if(
                m_lastListing.cbegin(), m_lastListing.cend(),
                [](const QFtpCompatDirEntry &e) {
                    return e.name() != ".DS_Store"
                           && !e.name().endsWith(".pem");
                });
            qInfo() << "         → remaining (excl. permanent files):" << unexpected
                    << (unexpected == 0 ? "(clean ✓)" : "(unexpected files ✗)");
            m_lastListing.clear();
            qInfo() << "\n══ Plain FTP complete ══\n";
            emit plainDone();
        }
    }

    static void step(const QString &l) { qInfo() << "\n──" << l; }
    static void writeFile(const QString &p, const QByteArray &d) {
        QFile f(p); f.open(QIODevice::WriteOnly); f.write(d);
    }

private:
    QFtpCompat m_ftp;
    QFile     *m_downloadFile{nullptr};
    QByteArray m_downloadedBytes, m_ghostBytes;
    QList<QFtpCompatDirEntry> m_lastListing;
    QList<int> m_cleanupIds;
    int m_openId{-1}, m_uploadSmallId{-1}, m_uploadLargeId{-1},
        m_mkdirId{-1}, m_uploadNestedId{-1},
        m_listRootId{-1}, m_listSubId{-1}, m_listFinalId{-1},
        m_downloadBaId{-1}, m_downloadLargeId{-1},
        m_renameId{-1}, m_removeId{-1}, m_removeLargeId{-1},
        m_removeNestedId{-1}, m_rmdirId{-1},
        m_pwdId{-1}, m_noopId{-1}, m_errorDlId{-1};
};

// =============================================================================
// Explicit FTPS test (port 2122)
// Tests: connect with AUTH TLS, self-signed cert ignore, upload, download,
//        content verify, delete – confirms TLS data channel works end-to-end.
// =============================================================================
class FtpTlsTest : public QObject
{
    Q_OBJECT
public:
    explicit FtpTlsTest(QObject *parent = nullptr) : QObject(parent)
    {
        // Accept self-signed certificate from the test server
        connect(&m_ftp, &QFtpCompat::sslErrors,
                &m_ftp, &QFtpCompat::ignoreSslErrors);

        connect(&m_ftp, &QFtpCompat::commandFinished,
                this,   &FtpTlsTest::onCommandFinished);
        connect(&m_ftp, &QFtpCompat::stateChanged, this,
                [](QFtpCompat::State s){ qInfo() << "[TLS state →]" << static_cast<int>(s); });
        connect(&m_ftp, &QFtpCompat::dataTransferProgress, this,
                [](qint64 done, qint64 total){
                    if (total > 0) qInfo().nospace() << "[TLS progress] " << done << "/" << total << " bytes";
                    else           qInfo().nospace() << "[TLS progress] ↓ " << done << " bytes";
                });
        connect(&m_ftp, &QFtpCompat::listInfo, this,
                [this](const QFtpCompatDirEntry &e) {
                    m_lastListing.append(e);
                });
    }

public slots:
    void run()
    {
        qInfo() << "\n══════════════════════════════════════════";
        qInfo() << " Explicit FTPS test (port" << TlsPort << ")";
        qInfo() << "══════════════════════════════════════════";

        step("CONNECT (Explicit FTPS – AUTH TLS)");
        // EncryptionMode::Explicit: plain TCP → AUTH TLS → PBSZ 0 → PROT P → login
        m_openId = m_ftp.open(Urls::conn(TlsPort),
                              QFtpCompat::EncryptionMode::Explicit);

        step("CLEANUP");
        m_cleanupIds << m_ftp.remove(Urls::file("tls_test.txt", TlsPort));

        step("UPLOAD tls_test.txt (over encrypted channel)");
        m_uploadId = m_ftp.put(Urls::file("tls_test.txt", TlsPort),
                               QByteArray("TLS upload works!\n"));

        step("LIST / (verify file visible)");
        m_listId = m_ftp.list(Urls::dir("", TlsPort));

        step("DOWNLOAD tls_test.txt → QByteArray");
        m_downloadId = m_ftp.get(Urls::file("tls_test.txt", TlsPort),
                                 &m_downloadedBytes);

        step("DELETE tls_test.txt");
        m_removeId = m_ftp.remove(Urls::file("tls_test.txt", TlsPort));

        step("LIST / final (expect empty)");
        m_listFinalId = m_ftp.list(Urls::dir("", TlsPort));
    }

private slots:
    void onCommandFinished(int id, bool hasError)
    {
        const QString label =
            m_cleanupIds.contains(id) ? QStringLiteral("CLEANUP") :
                id==m_openId     ? "OPEN (FTPS)"   : id==m_uploadId   ? "UPLOAD"       :
                id==m_listId     ? "LIST"           : id==m_downloadId ? "DOWNLOAD"     :
                id==m_removeId   ? "DELETE"         : id==m_listFinalId? "LIST final"   :
                QString("id=%1").arg(id);

        const bool errExpected = m_cleanupIds.contains(id);

        if      (hasError && !errExpected)  qCritical() << "  [FAIL]" << label << m_ftp.errorString();
        else if (m_cleanupIds.contains(id)) {}
        else                                qInfo()  << "  [OK  ]" << label;

        if (id == m_listId) {
            const bool found = std::any_of(m_lastListing.cbegin(), m_lastListing.cend(),
                                           [](const QFtpCompatDirEntry &e){ return e.name() == "tls_test.txt"; });
            qInfo() << "         → tls_test.txt visible:" << (found ? "yes ✓" : "no ✗");
            m_lastListing.clear();
        }
        if (id == m_downloadId) {
            qInfo() << "         → content match:"
                    << (m_downloadedBytes == "TLS upload works!\n" ? "✓" : "✗");
        }
        if (id == m_listFinalId) {
            const auto unexpected = std::count_if(
                m_lastListing.cbegin(), m_lastListing.cend(),
                [](const QFtpCompatDirEntry &e) {
                    return e.name() != ".DS_Store"
                           && !e.name().endsWith(".pem");
                });
            qInfo() << "         → remaining (excl. permanent files):" << unexpected
                    << (unexpected == 0 ? "(clean ✓)" : "(unexpected files ✗)");
            qInfo() << "\n══ Explicit FTPS complete ══\n";
            QCoreApplication::quit();
        }
    }

    static void step(const QString &l) { qInfo() << "\n──" << l; }

private:
    QFtpCompat m_ftp;
    QByteArray m_downloadedBytes;
    QList<QFtpCompatDirEntry> m_lastListing;
    QList<int> m_cleanupIds;
    int m_openId{-1}, m_uploadId{-1}, m_listId{-1},
        m_downloadId{-1}, m_removeId{-1}, m_listFinalId{-1};
};

// =============================================================================
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Run plain test first, then TLS test when plain is done
    auto *plain = new FtpTest(&app);
    auto *tls   = new FtpTlsTest(&app);

    QObject::connect(plain, &FtpTest::plainDone,
                     tls,   &FtpTlsTest::run);

    QTimer::singleShot(0, plain, &FtpTest::run);

    return app.exec();
}

#include "main.moc"
