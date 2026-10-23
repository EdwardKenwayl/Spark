"""Exercise the native HTTP client against an untrusted loopback TLS server."""

import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading


def main():
    client, openssl = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="spark-tls-") as directory:
        root = pathlib.Path(directory)
        certificate, key = root / "certificate.pem", root / "key.pem"
        subprocess.run(
            [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=localhost", "-keyout", str(key), "-out", str(certificate)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10,
        )
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(8)

            def serve():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(5)
                        try:
                            with context.wrap_socket(connection, server_side=True):
                                errors.append("Client accepted the untrusted certificate")
                        except ssl.SSLError:
                            pass  # The client aborts the handshake on verification failure.
                except Exception as error:
                    errors.append(str(error))

            worker = threading.Thread(target=serve)
            worker.start()
            try:
                response = subprocess.run(
                    [client, "--reject-untrusted-tls",
                     f"https://127.0.0.1:{listener.getsockname()[1]}/"],
                    check=False, timeout=8,
                )
            finally:
                worker.join(timeout=9)
            if worker.is_alive() or errors:
                raise RuntimeError(f"TLS fixture failed: {errors}")
            return response.returncode


if __name__ == "__main__":
    sys.exit(main())
