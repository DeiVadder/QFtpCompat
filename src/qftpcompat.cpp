// SPDX-License-Identifier: MIT

#include "qftpcompat.h"
#include "private/qftpcompatcommand_p.h"

#include <QBuffer>
#include <QDebug>
#include <QQueue>
#include <QSslSocket>
#include <QTimer>

using Cmd = QFtpCompatInternal::Command;

static constexpr quint16 DefaultFtpPort  = 21;
static constexpr int     InactivityMs    = 30000;
static const    QString  AnonymousUser   = QStringLiteral("anonymous");
static const    QString  AnonymousPass   = QStringLiteral("anonymous@");

// =============================================================================
// Construction / Destruction
// =============================================================================

QFtpCompat::QFtpCompat(QObject *parent)
    : QObject(parent)
    , m_control(new QSslSocket(this))
    , m_data(new QSslSocket(this))
{
    qDebug() << Q_FUNC_INFO << "created";
    // Control channel
    connect(m_control, &QAbstractSocket::connected,
            this, &QFtpCompat::onControlConnected);
    connect(m_control, &QAbstractSocket::readyRead,
            this, &QFtpCompat::onControlReadyRead);
    connect(m_control, &QAbstractSocket::errorOccurred,
            this, &QFtpCompat::onControlError);

    // Control channel – SSL
    connect(m_control, &QSslSocket::encrypted, this, [this]() {
        qDebug() << Q_FUNC_INFO << "control channel encrypted";
        if (m_ctrlState == ControlState::WaitingForControlHandshake) {
            if (m_encMode == EncryptionMode::Implicit) {
                // Implicit: TLS done, now expect the 220 banner
                m_ctrlState = ControlState::WaitingForWelcome;
            } else {
                // Explicit: TLS done after AUTH TLS, send PBSZ 0
                sendCommand("PBSZ 0");
                m_ctrlState = ControlState::WaitingForPbszAck;
            }
        }
    });
    connect(m_control, &QSslSocket::sslErrors, this,
            [this](const QList<QSslError> &errs) {
                qDebug() << Q_FUNC_INFO << "control SSL errors:" << errs;
                emit sslErrors(errs);
            });

    // Data channel
    connect(m_data, &QAbstractSocket::connected,    this, &QFtpCompat::onDataConnected);
    connect(m_data, &QAbstractSocket::readyRead,    this, &QFtpCompat::onDataReadyRead);
    connect(m_data, &QAbstractSocket::disconnected, this, &QFtpCompat::onDataDisconnected);
    connect(m_data, &QAbstractSocket::errorOccurred,this, &QFtpCompat::onDataError);

    // Data channel – SSL
    connect(m_data, &QSslSocket::encrypted, this, [this]() {
        qDebug() << Q_FUNC_INFO << "data channel encrypted";
        if (m_ctrlState == ControlState::WaitingForDataHandshake) {
            m_ctrlState = ControlState::WaitingForTransferStartAck;
            sendDataTransferCommand();
        }
    });
    connect(m_data, &QSslSocket::sslErrors, this,
            [this](const QList<QSslError> &errs) {
                qDebug() << Q_FUNC_INFO << "data SSL errors:" << errs;
                emit sslErrors(errs);
            });
}

QFtpCompat::~QFtpCompat()
{
    qDebug() << Q_FUNC_INFO << "destroyed";
    qDeleteAll(m_queue);
    delete m_current;
}

// =============================================================================
// Public API – connection
// =============================================================================

int QFtpCompat::open(const QUrl &url, EncryptionMode mode)
{
    qDebug() << Q_FUNC_INFO << url.toString() << "encryption:" << static_cast<int>(mode);
    auto *cmd    = new Cmd;
    cmd->type    = Cmd::Type::Open;
    cmd->url     = url;
    cmd->encMode = mode;
    return enqueue(cmd);
}

void QFtpCompat::setSslConfiguration(const QSslConfiguration &config)
{
    m_sslConfig = config;
    m_control->setSslConfiguration(config);
    m_data->setSslConfiguration(config);
}

QSslConfiguration QFtpCompat::sslConfiguration() const
{
    return m_sslConfig;
}

void QFtpCompat::ignoreSslErrors()
{
    m_control->ignoreSslErrors();
    m_data->ignoreSslErrors();
}

void QFtpCompat::close()
{
    qDebug() << Q_FUNC_INFO;
    clearPendingCommands();

    auto *cmd = new Cmd;
    cmd->type = Cmd::Type::Close;
    enqueue(cmd);
}

void QFtpCompat::abort()
{
    if (!m_current) {
        return;
    }

    qDebug() << Q_FUNC_INFO << "aborting command" << m_current->id;

    m_abortPending = true;

    if (isDataTransferCommand()) {
        // RFC 959: send ABOR on the control channel; the server responds
        // with 426 (transfer aborted) followed by 226 (ABOR successful).
        // Abort the data socket to stop the transfer immediately.
        sendCommand("ABOR");
        m_data->abort();
        m_ctrlState = ControlState::WaitingForAbortAck;
    } else {
        // No data channel active – mark the current command as aborted
        // and move on.
        setError(Error::Aborted,
                 QStringLiteral("Operation aborted by the user (command id %1)")
                     .arg(m_current->id));
        finalizeCommand(true);
    }
}

