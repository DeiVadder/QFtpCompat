"""
Explicit FTPS test server for QFtpCompat.

Runs on port 2122 alongside the plain FTP server (port 2121).
Requires a self-signed certificate in the same directory:

    openssl req -x509 -newkey rsa:2048 \
        -keyout key.pem -out cert.pem \
        -days 365 -nodes -subj "/CN=localhost"

Usage (run from the ftproot directory):
    python3 /path/to/test/server_tls.py [ftproot_dir]

If no argument is given, the current working directory is used as the FTP root.
"""
import os, sys
from pyftpdlib.handlers    import TLS_FTPHandler
from pyftpdlib.servers     import FTPServer
from pyftpdlib.authorizers import DummyAuthorizer

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
CERT = os.path.join(ROOT, "cert.pem")
KEY  = os.path.join(ROOT, "key.pem")

if not os.path.exists(CERT) or not os.path.exists(KEY):
    print(f"ERROR: cert.pem / key.pem not found in {ROOT}")
    print()
    print("Generate with:")
    print(f"  cd {ROOT}")
    print("  openssl req -x509 -newkey rsa:2048 \\")
    print("      -keyout key.pem -out cert.pem \\")
    print("      -days 365 -nodes -subj \"/CN=localhost\"")
    sys.exit(1)

authorizer = DummyAuthorizer()
authorizer.add_user("root", "root", ROOT, perm="elradfmwMT")

handler                      = TLS_FTPHandler
handler.certfile             = CERT
handler.keyfile              = KEY
handler.authorizer           = authorizer
handler.passive_ports        = range(60000, 60100)
handler.tls_control_required = True
handler.tls_data_required    = True

server = FTPServer(("127.0.0.1", 2122), handler)
print(f"Explicit FTPS server  127.0.0.1:2122  root: {ROOT}")
print("Press Ctrl+C to stop.")
server.serve_forever()
