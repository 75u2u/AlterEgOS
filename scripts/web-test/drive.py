#!/usr/bin/env python3
"""Drive alterego-web through a pseudo-terminal (80x24) the way the
desktop's terminal does, and check what it draws:

    drive.py ./src/web/alterego-web http://127.0.0.1:18080/

Opens the page, follows link 1 by number, goes back, types a URL after o,
and quits. Exits 1 with the screen text when something is missing.
"""
import fcntl, os, pty, re, select, struct, sys, termios, time

web, url = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.execv(web, [web, url])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))


def drain(seconds=1.0):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                out += os.read(fd, 65536)
            except OSError:
                break
    return re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", out).decode("utf-8", "replace")


fails = 0


def expect(screen, *wants):
    global fails
    for w in wants:
        if w not in screen:
            fails += 1
            print("MISSING:", w)
            print(screen)


expect(drain(), "テスト ページ - " + url, "相対リンク[1]", "4 個のリンク")
os.write(fd, b"1")
expect(drain(0.5), "リンク番号: 1")
os.write(fd, b"\r")
expect(drain(), "サブページ - " + url + "sub/page.html", "second page back[1]")
os.write(fd, b"b")
expect(drain(), "テスト ページ - " + url)
os.write(fd, b"o")
os.write(fd, (url + "sub/page.html").encode())
expect(drain(0.5), "URL: " + url + "sub/page.html")
os.write(fd, b"\r")
expect(drain(), "サブページ - ")
os.write(fd, b"q")
drain(0.5)
os.waitpid(pid, 0)
if fails:
    sys.exit(1)
print("web: the pty drive passed")