// =============================================================================
// Public API – transfer commands
// =============================================================================

int QFtpCompat::get(const QUrl &url, QIODevice *dst, TransferType type)
{
    qDebug() << Q_FUNC_INFO << url.path();
    auto *cmd          = new Cmd;
    cmd->type          = Cmd::Type::Get;
    cmd->url           = url;
    cmd->device        = dst;
    cmd->transferType  = type;
    return enqueue(cmd);
}

int QFtpCompat::get(const QUrl &url, QByteArray *dst, TransferType type)
{
    qDebug() << Q_FUNC_INFO << url.path() << "(→ QByteArray)";
    // Wrap the QByteArray in a QBuffer so the rest of the code stays uniform.
    auto *buf = new QBuffer;
    buf->open(QIODevice::WriteOnly);

    auto *cmd             = new Cmd;
    cmd->type             = Cmd::Type::Get;
    cmd->url              = url;
    cmd->device           = buf;
    cmd->byteArrayDst     = dst;
    cmd->ownsDevice       = true;    // we created the QBuffer; we must delete it
    cmd->transferType     = type;
    return enqueue(cmd);
}

int QFtpCompat::put(const QUrl &url, const QByteArray &data, TransferType type)
{
    qDebug() << Q_FUNC_INFO << url.path() << "size:" << data.size();
    auto *buf = new QBuffer;
    buf->setData(data);
    buf->open(QIODevice::ReadOnly);

    auto *cmd          = new Cmd;
    cmd->type          = Cmd::Type::Put;
    cmd->url           = url;
    cmd->device        = buf;
    cmd->ownsDevice    = true;
    cmd->transferType  = type;
    return enqueue(cmd);
}

int QFtpCompat::put(const QUrl &url, QIODevice *src, TransferType type)
{
    qDebug() << Q_FUNC_INFO << url.path()
    << "size:" << (src ? src->size() : -1);
    auto *cmd          = new Cmd;
    cmd->type          = Cmd::Type::Put;
    cmd->url           = url;
    cmd->device        = src;
    cmd->transferType  = type;
    return enqueue(cmd);
}

// =============================================================================
// Public API – directory / management commands
// =============================================================================

int QFtpCompat::list(const QUrl &url)
{
    qDebug() << Q_FUNC_INFO << url.path();
    auto *cmd = new Cmd;
    cmd->type = Cmd::Type::List;
    cmd->url  = url;
    return enqueue(cmd);
}

int QFtpCompat::mkdir(const QUrl &url)
{
    qDebug() << Q_FUNC_INFO << url.path();
    auto *cmd = new Cmd;
    cmd->type = Cmd::Type::Mkdir;
    cmd->url  = url;
    return enqueue(cmd);
}

int QFtpCompat::rmdir(const QUrl &url)
{
    qDebug() << Q_FUNC_INFO << url.path();
    auto *cmd = new Cmd;
    cmd->type = Cmd::Type::Rmdir;
    cmd->url  = url;
    return enqueue(cmd);
}

int QFtpCompat::remove(const QUrl &url)
{
    qDebug() << Q_FUNC_INFO << url.path();
    auto *cmd = new Cmd;
    cmd->type = Cmd::Type::Remove;
    cmd->url  = url;
    return enqueue(cmd);
}

int QFtpCompat::rename(const QUrl &from, const QUrl &to)
{
    qDebug() << Q_FUNC_INFO << from.path() << "->" << to.path();
    auto *cmd        = new Cmd;
    cmd->type        = Cmd::Type::Rename;
    cmd->url         = from;
    cmd->renameDst   = to.path();
    return enqueue(cmd);
}

int QFtpCompat::cd(const QUrl &url)
{
    qDebug() << Q_FUNC_INFO << url.path();
    auto *cmd = new Cmd;
    cmd->type = Cmd::Type::Cd;
    cmd->url  = url;
    return enqueue(cmd);
}

int QFtpCompat::pwd()
{
    auto *cmd   = new Cmd;
    cmd->type   = Cmd::Type::Pwd;
    cmd->rawCmd = QStringLiteral("PWD");
    return enqueue(cmd);
}

int QFtpCompat::rawCommand(const QString &command)
{
    qDebug() << Q_FUNC_INFO << command;
    auto *cmd   = new Cmd;
    cmd->type   = Cmd::Type::Raw;
    cmd->rawCmd = command;
    return enqueue(cmd);
}

// =============================================================================
// Public API – state queries
// =============================================================================

QFtpCompat::State  QFtpCompat::state()       const noexcept { return m_state; }
QFtpCompat::Error  QFtpCompat::error()       const noexcept { return m_error; }
QString            QFtpCompat::errorString()  const noexcept { return m_errorString; }
int                QFtpCompat::currentId()    const noexcept { return m_current ? m_current->id : -1; }
bool               QFtpCompat::hasPendingCommands() const noexcept { return !m_queue.isEmpty(); }

