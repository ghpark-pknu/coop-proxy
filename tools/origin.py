#!/usr/bin/env python3
"""실험용 원서버 (origin server)

프록시 뒤에서 "진짜 웹 서버" 역할을 한다. Python 표준 라이브러리의
http.server를 바탕으로, 실험에 필요한 기능 세 가지를 더했다.

  1. 파일 제공      : --dir 폴더의 파일을 HTTP로 돌려준다.
                      Last-Modified 헤더와 If-Modified-Since → 304 응답을 지원한다.
  2. 지연 주입      : --delay 초만큼 응답을 늦춘다 (멀리 있는 서버 흉내).
  3. 요청 수 세기   : 프록시가 원서버에 몇 번 왔는지 센다.
                      GET /__count → JSON으로 조회, GET /__reset → 0으로 초기화
                      (이 두 경로는 세지 않는다)
"""
import argparse
import json
import os
import threading
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class RequestCounter:
    """여러 스레드가 동시에 세므로 락으로 보호한다."""

    def __init__(self):
        self._lock = threading.Lock()
        self.reset()

    def reset(self):
        with self._lock:
            self.total = 0          # 전체 요청 수
            self.conditional = 0    # If-Modified-Since가 붙은 요청 수 (2단계 캐시 검증 확인용)
            self.paths = {}         # 경로별 요청 수

    def add(self, path, conditional):
        with self._lock:
            self.total += 1
            if conditional:
                self.conditional += 1
            self.paths[path] = self.paths.get(path, 0) + 1

    def snapshot(self):
        with self._lock:
            return {"total": self.total, "conditional": self.conditional,
                    "paths": dict(self.paths)}


class OriginHandler(SimpleHTTPRequestHandler):
    server_version = "CoopOrigin/1.0"
    # HTTP/1.0: 응답을 하나 보내고 연결을 닫는다 → 프록시는 EOF를 응답 끝으로 본다
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        if self.path == "/__count":
            return self._send_json(self.server.counter.snapshot())
        if self.path == "/__reset":
            self.server.counter.reset()
            return self._send_json({"reset": True})

        self.server.counter.add(self.path, "If-Modified-Since" in self.headers)
        if self.server.delay > 0:
            time.sleep(self.server.delay)
        super().do_GET()    # 파일 전송 (304 처리 포함)

    def _send_json(self, obj):
        body = json.dumps(obj, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            super().log_message(fmt, *args)


class OriginServer(ThreadingHTTPServer):
    # 연결 대기열 길이. 기본값 5는 동시 요청이 몰리면 연결이 지연되므로 늘린다
    request_queue_size = 128


def main():
    ap = argparse.ArgumentParser(description="실험용 원서버")
    ap.add_argument("--port", type=int, default=9000, help="포트 (기본 9000)")
    ap.add_argument("--dir", default=os.path.expanduser("~/coop-data/www"),
                    help="제공할 폴더 (기본 ~/coop-data/www)")
    ap.add_argument("--delay", type=float, default=0.0, help="응답 지연(초)")
    ap.add_argument("--quiet", action="store_true", help="요청마다 출력하지 않음")
    args = ap.parse_args()

    if not os.path.isdir(args.dir):
        raise SystemExit(f"폴더가 없습니다: {args.dir}\n"
                         f"먼저 python3 tools/gen_files.py 로 테스트 파일을 만드세요.")

    handler = partial(OriginHandler, directory=args.dir)
    server = OriginServer(("0.0.0.0", args.port), handler)
    server.counter = RequestCounter()
    server.delay = args.delay
    server.quiet = args.quiet

    print(f"원서버: http://127.0.0.1:{args.port}/  폴더={args.dir}  지연={args.delay}s", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        print("원서버 종료", flush=True)


if __name__ == "__main__":
    main()
