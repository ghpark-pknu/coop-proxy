#!/usr/bin/env python3
"""Zipf 분포 부하 생성기 — 협력형 웹 캐싱 프록시 실험용 (4단계)

웹 객체의 인기도는 소수에 몰린다. k번째로 인기 있는 객체가 요청될 확률이 1/k^s 에
비례하는 분포를 Zipf 분포라 하고, 실제 프록시 기록에서는 s가 0.64~0.83 정도였다.
이 도구는 그런 요청열을 만들어 프록시(클러스터 또는 독립 프록시들)에 보내고
응답 시간, 히트율, 원서버 요청 수를 잰다.

  - 요청열은 시드로 정해진다. 옵션이 같으면 클러스터와 독립 프록시에 똑같은 요청열을 보낸다.
  - 요청 i는 진입 노드 목록의 (i mod 노드 수)번째로 보낸다 (클라이언트가 노드에 고르게 퍼진 상황).
    진입 노드에 연결할 수 없으면 목록의 다음 노드로 다시 연결한다 (--no-retry로 끔).
    연결이 된 뒤의 실패는 다시 보내지 않는다 (같은 요청이 두 번 세어지지 않게).
  - 받은 본문은 원본 파일과 CRC32로 비교한다 (원본이 없는 PC에서 실행하면 크기만 비교).

종료 코드: 0 모두 성공 / 1 실패한 요청 있음 / 2 준비 실패(옵션·파일·원서버) / 3 --stats 확인 불일치
"""

import argparse
import bisect
import json
import math
import os
import random
import socket
import sys
import threading
import time
import unicodedata
import zlib
from collections import Counter

OUTCOMES = ("HIT", "VALIDATED", "STALE", "MISS", "REFRESH")      # 직접 처리한 노드의 결과
HIT_KINDS = ("HIT", "VALIDATED", "STALE")                         # 캐시로 응답한 것
NODE_KEYS = ("HIT", "VALIDATED", "STALE", "MISS", "REFRESH", "FORWARD", "BLOCKED", "ERROR")


# ---------------------------------------------------------------- 출력 도우미

