/**
 * \file qftpcompat.h
 * \brief Qt6-compatible async FTP client.
 */

// SPDX-License-Identifier: MIT

#pragma once

#include "qftpcompatdirentry.h"

#include <QAbstractSocket>
#include <QObject>
#include <QQueue>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QUrl>

class QIODevice;
class QByteArray;

namespace QFtpCompatInternal { struct Command; }

/**
 * \class QFtpCompat
 * \brief Asynchronous FTP client for Qt6.
 *
 * Replaces the \c QFtp class that was removed from Qt5 and provides an
 * identical asynchronous, command-queue-based API style.
 *
 * \section qftpcompat_usage Usage
 * \code
 * auto *ftp = new QFtpCompat(this);
 *
 * connect(ftp, &QFtpCompat::commandFinished, this, [ftp](int id, bool error) {
 *     if (error)
 *         qWarning() << "FTP error:" << ftp->errorString();
 * });
 * connect(ftp, &QFtpCompat::listInfo, this, [](QFtpCompatDirEntry e) {
 *     qDebug() << e.toString();
 * });
 *
 * ftp->open(QUrl("ftp://user:pass@192.168.1.1/"));
 * ftp->list(QUrl("ftp://192.168.1.1/flash/data/"));
 * ftp->get(QUrl("ftp://192.168.1.1/flash/data/config.cfg"), &buffer);
 * \endcode
 *
 * Commands are queued and executed sequentially.  Each method returns an
 * integer ID that matches the \p id parameter in \ref commandStarted and
 * \ref commandFinished.
 *
 * \section qftpcompat_modes Transfer modes
 * Only **Passive** mode (PASV) is implemented.  Active mode (PORT) is
 * reserved for future implementation.
 *
 * Both \c Binary (\c TYPE \c I) and \c Ascii (\c TYPE \c A) transfer types
 * are supported at the protocol level.  CRLF normalisation for Ascii transfers
 * is not yet applied in the data stream; see the \c Ascii note in
 * \ref TransferType.
 *
 * \section qftpcompat_threading Thread affinity
 * Must live on the thread that runs the Qt event loop (typically the main
 * thread).  Do not call any method from a different thread.
 *
 * \date 2024 – Created
 * \date 2026 – Updated
 */
class QFtpCompat : public QObject
{
    Q_OBJECT

public:
    // -----------------------------------------------------------------
    // Enumerations
    // -----------------------------------------------------------------

    /**
     * \brief High-level connection state.
     */
    enum class State {
        Unconnected, ///< No TCP connection established.
        Connecting,  ///< TCP connect in progress.
        Connected,   ///< TCP connected, waiting for server greeting (220).
        LoggedIn,    ///< Authenticated; ready to accept commands.
        Closing,     ///< QUIT sent, waiting for server acknowledgement.
    };
    Q_ENUM(State)

    /**
     * \brief Error codes set on the most recent failed command.
     */
    enum class Error {
        NoError,            ///< No error since the last successful command.
        HostNotFound,       ///< DNS resolution failed.
        ConnectionRefused,  ///< Server actively refused the TCP connection.
        NetworkTimeout,     ///< No activity within the inactivity timeout.
        NetworkError,       ///< Generic socket error.
        NotConnected,       ///< Command issued without an active connection.
        NotLoggedIn,        ///< Command requires authentication.
        LoginFailed,        ///< USER/PASS rejected by the server.
        FileNotFound,       ///< Server returned 550 for a file operation.
        PermissionDenied,   ///< Server returned 553 / 550 permission error.
        ServerError,        ///< Unexpected server response code.
        ParseError,         ///< Failed to parse a server response (PASV, LIST).
        LocalFileError,     ///< Could not open or write to the local file/device.
        Aborted,            ///< Operation cancelled by \ref abort().
        TransferFailed,     ///< Data channel closed before transfer completed.
        UnknownError,       ///< Catch-all for unclassified errors.
        // TLS-specific
        TlsHandshakeFailed, ///< TLS handshake failed on control or data channel.
        AuthTlsRejected,    ///< Server rejected the AUTH TLS command.
    };
    Q_ENUM(Error)

    /**
     * \brief FTP data channel mode.
     * \note Only \c Passive is currently implemented.
     *       \c Active (PORT) is reserved for a future release.
     */
    enum class TransferMode {
        Passive, ///< PASV – client opens the data connection (firewall-friendly).
        Active,  ///< PORT – server opens the data connection (not implemented).
    };
    Q_ENUM(TransferMode)

