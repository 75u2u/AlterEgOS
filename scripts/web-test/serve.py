#!/usr/bin/env python3
"""Test servers for alterego-web: a plain http server and an https one
(self-signed certificate) serving the same directory, plus two special
paths: /redirect (302 to /sub/page.html) and /chunked (a chunked body).

    serve.py DIR HTTP_PORT HTTPS_PORT CERT KEY
"""
import http.server, os, ssl, sys, threading


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/sub/page.html")
            self.end_headers()
            return
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for piece in (b"chunked ", b"body ok\n"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
            return
        super().do_GET()

    def log_message(self, *args):
        pass


def main():
    directory, http_port, https_port, cert, key = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    os.chdir(directory)
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), Handler)
    secure = http.server.ThreadingHTTPServer(("127.0.0.1", https_port), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    secure.socket = ctx.wrap_socket(secure.socket, server_side=True)
    threading.Thread(target=plain.serve_forever, daemon=True).start()
    secure.serve_forever()


if __name__ == "__main__":
    main()
