#!/usr/bin/env python3
"""Serves the daemon's built-in test film page to an ordinary browser.

The daemon's page (src/daemon/cef_host/test_film_page.hpp) only exists inside
the daemon, at https://player.invalid. This copies it out of that file and
serves it on http://localhost:8000, so a browser such as Helium can run the
exact same page against the same license server, and its console shows the
same "license request: ..." lines to compare with the daemon's (issue #21).
Browsers only allow DRM on secure origins, and localhost counts as one.

    python3 tools/test_page/serve.py

then open http://localhost:8000/test-film.html (add ?clear for the
unencrypted copy). Ctrl-C stops it.
"""

import http.server
import pathlib
import tempfile

PAGE = pathlib.Path(__file__).resolve().parents[2] / "src/daemon/cef_host/test_film_page.hpp"
PORT = 8000


def main():
    source = PAGE.read_text()
    html = source.split('R"html(', 1)[1].split(')html"', 1)[0]
    folder = pathlib.Path(tempfile.mkdtemp(prefix="scratter-page-"))
    (folder / "test-film.html").write_text(html)

    def handler(*args, **kwargs):
        return http.server.SimpleHTTPRequestHandler(*args, directory=str(folder), **kwargs)

    print(f"open http://localhost:{PORT}/test-film.html in the browser, then its console "
          "(Ctrl+Shift+J). Ctrl-C stops this.")
    with http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
