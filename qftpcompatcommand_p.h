// SPDX-License-Identifier: MIT
// Internal – do not include in public headers.

#pragma once

#include "qftpcompat.h"

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QUrl>

namespace QFtpCompatInternal {

/**
 * \internal
 * Represents a single FTP operation queued inside QFtpCompat.
 * All members are intentionally public – this struct is only visible
 * inside the library's own translation unit.
 */
struct Command
{
    enum class Type {
        Open,    ///< TCP connect + USER + PASS (uses url for host + credentials)
        List,    ///< LIST or NLST
        Get,     ///< RETR
        Put,     ///< STOR
        Remove,  ///< DELE
        Mkdir,   ///< MKD
        Rmdir,   ///< RMD
        Rename,  ///< RNFR + RNTO
        Cd,      ///< CWD
        Pwd,     ///< PWD
        Raw,     ///< arbitrary command string
        Close,   ///< QUIT
    };

    int  id{-1};
    Type type{Type::Raw};

    QUrl    url;          ///< Full URL for Open; path-only for transfer commands
    QString renameDst;    ///< RNTO destination path (Rename only)
    QString rawCmd;       ///< Raw FTP command string (Raw only)

    QIODevice  *device{nullptr};   ///< Download target or upload source device
    QByteArray *byteArrayDst{nullptr}; ///< Download target buffer (Get overload)
    bool        ownsDevice{false}; ///< true when we created device (e.g. QBuffer for QByteArray PUT)

    QFtpCompat::TransferType   transferType{QFtpCompat::TransferType::Binary};
    QFtpCompat::EncryptionMode encMode{QFtpCompat::EncryptionMode::None};
};

} // namespace QFtpCompatInternal