void QFtpCompat::resetConnection()
{
    qDebug() << Q_FUNC_INFO;
    clearPendingCommands();

    // Delete current command without emitting commandFinished
    delete m_current;
    m_current = nullptr;

    // Set Unconnected BEFORE aborting sockets so that onControlError()
    // sees the Unconnected state and returns early without re-finalizing.
    setState(State::Unconnected);
    m_ctrlState   = ControlState::Idle;
    m_abortPending = false;

    // Hard-close both channels (no QUIT, immediate TCP RST)
    m_data->abort();
    m_control->abort();
    m_encMode = EncryptionMode::None;
}

void QFtpCompat::clearPendingCommands()
{
    qDebug() << Q_FUNC_INFO << "clearing" << m_queue.size() << "pending commands";
    qDeleteAll(m_queue);
    m_queue.clear();
}

// =============================================================================
// Command queue management
// =============================================================================

int QFtpCompat::enqueue(Cmd *cmd)
{
    cmd->id = m_nextId++;
    qDebug() << Q_FUNC_INFO << "id:" << cmd->id
             << "type:" << static_cast<int>(cmd->type)
             << "queue depth:" << m_queue.size() + 1;
    m_queue.enqueue(cmd);

    // If nothing is currently executing, kick off the queue
    if (!m_current) {
        QMetaObject::invokeMethod(this, &QFtpCompat::startNextCommand,
                                  Qt::QueuedConnection);
    }
    return cmd->id;
}

void QFtpCompat::startNextCommand()
{
    qDebug() << Q_FUNC_INFO << "queue:" << m_queue.size();
    if (m_current || m_queue.isEmpty()) {
        return;
    }

    m_current = m_queue.dequeue();

    // Reset per-command transfer flags
    m_dataChannelClosed     = false;
    m_ctrlTransferComplete  = false;
    m_abortPending          = false;
    m_dataReadBuf.clear();

    emit commandStarted(m_current->id);
    startCurrentCommand();
}

void QFtpCompat::startCurrentCommand()
{
    Q_ASSERT(m_current);

    // All commands except Open require an active login.
    // Fail fast instead of writing to a closed socket and hanging.
    if (m_current->type != Cmd::Type::Open
        && m_current->type != Cmd::Type::Close
        && m_state != State::LoggedIn) {
        qWarning() << Q_FUNC_INFO
                   << "not logged in – failing command"
                   << static_cast<int>(m_current->type);
        setError(Error::NotLoggedIn,
                 QStringLiteral("command %1 requires login")
                     .arg(static_cast<int>(m_current->type)));
        finalizeCommand(true);
        return;
    }

    switch (m_current->type) {

    case Cmd::Type::Open: {
        if (m_state == State::LoggedIn) {
            finalizeCommand(false);
            return;
        }
        if (m_state == State::Unconnected) {
            const QUrl &url = m_current->url;
            m_encMode = m_current->encMode;
            m_host = url.host();

            // Default port: 990 for Implicit FTPS, 21 for everything else
            constexpr quint16 ImplicitFtpsPort = 990;
            m_port = (url.port() > 0)
                         ? static_cast<quint16>(url.port())
                         : (m_encMode == EncryptionMode::Implicit ? ImplicitFtpsPort
                                                                  : DefaultFtpPort);

            if (m_host.trimmed().isEmpty()) {
                setError(Error::HostNotFound,
                         QStringLiteral("open(): URL contains no host name ('%1')")
                             .arg(url.toString()));
                finalizeCommand(true);
                return;
            }

            // Apply SSL configuration to both sockets
            m_control->setSslConfiguration(m_sslConfig);
            m_data->setSslConfiguration(m_sslConfig);

            qDebug() << Q_FUNC_INFO
                     << "Connecting to"
                     << m_host << ":" << m_port
                     << "encryption:" << static_cast<int>(m_encMode);

            setState(State::Connecting);

            if (m_encMode == EncryptionMode::Implicit) {
                // Implicit FTPS: TLS from the first byte
                // Wait for encrypted() signal before expecting 220 banner
                m_ctrlState = ControlState::WaitingForControlHandshake;
                m_control->connectToHostEncrypted(m_host, m_port);
            } else {
                // Plain FTP or Explicit FTPS: plain TCP connect first
                m_ctrlState = ControlState::WaitingForWelcome;
                m_control->connectToHost(m_host, m_port);
            }
            return;
        }
        setError(Error::NetworkError,
                 QStringLiteral("open(): called in unexpected state %1")
                     .arg(static_cast<int>(m_state)));
        finalizeCommand(true);
        return;
    }

    case Cmd::Type::List:
    case Cmd::Type::Get:
    case Cmd::Type::Put:
        if (m_state != State::LoggedIn) {
            setError(Error::NotLoggedIn,
                     QStringLiteral("FTP %1: cannot execute – not logged in")
                         .arg(m_current->type == Cmd::Type::List ? QLatin1String("LIST")
                              : m_current->type == Cmd::Type::Get ? QLatin1String("RETR")
                                                                  : QLatin1String("STOR")));
            finalizeCommand(true);
            return;
        }
        beginDataTransfer();
        return;

    case Cmd::Type::Remove:
        sendCommand("DELE " + m_current->url.path().toUtf8());
        m_ctrlState = ControlState::WaitingForSimpleAck;
        return;

    case Cmd::Type::Mkdir:
        sendCommand("MKD " + m_current->url.path().toUtf8());
        m_ctrlState = ControlState::WaitingForSimpleAck;
        return;

    case Cmd::Type::Rmdir:
        sendCommand("RMD " + m_current->url.path().toUtf8());
        m_ctrlState = ControlState::WaitingForSimpleAck;
        return;

    case Cmd::Type::Rename:
        sendCommand("RNFR " + m_current->url.path().toUtf8());
        m_ctrlState = ControlState::WaitingForRnfrAck;
        return;

    case Cmd::Type::Cd:
        sendCommand("CWD " + m_current->url.path().toUtf8());
        m_ctrlState = ControlState::WaitingForSimpleAck;
        return;

    case Cmd::Type::Pwd:
    case Cmd::Type::Raw:
        sendCommand(m_current->rawCmd.toUtf8());
        m_ctrlState = ControlState::WaitingForSimpleAck;
        return;

    case Cmd::Type::Close:
        setState(State::Closing);
        sendCommand("QUIT");
        m_ctrlState = ControlState::WaitingForQuitAck;
        return;
    }
}