    /**
     * \brief FTP data representation type (RFC 959 §3.1.1).
     *
     * \note \c Ascii sends \c TYPE \c A to the server and therefore starts a
     *       valid ASCII-mode transfer at the protocol level, but the library
     *       currently does **not** perform CRLF normalisation on the data
     *       stream.  If your use case requires proper line-ending translation,
     *       apply it to the data before passing it to \ref put(), or after
     *       receiving it from \ref get().
     */
    enum class TransferType {
        Binary, ///< TYPE I – raw binary (8-bit) transfer.  Fully implemented.
        Ascii,  ///< TYPE A – text transfer.  Protocol-level TYPE sent; no CRLF transform (TODO).
    };
    Q_ENUM(TransferType)

    /**
     * \brief FTP encryption mode.
     * \value None     Plain FTP – default, fully backward-compatible.
     * \value Explicit Explicit FTPS (AUTH TLS, port 21).
     * \value Implicit Implicit FTPS (TLS from first byte, port 990).
     */
    enum class EncryptionMode {
        None,
        Explicit,
        Implicit,
    };
    Q_ENUM(EncryptionMode)

    // -----------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------

    explicit QFtpCompat(QObject *parent = nullptr);
    ~QFtpCompat() override;

    // -----------------------------------------------------------------
    // Connection commands
    // -----------------------------------------------------------------

    /**
     * \brief Connects to the FTP server and logs in using the URL credentials.
     *
     * The \p url must contain at minimum a host.  Port defaults to 21.
     * User defaults to \c "anonymous", password to \c "anonymous\@".
     *
     * Credentials can be embedded in the URL string or set via
     * \c QUrl::setUserName() / \c QUrl::setPassword() – both forms are
     * equivalent since the implementation reads \c url.userName() and
     * \c url.password() regardless of how the \c QUrl was constructed:
     *
     * \code
     * // Inline in the URL string
     * ftp->open(QUrl("ftp://admin:secret@192.168.1.1/"));
     *
     * // Via QUrl properties (e.g. when reading from QSettings)
     * QUrl url;
     * url.setScheme("ftp");
     * url.setHost(host);
     * url.setPort(port);
     * url.setUserName(user);
     * url.setPassword(password);
     * ftp->open(url);
     * \endcode
     *
     * \param url FTP URL carrying at minimum a host name.
     * \return Command ID for tracking via \ref commandFinished.
     */
    /// \brief Connect and log in.
    /// \param url     Server URL (scheme, host, port, user, password).
    /// \param mode    Encryption mode.  Default: \c EncryptionMode::None
    ///                 (plain FTP – backward-compatible).
    int open(const QUrl &url,
             EncryptionMode mode = EncryptionMode::None);

    /**
     * \brief Sends QUIT and closes the connection gracefully.
     *
     * Remaining queued commands are cleared.  Emits \ref done(false) when
     * the server acknowledges the QUIT.
     */
    void close();

    /**
     * \brief Aborts the current data transfer and clears the command queue.
     *
     * Sends \c ABOR on the control channel.  Emits \ref commandFinished
     * with \p error = \c true for the aborted command.
     */
    void abort();

    // -----------------------------------------------------------------
    // Transfer commands
    // -----------------------------------------------------------------

    /**
     * \brief Downloads a remote file into \p dst.
     *
     * \param url  Full URL of the remote file.
     * \param dst  Open writable \c QIODevice.  Not owned by \c QFtpCompat.
     * \param type Transfer type (Binary recommended for binary files).
     * \return Command ID.
     */
    int get(const QUrl &url, QIODevice *dst,
            TransferType type = TransferType::Binary);

    /**
     * \brief Downloads a remote file into \p dst byte array.
     *
     * \param url  Full URL of the remote file.
     * \param dst  Byte array to append downloaded data to.  Not owned.
     * \param type Transfer type.
     * \return Command ID.
     */
    int get(const QUrl &url, QByteArray *dst,
            TransferType type = TransferType::Binary);

    /**
     * \brief Uploads \p data to the remote path in \p url.
     *
     * \param url  Full URL of the remote destination.
     * \param data Data to upload.  Copied internally.
     * \param type Transfer type.
     * \return Command ID.
     */
    int put(const QUrl &url, const QByteArray &data,
            TransferType type = TransferType::Binary);

    /**
     * \brief Uploads data from \p src to the remote path in \p url.
     *
     * \param url  Full URL of the remote destination.
     * \param src  Open readable \c QIODevice.  Not owned by \c QFtpCompat.
     * \param type Transfer type.
     * \return Command ID.
     *
     * \note All data is read from \p src into an internal buffer at command
     *       start.  For streaming large files, consider pre-loading the data
     *       and using the \c QByteArray overload instead.
     */
    int put(const QUrl &url, QIODevice *src,
            TransferType type = TransferType::Binary);

