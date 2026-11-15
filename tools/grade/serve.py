"""Grade a batch of matches by ear.

    python tools/grade/serve.py batches/minimoog-1

Opens a page in your browser. For each reference recording you listen to the
candidate matches, pick the closest, and rate how close it is (1-5). Grades are
saved to grades.json in the batch folder as you go; then train on them with

    synthmatch_train --graded batches/minimoog-1

Only standard Python is needed. The page is served on this computer only
(127.0.0.1), and nothing is sent anywhere else.
"""

import argparse
import datetime
import http.server
import json
import os
import pathlib
import sys
import threading
import webbrowser

HERE = pathlib.Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description="Grade a batch of SynthMatch matches by ear.")
    parser.add_argument("batch", help="the batch folder (with batch.json)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true", help="don't open the browser")
    args = parser.parse_args()

    batch_dir = pathlib.Path(args.batch).resolve()
    manifest = batch_dir / "batch.json"
    if not manifest.exists():
        sys.exit(f"error: {manifest} not found. Make a batch with synthmatch_batch first.")
    grades_file = batch_dir / "grades.json"

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):  # keep the console quiet
            pass

        def send(self, code, body, kind):
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                self.send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/api/batch":
                data = json.loads(manifest.read_text(encoding="utf-8"))
                grades = {}
                if grades_file.exists():
                    grades = json.loads(grades_file.read_text(encoding="utf-8")).get("grades", {})
                data["grades"] = grades
                data["name"] = batch_dir.name
                self.send(200, json.dumps(data).encode(), "application/json")
            elif path.startswith("/files/"):
                # Only files inside the batch folder.
                target = (batch_dir / path[len("/files/"):]).resolve()
                if batch_dir not in target.parents or not target.is_file():
                    self.send(404, b"not found", "text/plain")
                    return
                kind = "audio/wav" if target.suffix == ".wav" else "application/json"
                self.send(200, target.read_bytes(), kind)
            else:
                self.send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path != "/api/grades":
                self.send(404, b"not found", "text/plain")
                return
            length = int(self.headers.get("Content-Length", "0"))
            grades = json.loads(self.rfile.read(length) or b"{}")
            record = {
                "format": "synthmatch-grades",
                "version": 1,
                "updated": datetime.datetime.now().isoformat(timespec="seconds"),
                "grades": grades,
            }
            tmp = grades_file.with_suffix(".tmp")
            tmp.write_text(json.dumps(record, indent=2), encoding="utf-8")
            os.replace(tmp, grades_file)  # never leave a half-written file
            self.send(200, b"{}", "application/json")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}/"
    print(f"Grading {batch_dir.name}: {url}  (Ctrl+C to stop; grades save as you go)")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped. Grades are in", grades_file)


if __name__ == "__main__":
    main()