void QFtpCompat::finalizeCommand(bool hadError)
{
    if (!m_current) {
        return;
    }

    const int id = m_current->id;
    qDebug() << Q_FUNC_INFO << "id:" << id << "error:" << hadError;

    // Clean up owned device
    if (m_current->ownsDevice && m_current->device) {
        // For get-to-QByteArray: flush QBuffer → QByteArray
        if (m_current->byteArrayDst) {
            auto *buf = qobject_cast<QBuffer *>(m_current->device);
            if (buf) {
                *m_current->byteArrayDst = buf->data();
            }
        }
        delete m_current->device;
        m_current->device = nullptr;
    }

    delete m_current;
    m_current = nullptr;

    emit commandFinished(id, hadError);

    if (m_queue.isEmpty()) {
        emit done(hadError);
    } else {
        QMetaObject::invokeMethod(this, &QFtpCompat::startNextCommand,
                                  Qt::QueuedConnection);
    }
}

// =============================================================================
// Control channel
// =============================================================================

void QFtpCompat::sendCommand(const QByteArray &cmd)
{
    if (cmd.startsWith("PASS")) {
        qDebug() << Q_FUNC_INFO << "PASS ****";
    } else {
        qDebug() << Q_FUNC_INFO << cmd;
    }
    m_control->write(cmd + "\r\n");
}

void QFtpCompat::onControlConnected()
{
    qDebug() << Q_FUNC_INFO;
    setState(State::Connected);
    // Welcome banner (220) arrives via onControlReadyRead → processResponse
}

void QFtpCompat::onControlReadyRead()
{
    // canReadLine() returns true only when a complete line (ending with '\n')
    // is available in Qt's internal socket buffer.  Qt handles partial-line
    // buffering internally, so no manual accumulation buffer is needed here.
    while (m_control->canReadLine()) {
        const QByteArray line = m_control->readLine().trimmed();

        if (line.size() < 3) {
            continue;
        }

        // Multi-line response continuation: "NNN-text" – skip until "NNN text"
        if (line.size() >= 4 && line.at(3) == '-') {
            qDebug() << Q_FUNC_INFO << "[multi-line]" << line;
            continue;
        }

        bool ok{false};
        const int code = line.left(3).toInt(&ok);
        if (!ok) {
            qDebug() << Q_FUNC_INFO << "Unparseable control line:" << line;
            continue;
        }

        processResponse(code, line.size() > 4 ? line.mid(4) : QByteArray{});
    }
}