    // -----------------------------------------------------------------
    // Directory / file-management commands
    // -----------------------------------------------------------------

    /**
     * \brief Requests a directory listing (LIST).
     *
     * Each parsed entry is emitted via \ref listInfo before
     * \ref commandFinished is emitted.
     *
     * \param url URL whose path is the directory to list.
     *            Use the root or an empty path to list the current directory.
     * \return Command ID.
     */
    int list(const QUrl &url);

    /**
     * \brief Creates a directory on the server (MKD).
     * \param url URL whose path is the directory to create.
     * \return Command ID.
     */
    int mkdir(const QUrl &url);

    /**
     * \brief Removes an empty directory on the server (RMD).
     * \param url URL whose path is the directory to remove.
     * \return Command ID.
     */
    int rmdir(const QUrl &url);

    /**
     * \brief Deletes a file on the server (DELE).
     * \param url URL whose path is the file to delete.
     * \return Command ID.
     */
    int remove(const QUrl &url);

    /**
     * \brief Renames or moves a file on the server (RNFR + RNTO).
     *
     * \param from URL whose path is the current file location.
     * \param to   URL whose path is the new file location.
     * \return Command ID.
     */
    int rename(const QUrl &from, const QUrl &to);

    /**
     * \brief Changes the current working directory (CWD).
     * \param url URL whose path is the target directory.
     * \return Command ID.
     */
    int cd(const QUrl &url);

    /**
     * \brief Requests the current working directory path (PWD).
     *
     * The path is returned via \ref rawCommandReply with code 257.
     * \return Command ID.
     */
    int pwd();

    /**
     * \brief Sends an arbitrary FTP command on the control channel.
     *
     * The server response is emitted via \ref rawCommandReply.
     * \param command Raw FTP command string (without CRLF).
     * \return Command ID.
     */
    int rawCommand(const QString &command);

    // -----------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------

    /// Current connection state.
    [[nodiscard]] State  state()       const noexcept;
    /// Error code set by the most recent failed command.
    [[nodiscard]] Error  error()       const noexcept;
    /// Verbose, human-readable description of the last error.
    [[nodiscard]] QString errorString() const noexcept;
    /// ID of the command currently being executed, or \c -1 if idle.
    [[nodiscard]] int    currentId()   const noexcept;
    /// \c true if commands are waiting in the queue.
    [[nodiscard]] bool   hasPendingCommands() const noexcept;

    /**
     * \brief Removes all pending (not yet started) commands from the queue.
     *
     * The currently executing command is not affected.
     */
    // -----------------------------------------------------------------
    // TLS / SSL
    // -----------------------------------------------------------------

    /**
     * \brief Sets the SSL configuration used for both control and data channels.
     *
     * Call before \ref open() to customise certificates, peer-verify mode, etc.
     * The default configuration uses \c QSslSocket::VerifyPeer.
     *
     * \par Embedded / self-signed servers
     * \code
     * QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
     * cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
     * ftp->setSslConfiguration(cfg);
     * \endcode
     */
    void setSslConfiguration(const QSslConfiguration &config);

    /// Returns the current SSL configuration.
    [[nodiscard]] QSslConfiguration sslConfiguration() const;

    /// Instructs both sockets to ignore all pending SSL errors.
    /// Call from a \ref sslErrors handler to accept self-signed certificates.
    void ignoreSslErrors();

    void clearPendingCommands();

    /// Hard-resets the FTP session: clears queue, aborts TCP sockets, sets
    /// state to Unconnected without sending QUIT. Safe to call in any state.
    void resetConnection();

signals:
    /**
     * \brief Emitted when the connection state changes.
     * \param newState New connection state.
     */
    void stateChanged(QFtpCompat::State newState);

    /**
     * \brief Emitted when a queued command begins executing.
     * \param id Command ID.
     */
    void commandStarted(int id);

    /**
     * \brief Emitted when a command completes (successfully or with error).
     *
     * \param id    Command ID (matches the return value of the enqueue method).
     * \param error \c true if the command failed; inspect \ref errorString() for details.
     */
    void commandFinished(int id, bool error);

    /**
     * \brief Emitted when all queued commands have completed.
     * \param error \c true if the last command in the queue failed.
     */
    void done(bool error);

    /**
     * \brief Emitted once per entry during a \ref list() command.
     * \param entry Parsed directory entry.
     */
    void listInfo(QFtpCompatDirEntry entry);

