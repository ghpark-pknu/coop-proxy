#!/usr/bin/env python3
"""락 전략 실험 결과 요약 (6단계) — scripts/exp_lock.sh가 만든 CSV를 표로 보여 준다.

같은 조건(샤드 수, 동시 연결 수)을 여러 번 잰 값은 중앙값으로 합친다.
(평균은 한 번 튄 값에 끌려가지만, 중앙값은 그렇지 않다)

사용 예:
  python3 tools/lock_summary.py results/lock_20261118_130000.csv
  python3 tools/lock_summary.py results/lock_*.csv --proxy-cpus 4 --load-cpus 4
"""
import argparse
import csv
import statistics
import sys
import unicodedata
from collections import defaultdict


def dwidth(s):
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in s)


def rjust(s, w):
    return " " * max(0, w - dwidth(s)) + s


def table(title, shards, concs, cell, fmt, note=""):
    print(f"\n{title}{'  ' + note if note else ''}")
    w = 10
    print(rjust("샤드＼동시", 12) + "".join(rjust(str(c), w) for c in concs))
    for s in shards:
        vals = [cell(s, c) for c in concs]
        print(rjust(str(s), 12) + "".join(rjust("-" if v is None else fmt(v), w) for v in vals))


def main():
    ap = argparse.ArgumentParser(description="락 전략 실험 CSV 요약")
    ap.add_argument("csv", nargs="+", help="exp_lock.sh의 결과 CSV (여러 개면 합쳐서 요약)")
    ap.add_argument("--proxy-cpus", type=int, help="프록시에 준 CPU 수 (포화 여부 판단용)")
    ap.add_argument("--load-cpus", type=int, help="부하 도구에 준 CPU 수 (부하 도구 포화 경고용)")
    args = ap.parse_args()

    rows = []
    for path in args.csv:
        with open(path, newline="") as f:
            rows += list(csv.DictReader(f))
    if not rows:
        print("결과가 없습니다")
        return 1

    groups = defaultdict(list)
    for r in rows:
        groups[(int(r["shards"]), int(r["conc"]))].append(r)
    shards = sorted({k[0] for k in groups})
    concs = sorted({k[1] for k in groups})
    reps = max(len(v) for v in groups.values())

    def med(s, c, field):
        vals = [float(r[field]) for r in groups.get((s, c), []) if r[field] not in ("", None)]
        return statistics.median(vals) if vals else None

    base = shards[0]
    print(f"측정 {len(rows)}회 (조건마다 최대 {reps}번 반복, 표의 값은 중앙값)")
    table("① 처리량 (요청/s)", shards, concs, lambda s, c: med(s, c, "rps"), lambda v: f"{v:,.0f}")
    table(f"② 샤드 {base}개 대비 처리량 배율", shards, concs,
          lambda s, c: (med(s, c, "rps") / med(base, c, "rps"))
          if med(s, c, "rps") and med(base, c, "rps") else None,
          lambda v: f"{v:.3f}x")
    table("③ 캐시 락 경합률 (%)", shards, concs, lambda s, c: med(s, c, "cache_contended_pct"),
          lambda v: f"{v:.3f}", "락을 잡으려 할 때 다른 스레드가 잡고 있던 비율")
    table("④ 통계 락 경합률 (%)", shards, concs, lambda s, c: med(s, c, "stats_contended_pct"),
          lambda v: f"{v:.3f}", "요청마다 한 번 잡는 전역 락")
    table("⑤ 요청 하나에 쓴 프록시 CPU 시간 (us)", shards, concs,
          lambda s, c: med(s, c, "proxy_us_per_req"), lambda v: f"{v:.1f}")
    table("⑥ 프록시가 쓴 CPU (코어)", shards, concs, lambda s, c: med(s, c, "proxy_cores"),
          lambda v: f"{v:.2f}", f"(프록시에 준 CPU {args.proxy_cpus}개)" if args.proxy_cpus else "")
    table("⑦ 응답 시간 p99 (ms)", shards, concs, lambda s, c: med(s, c, "p99_ms"), lambda v: f"{v:.3f}")

    # 반복 사이의 흩어짐: 같은 조건에서 (최대 - 최소) / 중앙값
    spreads = []
    for (s, c), rs in groups.items():
        v = [float(r["rps"]) for r in rs]
        if len(v) >= 2:
            spreads.append(((max(v) - min(v)) / statistics.median(v) * 100, s, c))
    print()
    if spreads:
        worst = max(spreads)
        print(f"반복 사이 처리량 흩어짐: 평균 {statistics.mean(x[0] for x in spreads):.1f}%, "
              f"가장 큰 곳 {worst[0]:.1f}% (샤드 {worst[1]}, 동시 {worst[2]})")
        print("  → 샤드 수에 따른 차이가 이 흩어짐보다 작으면, 그 차이는 측정 오차와 구별되지 않는다")

    failed = sum(int(r["failed"]) for r in rows)
    if failed:
        print(f"※ 실패한 요청이 {failed}개 있습니다. CSV의 failed 열을 확인하세요.")
    if args.load_cpus:
        lmax = max(float(r["loader_cores"]) for r in rows)
        if lmax > 0.85 * args.load_cpus:
            print(f"※ 부하 도구가 CPU {args.load_cpus}개 중 {lmax:.2f}개를 썼습니다. "
                  "부하 도구가 먼저 포화됐을 수 있으니 높은 동시 연결 수의 결과는 주의해서 보세요.")
        else:
            print(f"부하 도구 CPU 사용 최대 {lmax:.2f}코어 / {args.load_cpus}개 → 부하 도구는 포화되지 않음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