def dwidth(s):
    """터미널에서 차지하는 칸 수 (한글은 2칸)"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in s)


def ljust(s, w):
    return s + " " * max(0, w - dwidth(s))


def rjust(s, w):
    return " " * max(0, w - dwidth(s)) + s


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def mb(n):
    return n / 1048576


def fmt_ms(sec):
    if sec is None:
        return "-"
    ms = sec * 1000
    return f"{ms:.2f}" if ms < 10 else f"{ms:.1f}" if ms < 100 else f"{ms:.0f}"


def percentile(sorted_vals, q):
    """최근접 순위(nearest-rank) 방식: 정렬한 값에서 ceil(q/100 × n)번째"""
    if not sorted_vals:
        return None
    k = max(1, math.ceil(q / 100.0 * len(sorted_vals)))
    return sorted_vals[k - 1]


def parse_hostport(s):
    s = s.strip()
    if s.startswith("["):                       # [::1]:8081
        host, _, port = s[1:].partition("]:")
    else:
        host, _, port = s.rpartition(":")
    if not host or not port.isdigit() or not 1 <= int(port) <= 65535:
        raise ValueError(f"주소 형식이 틀립니다: {s!r} (예: 127.0.0.1:8081)")
    return host, int(port)


def hp(host, port):
    return f"[{host}]:{port}" if ":" in host else f"{host}:{port}"


# ---------------------------------------------------------------- 요청열

class Zipf:
    """순위 r(0부터)의 확률 ∝ 1/(r+1)^s.
    누적 분포(CDF)를 만들어 두고, 0~1 난수 u가 처음으로 넘지 않는 칸을 이분 탐색으로 찾는다."""

    def __init__(self, k, s):
        w = [1.0 / (r + 1) ** s for r in range(k)]
        total = sum(w)
        self.p = [x / total for x in w]
        self.cdf = []
        acc = 0.0
        for x in self.p:
            acc += x
            self.cdf.append(acc)
        self.cdf[-1] = 1.0              # 부동소수 오차로 1보다 조금 작아지는 것을 막는다

    def sample(self, rng):
        return bisect.bisect_right(self.cdf, rng.random())    # random()은 [0, 1) → 0 ~ k-1


class Workload:
    """시드로 정해지는 요청열. i번째 요청의 객체 번호를 돌려준다 (필요한 만큼 늘려 가며 만든다)."""

    def __init__(self, objects, s, seed):
        self.objects = objects
        self.rng = random.Random(seed)
        self.rank_to_obj = list(range(len(objects)))
        self.rng.shuffle(self.rank_to_obj)      # 인기 순위와 객체(크기)가 엮이지 않게 섞는다
        self.zipf = Zipf(len(objects), s)
        self.seq = []
        self.lock = threading.Lock()

    def obj_at(self, i):
        with self.lock:
            while len(self.seq) <= i:
                self.seq.append(self.rank_to_obj[self.zipf.sample(self.rng)])
            return self.seq[i]


def predict_origin(seq, n_entries, start):
    """캐시가 충분하고 동시 요청이 없을 때의 원서버 요청 수 (seq[start:] 구간에서).
    클러스터: 객체마다 클러스터 전체에서 처음 한 번 / 독립 프록시: 진입 노드마다 처음 한 번"""
    seen_all, seen_entry = set(), [set() for _ in range(n_entries)]
    cluster = independent = 0
    for i, o in enumerate(seq):
        e = i % n_entries
        new_all, new_entry = o not in seen_all, o not in seen_entry[e]
        seen_all.add(o)
        seen_entry[e].add(o)
        if i >= start:
            cluster += new_all
            independent += new_entry
    return cluster, independent


# ---------------------------------------------------------------- HTTP

def http_get_json(host, port, path, timeout=3.0):
    """관제용 GET (프록시 환경 변수의 영향을 받지 않게 소켓으로 직접 보낸다). 실패하면 None"""
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.sendall(f"GET {path} HTTP/1.0\r\nHost: {hp(host, port)}\r\n\r\n".encode("ascii"))
            data = bytearray()
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
    except OSError:
        return None
    head, sep, body = bytes(data).partition(b"\r\n\r\n")
    parts = head.split(None, 2)
    if not sep or len(parts) < 2 or parts[1] != b"200":
        return None
    try:
        return json.loads(body.decode("utf-8"))
    except ValueError:
        return None


class Result:
    __slots__ = ("i", "obj", "entry", "status", "nbytes", "latency", "ttfb", "chain", "error", "retries")

    def __init__(self, i, obj):
        self.i, self.obj = i, obj
        self.entry = None        # 실제로 연결한 진입 노드 "host:port"
        self.status = 0
        self.nbytes = 0          # 받은 본문 바이트
        self.latency = None      # 첫 연결 시도 ~ 응답 끝(EOF), 초
        self.ttfb = None         # 첫 연결 시도 ~ 첫 바이트, 초
        self.chain = []          # X-Cache 줄들: [("FORWARD", "node1"), ("MISS", "node2")]
        self.error = None
        self.retries = 0         # 다음 진입 노드로 다시 연결한 횟수


def fetch(i, oi, obj, entries, origin, timeout, retry):
    r = Result(i, oi)
    req = (f"GET http://{origin}{obj['path']} HTTP/1.0\r\n"
           f"Host: {origin}\r\n"
           f"User-Agent: coop-loadgen/1.0\r\n\r\n").encode("ascii")
    n = len(entries)
    t0 = time.perf_counter()
    sock = None
    for a in range(n if retry else 1):
        host, port = entries[(i + a) % n]
        try:
            sock = socket.create_connection((host, port), timeout=timeout)
        except socket.timeout:
            r.error = "연결 시간 초과"
            continue
        except OSError:
            r.error = "연결 실패"
            continue
        r.entry, r.retries, r.error = hp(host, port), a, None
        break
    if sock is None:
        r.retries = (n if retry else 1) - 1
        r.latency = time.perf_counter() - t0
        return r

    head = bytearray()
    head_end = -1
    crc = 0
    try:
        with sock:
            sock.sendall(req)
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                if r.ttfb is None:
                    r.ttfb = time.perf_counter() - t0
                if head_end < 0:
                    head += chunk
                    k = head.find(b"\r\n\r\n")
                    if k >= 0:
                        head_end = k + 4
                        body = bytes(head[head_end:])
                        r.nbytes = len(body)
                        crc = zlib.crc32(body)
                        del head[head_end:]
                    elif len(head) > 65536:
                        r.error = "응답 헤더가 너무 김"
                        break
                else:
                    r.nbytes += len(chunk)
                    crc = zlib.crc32(chunk, crc)
    except socket.timeout:
        r.error = "응답 시간 초과"
    except OSError:
        r.error = "연결 끊김"
    r.latency = time.perf_counter() - t0

    # 헤더 해석 (도중에 실패했어도 받은 만큼은 해석해 둔다: --stats 확인에 쓰인다)
    lines = bytes(head[:head_end] if head_end >= 0 else head).split(b"\r\n")
    status = lines[0].split(None, 2)
    if len(status) >= 2 and status[0].startswith(b"HTTP/") and status[1].isdigit():
        r.status = int(status[1])
    clen = None
    for line in lines[1:]:
        name, _, value = line.partition(b":")
        name = name.strip().lower()
        if name == b"x-cache":
            v = value.decode("latin-1").split()
            r.chain.append((v[0] if v else "?", v[2] if len(v) >= 3 and v[1] == "from" else "?"))
        elif name == b"content-length" and value.strip().isdigit():
            clen = int(value.strip())

    if r.error:
        return r
    if head_end < 0:
        r.error = "응답 형식 오류" if head else "빈 응답"
    elif r.status != 200:
        r.error = f"HTTP {r.status}"
    elif clen is not None and clen != r.nbytes:
        r.error = "본문 잘림"
    elif r.nbytes != obj["size"]:
        r.error = "크기 불일치"
    elif obj.get("crc") is not None and crc != obj["crc"]:
        r.error = "내용 불일치"
    return r


# ---------------------------------------------------------------- 실행

class Progress:
    def __init__(self, enabled):
        self.enabled = enabled
        self.lock = threading.Lock()
        self.done = self.failed = 0
        self.stop = threading.Event()
        self.thread = None

    def add(self, r):
        with self.lock:
            self.done += 1
            self.failed += r.error is not None

    def start(self, label):
        if not self.enabled:
            return
        t0 = time.perf_counter()

        def loop():
            while not self.stop.wait(1.0):
                with self.lock:
                    done, failed = self.done, self.failed
                el = time.perf_counter() - t0
                sys.stderr.write(f"\r  {label}: {done}개 · {done / el:.0f}/s · 실패 {failed}   ")
                sys.stderr.flush()

        self.thread = threading.Thread(target=loop, daemon=True)
        self.thread.start()

    def finish(self):
        self.stop.set()
        if self.thread:
            self.thread.join()
            sys.stderr.write("\r" + " " * 60 + "\r")
            sys.stderr.flush()


def run_phase(work, start, count, duration, args, entries, origin, stop, label):
    """요청 번호 start부터 count개(또는 duration초 동안)를 동시 args.concurrency개로 보낸다"""
    lock = threading.Lock()
    nxt = [start]
    end = start + count if count is not None else None
    t0 = time.perf_counter()
    deadline = t0 + duration if duration is not None else None
    nthreads = args.concurrency if count is None else max(1, min(args.concurrency, count))
    outs = [[] for _ in range(nthreads)]
    progress = Progress(not args.quiet and sys.stderr.isatty())

    def take():
        with lock:
            i = nxt[0]
            if stop.is_set() or (end is not None and i >= end) or \
               (deadline is not None and time.perf_counter() >= deadline):
                return None
            nxt[0] = i + 1
            return i

    def worker(out):
        while True:
            i = take()
            if i is None:
                return
            if args.rate:                                   # 요청 i의 예정 시각까지 기다린다
                delay = t0 + (i - start) / args.rate - time.perf_counter()
                if delay > 0 and stop.wait(delay):
                    return
                if deadline is not None and time.perf_counter() >= deadline:
                    return
            oi = work.obj_at(i)
            r = fetch(i, oi, work.objects[oi], entries, origin, args.timeout, not args.no_retry)
            out.append(r)
            progress.add(r)

    threads = [threading.Thread(target=worker, args=(outs[k],), daemon=True) for k in range(nthreads)]
    progress.start(label)
    for t in threads:
        t.start()
    try:
        for t in threads:
            while t.is_alive():
                t.join(0.2)
    except KeyboardInterrupt:
        stop.set()
        for t in threads:
            t.join()
    elapsed = time.perf_counter() - t0
    progress.finish()
    results = [r for out in outs for r in out]
    results.sort(key=lambda r: r.i)
    return results, elapsed


def summarize(results, elapsed):
    ok = [r for r in results if r.error is None]
    finals = Counter(r.chain[-1][0] for r in results if r.chain)
    hits = sum(finals[k] for k in HIT_KINDS)
    misses = finals["MISS"] + finals["REFRESH"]
    lat = sorted(r.latency for r in ok)
    total_bytes = sum(r.nbytes for r in results)
    return {
        "requests": len(results),
        "ok": len(ok),
        "failed": len(results) - len(ok),
        "errors": dict(Counter(r.error for r in results if r.error)),
        "connected": sum(1 for r in results if r.entry),
        "retries": sum(r.retries for r in results),
        "elapsed_sec": elapsed,
        "rps": len(results) / elapsed if elapsed > 0 else 0.0,
        "bytes": total_bytes,
        "final": {k: finals[k] for k in OUTCOMES},
        "final_other": sum(v for k, v in finals.items() if k not in OUTCOMES),
        "forwarded": sum(1 for r in results if len(r.chain) >= 2),
        "hit_ratio": hits / (hits + misses) if hits + misses else None,
        "latency_ms": {
            "mean": 1000 * sum(lat) / len(lat) if lat else None,
            **{f"p{q}": 1000 * percentile(lat, q) if lat else None for q in (50, 90, 95, 99)},
            "max": 1000 * lat[-1] if lat else None,
        },
        "per_entry": dict(Counter(r.entry for r in results if r.entry)),
    }


def node_delta(before, after):
    """측정 전후 /__stats의 차이. 두 번 다 응답이 없으면 "down"(계산에서 제외),
    한 번만 응답했거나 그사이 재시작했으면 None(확인 불가)"""
    if before is None and after is None:
        return "down"
    if not before or not after or after["uptime_ms"] < before["uptime_ms"]:
        return None
    d = {k: after["outcomes"][k] - before["outcomes"][k] for k in NODE_KEYS}
    d["client"] = after["requests"]["client"] - before["requests"]["client"]
    d["peer"] = after["requests"]["peer"] - before["requests"]["peer"]
    d["origin"] = after["origin_requests"] - before["origin_requests"]
    d["peer_failures"] = after["peer_failures"] - before["peer_failures"]
    return d


# ---------------------------------------------------------------- 보고

def print_dry_run(work, args, n_entries, total, warmup):
    seq = [work.obj_at(i) for i in range(warmup + total)]
    meas = seq[warmup:]
    k = len(work.objects)
    p = work.zipf.p
    cnt = Counter(meas)
    rank_of = {o: r for r, o in enumerate(work.rank_to_obj)}

    def share_top(m):
        top = set(work.rank_to_obj[:m])
        return sum(1 for o in meas if o in top) / len(meas)

    tops = [1, 10, max(1, k // 10)]
    print(f"[요청열 분석] Zipf s={args.zipf} · 객체 {k}개 · 요청 {total}개"
          f"{f' (예열 {warmup}개 다음)' if warmup else ''} · 시드 {args.seed}")
    print("  인기 상위 객체가 받는 요청 비율:   이론          요청열")
    shares = {}
    for m in tops:
        th, sm = sum(p[:m]), share_top(m)
        shares[str(m)] = {"theory": th, "sample": sm}
        label = f"상위 {m}개" + (f" ({100 * m // k}%)" if m == k // 10 else "")
        print(f"    {ljust(label, 18)}  {100 * th:6.2f}%      {100 * sm:6.2f}%")
    print("  가장 인기 있는 객체 5개:")
    for o, c in cnt.most_common(5):
        obj = work.objects[o]
        print(f"    {obj['path']}  {obj['size'] / 1024:7.1f}KB  인기 순위 {rank_of[o] + 1:3d}  "
              f"이론 {100 * p[rank_of[o]]:5.2f}%  요청열 {pct(c, total):5.2f}%")
    first_seen = set(seq[:warmup])
    cold = 0
    for o in meas:
        if o not in first_seen:
            cold += 1
            first_seen.add(o)
    req_bytes = sum(work.objects[o]["size"] for o in meas)
    uniq_bytes = sum(work.objects[o]["size"] for o in cnt)
    cl, ind = predict_origin(seq, n_entries, warmup)
    print(f"  서로 다른 객체 {len(cnt)}개 ({mb(uniq_bytes):.1f}MB) · 요청한 데이터 합계 {mb(req_bytes):.1f}MB")
    print(f"  캐시가 무한할 때 최대 히트율 {pct(total - cold, total):.1f}% (처음 보는 객체만 미스)")
    print(f"  원서버 요청 예측 (캐시 충분, 동시 요청 없음, 진입 노드 {n_entries}개에 번갈아):")
    print(f"    협력형 클러스터 {cl}개 · 독립 프록시 {ind}개 → {ind / cl if cl else 0:.2f}배")
    return {
        "objects": k, "requests": total, "warmup": warmup,
        "top_share": shares, "distinct": len(cnt), "distinct_bytes": uniq_bytes,
        "requested_bytes": req_bytes, "max_hit_ratio": (total - cold) / total if total else None,
        "predict_origin": {"cluster": cl, "independent": ind},
    }


def print_summary(s, args, entries, origin_delta, pred):
    print(f"[결과] {s['elapsed_sec']:.2f}초 · {s['rps']:.1f} 요청/s · 받은 본문 {mb(s['bytes']):.1f}MB "
          f"({mb(s['bytes']) / s['elapsed_sec'] if s['elapsed_sec'] > 0 else 0:.1f}MB/s)")
    line = f"  성공 {s['ok']} · 실패 {s['failed']}"
    if s["errors"]:
        line += "  (" + ", ".join(f"{k} {v}" for k, v in sorted(s["errors"].items(), key=lambda x: -x[1])) + ")"
    print(line)
    f = s["final"]
    print("  최종 처리 (X-Cache 마지막 줄): " + " · ".join(f"{k} {f[k]}" for k in OUTCOMES)
          + (f" · 기타 {s['final_other']}" if s["final_other"] else ""))
    hr = s["hit_ratio"]
    print(f"  히트율 {'-' if hr is None else f'{100 * hr:.1f}%'} · 다른 노드를 거친 요청 {s['forwarded']} "
          f"({pct(s['forwarded'], s['requests']):.1f}%) · 진입 노드 재연결 {s['retries']}회")
    L = s["latency_ms"]
    if L["mean"] is not None:
        print(f"  응답 시간 ms: 평균 {fmt_ms(L['mean'] / 1000)} · p50 {fmt_ms(L['p50'] / 1000)} · "
              f"p90 {fmt_ms(L['p90'] / 1000)} · p95 {fmt_ms(L['p95'] / 1000)} · "
              f"p99 {fmt_ms(L['p99'] / 1000)} · 최대 {fmt_ms(L['max'] / 1000)}")
    if len(entries) > 1:
        print("  진입 노드별 요청: " + " · ".join(f"{e} {s['per_entry'].get(e, 0)}" for e in (hp(h, p) for h, p in entries)))
    if origin_delta is not None:
        print(f"  원서버 요청 {origin_delta['total']}개 (요청의 {pct(origin_delta['total'], s['requests']):.1f}%, "
              f"조건부 {origin_delta['conditional']}개)")
    print(f"  참고: 캐시가 충분하고 동시 요청이 없다면 원서버 요청은 "
          f"협력형 클러스터 {pred[0]}개, 독립 프록시 {pred[1]}개")


def print_node_checks(nodes, deltas, s, origin_delta):
    """노드 /__stats 변화량 표와 불변식 확인. 확인 결과 목록을 돌려준다"""
    print("[노드 통계 변화량 (/__stats)]")
    cols = ["클라이언트", "넘겨받음", "HIT", "VALIDATED", "STALE", "MISS", "REFRESH", "FORWARD", "원서버", "연결실패"]
    keys = ["client", "peer", "HIT", "VALIDATED", "STALE", "MISS", "REFRESH", "FORWARD", "origin", "peer_failures"]
    w0 = max([dwidth("합계")] + [dwidth(n) for n in nodes]) + 2
    print("  " + ljust("노드", w0) + "".join(rjust(c, max(8, dwidth(c)) + 2) for c in cols))
    total = Counter()
    missing = []
    for name in nodes:
        d = deltas[name]
        if d == "down":
            print("  " + ljust(name, w0) + "  (측정 전후 모두 응답 없음: 계산에서 제외)")
            continue
        if d is None:
            missing.append(name)
            print("  " + ljust(name, w0) + "  (측정 중 응답이 끊겼거나 재시작함)")
            continue
        total.update({k: d[k] for k in keys + ["BLOCKED", "ERROR"]})
        print("  " + ljust(name, w0) + "".join(rjust(str(d[k]), max(8, dwidth(c)) + 2) for k, c in zip(keys, cols)))
    print("  " + ljust("합계", w0) + "".join(rjust(str(total[k]), max(8, dwidth(c)) + 2) for k, c in zip(keys, cols)))

    checks = []

    def check(desc, ok):
        checks.append({"check": desc, "ok": ok})
        print(f"    {'✓' if ok else '✗'} {desc}")

    print("  확인")
    if missing:
        print(f"    - 생략: 통계를 읽지 못한 노드가 있습니다 ({', '.join(missing)})")
        return checks
    check(f"노드들이 센 클라이언트 요청 {total['client']} = 연결된 요청 {s['connected']}",
          total["client"] == s["connected"])
    node_final = {k: total[k] for k in OUTCOMES}
    check("노드별 결과 합 (" + " ".join(f"{k} {node_final[k]}" for k in OUTCOMES if node_final[k]) +
          ") = 응답의 X-Cache 마지막 줄", node_final == s["final"])
    check(f"넘긴 요청(FORWARD) {total['FORWARD']} = 넘겨받은 요청 {total['peer']} = 다른 노드를 거친 응답 {s['forwarded']}",
          total["FORWARD"] == total["peer"] == s["forwarded"])
    if origin_delta is not None:
        check(f"노드들의 원서버 요청 {total['origin']} = 원서버가 센 요청 {origin_delta['total']}",
              total["origin"] == origin_delta["total"])
    return checks


# ---------------------------------------------------------------- main

def main():
    data_dir = os.environ.get("COOP_DATA") or os.path.expanduser("~/coop-data")
    ap = argparse.ArgumentParser(description="Zipf 분포 부하 생성기 (협력형 웹 캐싱 프록시 실험용)")
    ap.add_argument("--proxies", default="127.0.0.1:8081,127.0.0.1:8082,127.0.0.1:8083",
                    help="진입 노드 목록, 쉼표로 구분 (기본 127.0.0.1:8081,127.0.0.1:8082,127.0.0.1:8083)")
    ap.add_argument("--origin", default="127.0.0.1:9000", help="원서버 주소 (기본 127.0.0.1:9000)")
    ap.add_argument("--manifest", default=os.path.join(data_dir, "www", "manifest.json"),
                    help="객체 목록 (기본 $COOP_DATA/www/manifest.json, COOP_DATA 기본값 ~/coop-data)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("-n", "--requests", type=int, help="보낼 요청 수 (기본 2000)")
    g.add_argument("-d", "--duration", type=float, help="이 시간(초) 동안 보낸다 (요청 수 대신)")
    ap.add_argument("-c", "--concurrency", type=int, default=8, help="동시 연결 수 (기본 8)")
    ap.add_argument("--rate", type=float, default=0, help="초당 요청 수 상한 (기본 0 = 제한 없음)")
    ap.add_argument("--zipf", type=float, default=0.8, help="Zipf 지수 s (기본 0.8, 0이면 균등)")
    ap.add_argument("--objects", type=int, help="객체 목록의 앞에서 몇 개를 쓸지 (기본 전부)")
    ap.add_argument("--seed", type=int, default=2026, help="요청열 시드 (기본 2026)")
    ap.add_argument("--warmup", type=int, default=0, help="측정 전에 보낼 예열 요청 수 (기본 0)")
    ap.add_argument("--timeout", type=float, default=10.0, help="요청 하나의 송수신 제한 시간, 초 (기본 10)")
    ap.add_argument("--no-retry", action="store_true", help="진입 노드 연결 실패 시 다음 노드로 다시 연결하지 않음")
    ap.add_argument("--stats", action="store_true", help="노드 /__stats 변화량을 표로 보이고 부하 생성기 결과와 맞춰 본다")
    ap.add_argument("--csv", help="요청별 기록을 CSV로 저장")
    ap.add_argument("--json", help="요약을 JSON으로 저장")
    ap.add_argument("--dry-run", action="store_true", help="요청을 보내지 않고 요청열만 분석")
    ap.add_argument("-q", "--quiet", action="store_true", help="진행 표시를 하지 않음")
    args = ap.parse_args()

    def fail(msg):
        print(f"loadgen: {msg}", file=sys.stderr)
        sys.exit(2)

    if args.concurrency < 1:
        fail("--concurrency는 1 이상이어야 합니다")
    if args.zipf < 0:
        fail("--zipf는 0 이상이어야 합니다")
    if args.rate < 0 or args.warmup < 0 or args.timeout <= 0:
        fail("--rate·--warmup은 0 이상, --timeout은 0보다 커야 합니다")
    if args.requests is not None and args.requests < 1:
        fail("--requests는 1 이상이어야 합니다")
    if args.duration is not None and args.duration <= 0:
        fail("--duration은 0보다 커야 합니다")
    try:
        entries = [parse_hostport(x) for x in args.proxies.split(",") if x.strip()]
        origin_host, origin_port = parse_hostport(args.origin)
    except ValueError as e:
        fail(str(e))
    if not entries:
        fail("--proxies가 비어 있습니다")
    origin = hp(origin_host, origin_port)

    try:
        with open(args.manifest, encoding="utf-8") as f:
            objects = json.load(f)["objects"]
    except (OSError, ValueError, KeyError) as e:
        fail(f"객체 목록을 읽지 못했습니다: {args.manifest} ({e})\n"
             f"  먼저 python3 tools/gen_files.py 로 테스트 파일을 만드세요")
    objects = sorted(objects, key=lambda o: o["path"])
    if args.objects is not None:
        if not 1 <= args.objects <= len(objects):
            fail(f"--objects는 1~{len(objects)} 사이여야 합니다")
        objects = objects[:args.objects]

    # 내용 검사용 CRC32: 원본 파일이 이 PC에 있으면 읽어 둔다
    www = os.path.dirname(os.path.abspath(args.manifest))
    verified = 0
    for o in objects:
        o["crc"] = None
        path = os.path.join(www, o["path"].lstrip("/"))
        try:
            if os.path.getsize(path) == o["size"]:
                with open(path, "rb") as f:
                    o["crc"] = zlib.crc32(f.read())
                verified += 1
        except OSError:
            pass

    work = Workload(objects, args.zipf, args.seed)
    total = args.requests if args.requests is not None else (None if args.duration else 2000)
    report = {"config": {"proxies": [hp(h, p) for h, p in entries], "origin": origin, "zipf": args.zipf,
                         "objects": len(objects), "seed": args.seed, "requests": total,
                         "duration": args.duration, "concurrency": args.concurrency, "rate": args.rate,
                         "warmup": args.warmup, "retry": not args.no_retry}}

    if args.dry_run:
        report["dry_run"] = print_dry_run(work, args, len(entries), total or 10000, args.warmup)
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(report, f, ensure_ascii=False, indent=1)
        return 0

    plan = f"요청 {total}개" if total is not None else f"{args.duration:g}초 동안"
    print(f"[부하 생성] Zipf s={args.zipf} · 객체 {len(objects)}개 · {plan} · 동시 {args.concurrency}"
          + (f" · 초당 {args.rate:g}개 상한" if args.rate else "") + f" · 시드 {args.seed}")
    print(f"  진입 노드: {', '.join(hp(h, p) for h, p in entries)}"
          + (" (요청 번호 순서대로 돌아가며)" if len(entries) > 1 else ""))
    print(f"  원서버: {origin} · 내용 검사: "
          + (f"CRC32 (원본 파일 {verified}개와 비교)" if verified == len(objects)
             else f"크기만 비교 (원본 파일이 없는 객체 {len(objects) - verified}개)"))

    stop = threading.Event()
    if args.warmup:
        res, el = run_phase(work, 0, args.warmup, None, args, entries, origin, stop, "예열")
        bad = sum(1 for r in res if r.error)
        print(f"  예열 {len(res)}개 완료 ({el:.2f}초, 실패 {bad})")

    # 측정 전 상태 (원서버 요청 수, 노드 통계)
    o_before = http_get_json(origin_host, origin_port, "/__count")
    if o_before is None:
        print(f"  ! 원서버 {origin}의 /__count를 읽지 못했습니다: 원서버 요청 수는 생략합니다")
    stat_nodes = []
    if args.stats:
        for h, p in entries:                       # 진입 노드 + 그 노드들이 아는 클러스터 노드 전부
            key = hp(h, p)
            if key not in stat_nodes:
                stat_nodes.append(key)
            js = http_get_json(h, p, "/__stats")
            for m in (js or {}).get("cluster", []):
                k2 = hp(m["host"], m["port"])
                if k2 not in stat_nodes:
                    stat_nodes.append(k2)
    s_before = {k: http_get_json(*parse_hostport(k), "/__stats") for k in stat_nodes}

    results, elapsed = run_phase(work, args.warmup, total, args.duration, args, entries, origin, stop, "측정")
    if stop.is_set():
        print("  ! 중단됨: 그때까지의 결과만 보입니다")
    if not results:
        print("보낸 요청이 없습니다")
        return 1

    o_after = http_get_json(origin_host, origin_port, "/__count")
    s_after = {k: http_get_json(*parse_hostport(k), "/__stats") for k in stat_nodes}
    origin_delta = None
    if o_before is not None and o_after is not None:
        origin_delta = {"total": o_after["total"] - o_before["total"],
                        "conditional": o_after["conditional"] - o_before["conditional"]}

    s = summarize(results, elapsed)
    seq = [work.obj_at(i) for i in range(results[-1].i + 1)]
    pred = predict_origin(seq, len(entries), args.warmup)
    print_summary(s, args, entries, origin_delta, pred)
    report.update(result=s, origin=origin_delta, predict_origin={"cluster": pred[0], "independent": pred[1]})

    rc = 0 if s["failed"] == 0 else 1
    if args.stats:
        deltas = {}
        names = {}
        for k in stat_nodes:
            deltas[k] = node_delta(s_before[k], s_after[k])
            js = s_after[k] or s_before[k]
            names[k] = f"{js['node']} ({k})" if js else k
        shown = [names[k] for k in stat_nodes]
        checks = print_node_checks(shown, {names[k]: deltas[k] for k in stat_nodes}, s, origin_delta)
        report["nodes"] = {names[k]: deltas[k] for k in stat_nodes}
        report["checks"] = checks
        if rc == 0 and any(not c["ok"] for c in checks):
            rc = 3

    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as f:
            f.write("i,entry,path,status,bytes,latency_ms,ttfb_ms,final,chain,retries,error\n")
            for r in results:
                chain = ">".join(f"{a}@{b}" for a, b in r.chain)
                final = r.chain[-1][0] if r.chain else ""
                ttfb = f"{1000 * r.ttfb:.3f}" if r.ttfb is not None else ""
                f.write(f"{r.i},{r.entry or ''},{objects[r.obj]['path']},{r.status},{r.nbytes},"
                        f"{1000 * r.latency:.3f},{ttfb},{final},{chain},{r.retries},{r.error or ''}\n")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