    /**
     * \brief Emitted periodically during data transfers.
     * \param bytesTransferred Bytes transferred so far.
     * \param bytesTotal       Total bytes to transfer, or \c -1 if unknown.
     */
    void dataTransferProgress(qint64 bytesTransferred, qint64 bytesTotal);

    /**
     * \brief Emitted for \ref rawCommand() responses and \ref pwd() results.
     * \param code   FTP response code (e.g. 257 for PWD).
     * \param detail Human-readable message text from the server.
     */
    void rawCommandReply(int code, QString detail);

    /**
     * \brief Emitted when SSL/TLS errors occur.
     *
     * To accept self-signed certificates (common on embedded servers):
     * \code
     * connect(ftp, &QFtpCompat::sslErrors, ftp, &QFtpCompat::ignoreSslErrors);
     * \endcode
     */
    void sslErrors(const QList<QSslError> &errors);

private slots:
    void onControlConnected();
    void onControlReadyRead();
    void onControlError(QAbstractSocket::SocketError socketError);

    void onDataConnected();
    void onDataReadyRead();
    void onDataDisconnected();
    void onDataError(QAbstractSocket::SocketError socketError);
    void onTransferCompleted();

private:
    // -----------------------------------------------------------------
    // Internal state machine
    // -----------------------------------------------------------------
    enum class ControlState {
        Idle,
        WaitingForWelcome,
        WaitingForUserAck,
        WaitingForPassAck,
        WaitingForTypeAck,
        WaitingForPasvAck,
        ConnectingData,
        WaitingForTransferStartAck,
        Transferring,
        WaitingForTransferCompleteAck,
        WaitingForSimpleAck,    ///< DELE / MKD / RMD / CWD / PWD
        WaitingForRnfrAck,
        WaitingForRntoAck,
        WaitingForAbortAck,
        WaitingForQuitAck,
        // TLS-specific states
        WaitingForAuthTlsAck,       ///< AUTH TLS sent, waiting for 234
        WaitingForPbszAck,          ///< PBSZ 0 sent, waiting for 200
        WaitingForProtAck,          ///< PROT P sent, waiting for 200
        WaitingForControlHandshake, ///< startClientEncryption() called on control socket
        WaitingForDataHandshake,    ///< startClientEncryption() called on data socket
    };

    // -----------------------------------------------------------------
    // Command queue management
    // -----------------------------------------------------------------
    int  enqueue(QFtpCompatInternal::Command *cmd);
    void startNextCommand();
    void startCurrentCommand();

    void finalizeCommand(bool hadError);

    // -----------------------------------------------------------------
    // FTP protocol helpers
    // -----------------------------------------------------------------
    void sendCommand(const QByteArray &cmd);
    void processResponse(int code, const QByteArray &message);

    void beginDataTransfer();       ///< Sends TYPE then PASV
    void sendTypeCommand();
    void sendPasv();
    void sendDataTransferCommand(); ///< Sends RETR / STOR / LIST after data channel is ready
    void startDataChannelEncryption(); ///< Calls startClientEncryption on data socket

    [[nodiscard]] bool parsePasvAddress(const QByteArray &msg,
                                        QString &host, quint16 &port) const;

    void setError(Error err, const QString &detail);
    void setState(State s);

    [[nodiscard]] bool isDataTransferCommand() const noexcept;

    // -----------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------
    QSslSocket *m_control{nullptr};
    QSslSocket *m_data{nullptr};

    EncryptionMode  m_encMode{EncryptionMode::None};
    QSslConfiguration m_sslConfig{QSslConfiguration::defaultConfiguration()};

    QByteArray m_dataReadBuf;     ///< Accumulates download payload

    State        m_state{State::Unconnected};
    ControlState m_ctrlState{ControlState::Idle};
    Error        m_error{Error::NoError};
    QString      m_errorString;

    QString  m_host;
    quint16  m_port{21};

    // Current command
    QFtpCompatInternal::Command *m_current{nullptr};
    QQueue<QFtpCompatInternal::Command *> m_queue;

    int m_nextId{0};

    // Data-channel completion flags (see DownloadManager design notes)
    bool m_dataChannelClosed{false};
    bool m_ctrlTransferComplete{false};

    bool m_abortPending{false};

    qint64 m_downloadReceived{0};   ///< Bytes received so far (download).
    qint64 m_uploadTotal{0};        ///< Total bytes to upload.
    qint64 m_uploadTransferred{0};  ///< Bytes written to data socket so far.
};