void QFtpCompat::processResponse(int code, const QByteArray &message)
{
    qDebug() << Q_FUNC_INFO << code << message;

    // Server-side errors in any state → abort current command.
    // 421 = service unavailable (timeout/shutdown).
    if (code >= 500) {
        // Switch to Idle BEFORE aborting the data socket so that any
        // synchronous onDataDisconnected / onDataError fired by abort()
        // is filtered by the state guard and does not corrupt state.
        m_ctrlState = ControlState::Idle;
        if (m_data->state() != QAbstractSocket::UnconnectedState) {
            m_data->abort();
        }
        const QString detail = QStringLiteral("server returned error %1: '%2'")
                                   .arg(code)
                                   .arg(QString::fromUtf8(message));
        setError(code == 530 ? Error::LoginFailed
                 : code == 550 ? Error::FileNotFound
                 : code == 553 ? Error::PermissionDenied
                               : Error::ServerError,
                 detail);
        finalizeCommand(true);
        return;
    }
    if (code == 421) {
        setError(Error::NetworkTimeout,
                 QStringLiteral("server closed connection (421): '%1'")
                     .arg(QString::fromUtf8(message)));
        finalizeCommand(true);
        m_control->abort();
        setState(State::Unconnected);
        return;
    }

    switch (m_ctrlState) {

    // ------------------------------------------------------------------
    // Open: waiting for server greeting
    // ------------------------------------------------------------------
    case ControlState::WaitingForWelcome:
        if (code == 220) {
            setState(State::Connected);
            if (m_encMode == EncryptionMode::Explicit) {
                // Explicit FTPS: upgrade the control channel to TLS first
                sendCommand("AUTH TLS");
                m_ctrlState = ControlState::WaitingForAuthTlsAck;
            } else {
                const QUrl &url = m_current->url;
                const QString user = url.userName().isEmpty() ? AnonymousUser : url.userName();
                sendCommand("USER " + user.toUtf8());
                m_ctrlState = ControlState::WaitingForUserAck;
            }
        } else {
            setError(Error::ServerError,
                     QStringLiteral("open(): expected 220 welcome banner, got %1: '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    case ControlState::WaitingForUserAck:
        if (code == 331) {
            // Password required (normal)
            const QUrl &url = m_current->url;
            const QString pass = url.password().isEmpty() ? AnonymousPass : url.password();
            sendCommand("PASS " + pass.toUtf8());
            m_ctrlState = ControlState::WaitingForPassAck;
        } else if (code == 230) {
            // Server accepted without password
            setState(State::LoggedIn);
            finalizeCommand(false);
        } else {
            setError(Error::LoginFailed,
                     QStringLiteral("open(): USER rejected (code %1): '%2' – check username")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    case ControlState::WaitingForPassAck:
        if (code == 230) {
            setState(State::LoggedIn);
            finalizeCommand(false);
        } else if (code == 332) {
            // ACCT required (rare)
            setError(Error::LoginFailed,
                     QStringLiteral("open(): server requires ACCOUNT (332), which is not supported"));
            finalizeCommand(true);
        } else {
            setError(Error::LoginFailed,
                     QStringLiteral("open(): authentication failed (code %1): '%2' – check password")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // Data-transfer preamble: TYPE response
    // ------------------------------------------------------------------
    case ControlState::WaitingForTypeAck:
        // RFC 959: 200 = command OK; some embedded servers respond 250
        if (code == 200 || code == 250) {
            sendPasv();
        } else {
            setError(Error::ServerError,
                     QStringLiteral("TYPE command rejected (code %1): '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // Data-transfer preamble: PASV response
    // ------------------------------------------------------------------
    case ControlState::WaitingForPasvAck: {
        if (code != 227) {
            setError(Error::ServerError,
                     QStringLiteral("PASV rejected (code %1): '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
            return;
        }

        QString dataHost;
        quint16 dataPort{0};
        if (!parsePasvAddress(message, dataHost, dataPort)) {
            setError(Error::ParseError,
                     QStringLiteral("PASV: could not parse server address from '%1'")
                         .arg(QString::fromUtf8(message)));
            finalizeCommand(true);
            return;
        }

        // NAT fallback: some embedded FTP servers report 0.0.0.0
        if (dataHost == QLatin1String("0.0.0.0") || dataHost.isEmpty()) {
            dataHost = m_control->peerAddress().toString();
            qDebug() << Q_FUNC_INFO
                     << "PASV returned invalid host, using control peer address:" << dataHost;
        }

        if (dataPort == 0) {
            setError(Error::ParseError,
                     QStringLiteral("PASV: server returned port 0 in '%1'")
                         .arg(QString::fromUtf8(message)));
            finalizeCommand(true);
            return;
        }

        qDebug() << Q_FUNC_INFO << "Data channel:" << dataHost << ":" << dataPort;
        m_ctrlState = ControlState::ConnectingData;
        m_data->connectToHost(dataHost, dataPort);
        break;
    }

    // ConnectingData: no control-channel response expected here.
    // onDataConnected() drives the next step.
    case ControlState::ConnectingData:
        break;

    // ------------------------------------------------------------------
    // Transfer start acknowledgement (RETR / STOR / LIST)
    // ------------------------------------------------------------------
    case ControlState::WaitingForTransferStartAck:
        if (code == 125 || code == 150) {
            m_ctrlState = ControlState::Transferring;
            if (m_current && m_current->type == Cmd::Type::Put) {
                // Upload: read source into buffer and write it all to the data channel.
                // Qt buffers the write; disconnectFromHost() is deferred until sent.
                QByteArray payload;
                if (m_current->device) {
                    if (!m_current->device->isReadable()) {
                        m_current->device->open(QIODevice::ReadOnly);
                    }
                    payload = m_current->device->readAll();
                }
                qDebug() << Q_FUNC_INFO << "UP uploading:" << payload.size() << "bytes";
                emit dataTransferProgress(0, payload.size());
                m_data->write(payload);
                m_data->disconnectFromHost();
            }
            // For Get / List: data arrives via onDataReadyRead()
        } else {
            setError(Error::ServerError,
                     QStringLiteral("transfer start rejected (code %1): '%2' – "
                                    "check remote path '%3'")
                         .arg(code)
                         .arg(QString::fromUtf8(message))
                         .arg(m_current ? m_current->url.path() : QString{}));
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // Transfer completion (226 / 250)
    // Can arrive before or after onDataDisconnected().
    // ------------------------------------------------------------------
    case ControlState::Transferring:
    case ControlState::WaitingForTransferCompleteAck:
        if (code == 226 || code == 250) {
            m_ctrlTransferComplete = true;
            if (m_dataChannelClosed) {
                onTransferCompleted();
            }
        } else if (code >= 400) {
            setError(Error::TransferFailed,
                     QStringLiteral("transfer failed mid-stream (code %1): '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            m_data->abort();
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // ABOR acknowledgement
    // RFC 959: server sends 426 (connection closed, transfer aborted)
    // then 226 (abort successful).  We accept either 2xx as "done".
    // ------------------------------------------------------------------
    case ControlState::WaitingForAbortAck:
        if (code == 426) {
            // Expected "transfer aborted" – wait for the following 226
            break;
        }
        if (code == 226 || code == 225) {
            setError(Error::Aborted,
                     QStringLiteral("operation aborted by user (ABOR acknowledged)"));
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // Simple single-response commands (DELE, MKD, RMD, CWD, PWD, raw)
    // ------------------------------------------------------------------
    case ControlState::WaitingForSimpleAck:
        if (code >= 200 && code < 300) {
            if (m_current && (m_current->type == Cmd::Type::Pwd
                              || m_current->type == Cmd::Type::Raw)) {
                emit rawCommandReply(code, QString::fromUtf8(message));
            }
            finalizeCommand(false);
        } else {
            setError(Error::ServerError,
                     QStringLiteral("command rejected (code %1): '%2' – path: '%3'")
                         .arg(code)
                         .arg(QString::fromUtf8(message))
                         .arg(m_current ? m_current->url.path() : QString{}));
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // RNFR / RNTO
    // ------------------------------------------------------------------
    case ControlState::WaitingForRnfrAck:
        if (code == 350) {
            // "File exists, ready for destination name"
            if (m_current) {
                sendCommand("RNTO " + m_current->renameDst.toUtf8());
                m_ctrlState = ControlState::WaitingForRntoAck;
            }
        } else {
            setError(Error::ServerError,
                     QStringLiteral("RNFR rejected (code %1): '%2' – source path: '%3'")
                         .arg(code)
                         .arg(QString::fromUtf8(message))
                         .arg(m_current ? m_current->url.path() : QString{}));
            finalizeCommand(true);
        }
        break;

    case ControlState::WaitingForRntoAck:
        if (code == 250) {
            finalizeCommand(false);
        } else {
            setError(Error::ServerError,
                     QStringLiteral("RNTO rejected (code %1): '%2' – destination path: '%3'")
                         .arg(code)
                         .arg(QString::fromUtf8(message))
                         .arg(m_current ? m_current->renameDst : QString{}));
            finalizeCommand(true);
        }
        break;

    // ------------------------------------------------------------------
    // QUIT
    // ------------------------------------------------------------------
    case ControlState::WaitingForQuitAck:
        // 221 = service closing; accept any 2xx
        if (code >= 200 && code < 300) {
            m_control->disconnectFromHost();
            setState(State::Unconnected);
            finalizeCommand(false);
        }
        break;

    // ------------------------------------------------------------------
    // Explicit FTPS: AUTH TLS → PBSZ 0 → PROT P → USER/PASS
    // ------------------------------------------------------------------
    case ControlState::WaitingForAuthTlsAck:
        if (code == 234) {
            // Server accepted AUTH TLS – start TLS handshake on control socket
            m_ctrlState = ControlState::WaitingForControlHandshake;
            m_control->startClientEncryption();
        } else {
            setError(Error::AuthTlsRejected,
                     QStringLiteral("AUTH TLS rejected (code %1): '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    case ControlState::WaitingForPbszAck:
        if (code == 200) {
            sendCommand("PROT P"); // Protect data channel
            m_ctrlState = ControlState::WaitingForProtAck;
        } else {
            setError(Error::TlsHandshakeFailed,
                     QStringLiteral("PBSZ rejected (code %1): '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    case ControlState::WaitingForProtAck:
        if (code == 200) {
            // Data channel protection confirmed – proceed with login
            const QUrl &url = m_current->url;
            const QString user = url.userName().isEmpty() ? AnonymousUser : url.userName();
            sendCommand("USER " + user.toUtf8());
            m_ctrlState = ControlState::WaitingForUserAck;
        } else {
            setError(Error::TlsHandshakeFailed,
                     QStringLiteral("PROT P rejected (code %1): '%2'")
                         .arg(code).arg(QString::fromUtf8(message)));
            finalizeCommand(true);
        }
        break;

    case ControlState::WaitingForControlHandshake:
    case ControlState::WaitingForDataHandshake:
        // Handled by QSslSocket::encrypted() signal – server responses
        // during handshake are unexpected and ignored here.
        qDebug() << Q_FUNC_INFO << "Response during TLS handshake (ignored):" << code;
        break;

    case ControlState::Idle:
        qDebug() << Q_FUNC_INFO << "Unexpected response in Idle state:" << code << message;
        break;

    } // end switch
}

void QFtpCompat::onControlError(QAbstractSocket::SocketError socketError)
{
    qDebug() << Q_FUNC_INFO << socketError << m_control->errorString();

    if (m_state == State::Unconnected || m_ctrlState == ControlState::WaitingForQuitAck) {
        return; // normal close during QUIT
    }

    Error err;
    QString detail;

    switch (socketError) {
    case QAbstractSocket::HostNotFoundError:
        err    = Error::HostNotFound;
        detail = QStringLiteral("host '%1' not found – check the URL and network connectivity")
                     .arg(m_host);
        break;
    case QAbstractSocket::ConnectionRefusedError:
        err    = Error::ConnectionRefused;
        detail = QStringLiteral("connection refused by '%1:%2' – "
                                "verify the FTP server is running and the port is correct")
                     .arg(m_host).arg(m_port);
        break;
    case QAbstractSocket::RemoteHostClosedError:
        err    = Error::NetworkError;
        detail = QStringLiteral("server '%1' closed the control connection unexpectedly "
                                "(possible server-side timeout or crash)")
                     .arg(m_host);
        break;
    case QAbstractSocket::SocketTimeoutError:
        err    = Error::NetworkTimeout;
        detail = QStringLiteral("control connection to '%1' timed out").arg(m_host);
        break;
    default:
        err    = Error::NetworkError;
        detail = QStringLiteral("control socket error [Qt code %1]: %2")
                     .arg(static_cast<int>(socketError))
                     .arg(m_control->errorString());
        break;
    }

    setError(err, detail);
    setState(State::Unconnected);

    if (m_current) {
        finalizeCommand(true);
    }
}

// =============================================================================
// Data channel
// =============================================================================

void QFtpCompat::onDataConnected()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_current) { return; }

    if (m_encMode != EncryptionMode::None) {
        // FTPS: encrypt the data channel before sending RETR/STOR/LIST
        qDebug() << Q_FUNC_INFO << "starting data channel TLS handshake";
        m_ctrlState = ControlState::WaitingForDataHandshake;
        startDataChannelEncryption();
    } else {
        m_ctrlState = ControlState::WaitingForTransferStartAck;
        sendDataTransferCommand();
    }
}

void QFtpCompat::startDataChannelEncryption()
{
    m_data->setSslConfiguration(m_sslConfig);
    m_data->startClientEncryption();
}

void QFtpCompat::sendDataTransferCommand()
{
    if (!m_current) {
        return;
    }

    switch (m_current->type) {
    case Cmd::Type::List:
        sendCommand("LIST" + (m_current->url.path().isEmpty()
                                  ? QByteArray{}
                                  : " " + m_current->url.path().toUtf8()));
        break;
    case Cmd::Type::Get:
        sendCommand("RETR " + m_current->url.path().toUtf8());
        break;
    case Cmd::Type::Put:
        sendCommand("STOR " + m_current->url.path().toUtf8());
        break;
    default:
        break;
    }
}

void QFtpCompat::onDataReadyRead()
{
    const QByteArray chunk = m_data->readAll();

    if (!m_current) {
        return;
    }

    if (m_current->type == Cmd::Type::Get) {
        if (m_current->device) {
            m_current->device->write(chunk);
        }
        const qint64 received = m_current->device ? m_current->device->pos() : 0;
        qDebug() << Q_FUNC_INFO << "DOWN received:" << received << "bytes";
        emit dataTransferProgress(received, -1);
    } else if (m_current->type == Cmd::Type::List) {
        m_dataReadBuf += chunk;
    }
}

void QFtpCompat::onDataDisconnected()
{
    qDebug() << Q_FUNC_INFO;

    // Ignore stale disconnect events (e.g. from a 550-aborted data socket
    // firing while the next command is already in WaitingForPasvAck).
    if (m_ctrlState != ControlState::Transferring
        && m_ctrlState != ControlState::WaitingForTransferStartAck
        && m_ctrlState != ControlState::WaitingForTransferCompleteAck
        && m_ctrlState != ControlState::WaitingForAbortAck) {
        qDebug() << Q_FUNC_INFO << "stale – ignored in state"
                 << static_cast<int>(m_ctrlState);
        return;
    }

    // Drain any last bytes
    const QByteArray tail = m_data->readAll();

    if (m_current) {
        if (m_current->type == Cmd::Type::Get && m_current->device) {
            if (!tail.isEmpty()) {
                m_current->device->write(tail);
            }
        } else if (m_current->type == Cmd::Type::List) {
            m_dataReadBuf += tail;
        }
    }

    m_dataChannelClosed = true;

    if (m_ctrlTransferComplete) {
        onTransferCompleted();
    } else {
        m_ctrlState = ControlState::WaitingForTransferCompleteAck;
    }
}

void QFtpCompat::onDataError(QAbstractSocket::SocketError socketError)
{
    qDebug() << Q_FUNC_INFO << socketError;

    // RemoteHostClosedError is normal end-of-transfer – let onDataDisconnected handle it.
    if (socketError == QAbstractSocket::RemoteHostClosedError) {
        return;
    }

    // Ignore stale errors not belonging to an active data transfer.
    if (m_ctrlState != ControlState::Transferring
        && m_ctrlState != ControlState::WaitingForTransferStartAck
        && m_ctrlState != ControlState::WaitingForTransferCompleteAck
        && m_ctrlState != ControlState::ConnectingData
        && m_ctrlState != ControlState::WaitingForAbortAck) {
        qDebug() << Q_FUNC_INFO << "stale – ignored in state"
                 << static_cast<int>(m_ctrlState);
        return;
    }

    setError(Error::NetworkError,
             QStringLiteral("data channel error [Qt code %1]: %2 – "
                            "transfer of '%3' interrupted")
                 .arg(static_cast<int>(socketError))
                 .arg(m_data->errorString())
                 .arg(m_current ? m_current->url.path() : QString{}));
    finalizeCommand(true);
}

// =============================================================================
// Transfer completion (called when BOTH 226 received AND data channel closed)
// =============================================================================

void QFtpCompat::onTransferCompleted()
{
    qDebug() << Q_FUNC_INFO;
    if (!m_current) {
        return;
    }
    // Emit 100% progress on upload completion
    if (m_current->type == Cmd::Type::Put && m_uploadTotal > 0) {
        qDebug() << Q_FUNC_INFO << "↑ upload complete:" << m_uploadTotal << "bytes";
        emit dataTransferProgress(m_uploadTotal, m_uploadTotal);
    }

    if (m_current->type == Cmd::Type::List) {
        // Parse the accumulated listing and emit one signal per entry
        const auto entries = QFtpCompatDirEntry::parseList(m_dataReadBuf);
        qDebug() << Q_FUNC_INFO << "LIST: parsed" << entries.size() << "entries";
        for (const QFtpCompatDirEntry &e : entries) {
            emit listInfo(e);
        }
    }

    finalizeCommand(false);
}

// =============================================================================
// Data-transfer preamble helpers
// =============================================================================

void QFtpCompat::beginDataTransfer()
{
    qDebug() << Q_FUNC_INFO;
    sendTypeCommand();
}

void QFtpCompat::sendTypeCommand()
{
    qDebug() << Q_FUNC_INFO;
    const bool isBinary = !m_current
                          || m_current->transferType == TransferType::Binary;

    if (isBinary) {
        sendCommand("TYPE I");
    } else {
        // Ascii mode: TYPE A is sent; CRLF transformation is NOT applied
        // to the data stream in this version (see TransferType::Ascii doc).
        // TODO: implement CRLF normalisation for proper Ascii-mode transfers.
        sendCommand("TYPE A");
    }
    m_ctrlState = ControlState::WaitingForTypeAck;
}

void QFtpCompat::sendPasv()
{
    qDebug() << Q_FUNC_INFO;
    sendCommand("PASV");
    m_ctrlState = ControlState::WaitingForPasvAck;
}

// =============================================================================
// PASV address parser
// Format: "Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
// =============================================================================

bool QFtpCompat::parsePasvAddress(const QByteArray &msg,
                                  QString &host, quint16 &port) const
{
    const int open  = msg.indexOf('(');
    const int close = msg.indexOf(')');

    if (open < 0 || close <= open) {
        return false;
    }

    const QByteArray inner = msg.mid(open + 1, close - open - 1);
    const QList<QByteArray> parts = inner.split(',');

    if (parts.size() != 6) {
        return false;
    }

    bool ok{false};
    const int h1 = parts[0].trimmed().toInt(&ok); if (!ok) { return false; }
    const int h2 = parts[1].trimmed().toInt(&ok); if (!ok) { return false; }
    const int h3 = parts[2].trimmed().toInt(&ok); if (!ok) { return false; }
    const int h4 = parts[3].trimmed().toInt(&ok); if (!ok) { return false; }
    const int p1 = parts[4].trimmed().toInt(&ok); if (!ok) { return false; }
    const int p2 = parts[5].trimmed().toInt(&ok); if (!ok) { return false; }

    host = QStringLiteral("%1.%2.%3.%4").arg(h1).arg(h2).arg(h3).arg(h4);
    port = static_cast<quint16>(p1 * 256 + p2);
    qDebug() << Q_FUNC_INFO << "data channel:" << host << ":" << port;
    return true;
}

// =============================================================================
// State / error helpers
// =============================================================================

void QFtpCompat::setState(State s)
{
    if (m_state == s) {
        return;
    }
    qDebug() << Q_FUNC_INFO
             << "state:" << static_cast<int>(m_state)
             << "->" << static_cast<int>(s);
    m_state = s;
    emit stateChanged(s);
}

void QFtpCompat::setError(Error err, const QString &detail)
{
    m_error       = err;
    m_errorString = detail;
    qDebug() << Q_FUNC_INFO << detail;
}

bool QFtpCompat::isDataTransferCommand() const noexcept
{
    if (!m_current) {
        return false;
    }
    switch (m_current->type) {
    case Cmd::Type::List:
    case Cmd::Type::Get:
    case Cmd::Type::Put:
        return true;
    default:
        return false;
    }
}
