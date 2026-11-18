#!/usr/bin/env python3
"""락 전략 실험 결과 요약 — scripts/exp_lock.sh가 만든 CSV를 표로 보여 준다 (6~7단계).

같은 조건(샤드 수, 동시 연결 수)을 여러 번 잰 값은 중앙값으로 합친다.
(평균은 한 번 튄 값에 끌려가지만, 중앙값은 그렇지 않다)

7단계에서 늘어난 표
  ⑧ 경합률 감소: 관측 대 예측 Σ(샤드별 요청 몫)²   ← 프록시와 같은 해시로 계산
  ⑨ 요청 하나가 캐시 락을 기다린 시간 (요청당 CPU 시간과 비교)
  ⑩ 메인(accept) 스레드가 쓴 CPU        ← 7단계 CSV에만 있음
  ⑪ 자료구조만 잰 배율 대 프록시 전체 배율  ← 같은 이름의 _bench.csv가 있으면
6단계 형식(19열) CSV도 그대로 읽는다. 없는 열의 표는 건너뛴다.

사용 예:
  python3 tools/lock_summary.py results/lock_20261118_122931.csv
  python3 tools/lock_summary.py results/lock_A.csv results/lock_B.csv    # 여러 번 잰 것을 합쳐서
"""
import argparse
import statistics
import sys
import unicodedata

from exp_common import (bench_path, group_by, hot_share, median_of, ncpus, num, read_env,
                        read_rows, sum_sq)


def dwidth(s):
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in s)


def rjust(s, w):
    return " " * max(0, w - dwidth(s)) + s


def table(title, row_keys, col_keys, cell, fmt, note="", corner="샤드＼동시", w=10):
    print(f"\n{title}{'  ' + note if note else ''}")
    print(rjust(corner, 12) + "".join(rjust(str(c), w) for c in col_keys))
    for rk in row_keys:
        vals = [cell(rk, c) for c in col_keys]
        print(rjust(str(rk), 12) + "".join(rjust("-" if v is None else fmt(v), w) for v in vals))


def main():
    ap = argparse.ArgumentParser(description="락 전략 실험 CSV 요약")
    ap.add_argument("csv", nargs="+", help="exp_lock.sh의 결과 CSV (여러 개면 합쳐서 요약)")
    ap.add_argument("--proxy-cpus", type=int, help="프록시에 준 CPU 수 (없으면 .env에서 읽음)")
    ap.add_argument("--load-cpus", type=int, help="부하 도구에 준 CPU 수 (없으면 .env에서 읽음)")
    args = ap.parse_args()

    rows = read_rows(args.csv)
    if not rows:
        print("결과가 없습니다")
        return 1
    env = read_env(args.csv[0])
    origin = env.get("origin", "127.0.0.1:19600")       # 6단계 exp_lock.sh도 19600을 썼다
    keys = int(env.get("keys", 200))
    zipf = float(env.get("zipf", 0.8))
    np_ = args.proxy_cpus or ncpus(env.get("proxy_cpus"))
    nl_ = args.load_cpus or ncpus(env.get("load_cpus"))

    g = group_by(rows, "shards", "conc")
    groups = {(int(s), int(c)): v for (s, c), v in g.items()}
    shards = sorted({k[0] for k in groups})
    concs = sorted({k[1] for k in groups})
    reps = max(len(v) for v in groups.values())
    base = shards[0]
    has = lambda key: any(num(r, key) is not None for r in rows)

    def med(s, c, key):
        return median_of(groups.get((s, c), []), key)

    def ratio(s, c, key):
        a, b = med(s, c, key), med(base, c, key)
        return a / b if a is not None and b else None

    if env:
        print(f"실험 환경: {env.get('cpu_model', '?')}, 프록시 CPU {env.get('proxy_cpus', '?')} / "
              f"부하 도구 CPU {env.get('load_cpus', '?')}, 측정 {env.get('dur', '?')}초 × 반복 {env.get('repeat', '?')}")
    print(f"측정 {len(rows)}회 (조건마다 최대 {reps}번 반복, 표의 값은 중앙값)")

    table("① 처리량 (요청/s)", shards, concs, lambda s, c: med(s, c, "rps"), lambda v: f"{v:,.0f}")

    def spread(s, c):
        v = [num(r, "rps") for r in groups.get((s, c), [])]
        return (max(v) - min(v)) / statistics.median(v) * 100 if len(v) >= 2 else None
    table("①-2 반복 사이 처리량 흩어짐: (최대 − 최소) ÷ 중앙값 (%)", shards, concs, spread,
          lambda v: f"{v:.0f}", "샤드 사이의 차이가 이보다 작으면 측정 오차와 구별되지 않는다")
    table(f"② 샤드 {base}개 대비 처리량 배율", shards, concs, lambda s, c: ratio(s, c, "rps"),
          lambda v: f"{v:.3f}x")
    table("③ 캐시 락 경합률 (%)", shards, concs, lambda s, c: med(s, c, "cache_contended_pct"),
          lambda v: f"{v:.3f}", "락을 잡으려 할 때 다른 스레드가 잡고 있던 비율")
    table("④ 통계 락 경합률 (%)", shards, concs, lambda s, c: med(s, c, "stats_contended_pct"),
          lambda v: f"{v:.3f}", "요청마다 한 번 잡는 전역 락")
    table("⑤ 요청 하나에 쓴 프록시 CPU 시간 (us)", shards, concs,
          lambda s, c: med(s, c, "proxy_us_per_req"), lambda v: f"{v:.1f}")
    table("⑥ 프록시가 쓴 CPU (코어)", shards, concs, lambda s, c: med(s, c, "proxy_cores"),
          lambda v: f"{v:.2f}", f"(프록시에 준 CPU {np_}개)" if np_ else "")
    table("⑦ 응답 시간 p99 (ms)", shards, concs, lambda s, c: med(s, c, "p99_ms"), lambda v: f"{v:.3f}")

    # ⑧ 경합률 감소의 예측: 경합이 드문 영역에서는 "지금 락을 쥔 스레드가 같은 샤드에 있을 확률"에
    #    비례하므로, 샤드 1개 대비 경합률 = Σ p_s² (p_s = 샤드 s가 받는 요청의 몫)
    hi = [c for c in concs if c >= 8] or concs
    print(f"\n⑧ 캐시 락 경합률: 샤드 {base}개 대비 비율, 관측 대 예측 Σ(샤드별 요청 몫)²"
          f"  (원서버 {origin}, 객체 {keys}개, Zipf {zipf})")
    print(rjust("샤드", 12) + rjust("예측 Σp²", 12) + rjust("1/S", 10)
          + "".join(rjust(f"관측 동시{c}", 12) for c in hi) + rjust("1위 키 몫", 12))
    for s in shards:
        pred = sum_sq(origin, keys, zipf, s) / sum_sq(origin, keys, zipf, base)
        obs = []
        for c in hi:
            a, b = med(s, c, "cache_contended_pct"), med(base, c, "cache_contended_pct")
            # 경합 횟수가 적으면 비율이 우연에 좌우된다: 샤드 1개 쪽 경합이 반복마다 100회 이상일 때만 비교
            enough = all((num(r, "cache_contended") or 0) >= 100 for r in groups.get((base, c), []))
            obs.append(f"{a / b:.3f}" if a is not None and b and enough else "표본 부족")
        print(rjust(str(s), 12) + rjust(f"{pred:.3f}", 12) + rjust(f"{base / s:.3f}", 10)
              + "".join(rjust(o, 12) for o in obs) + rjust(f"{100 * hot_share(origin, keys, zipf, s):.1f}%", 12))
    print("  (Σp²가 1/S보다 큰 만큼이 Zipf 쏠림의 몫이다. 예측은 경합이 드물 때만 맞는다:"
          " 자료구조 벤치마크처럼 경합이 잦으면 줄을 서는 효과가 더해진다."
          " '표본 부족' = 샤드 1개 쪽 경합이 100회 미만이라 비율을 믿을 수 없음)")

    # ⑨ 캐시 락을 기다린 시간: HIT만 재므로 요청 하나 = 캐시 락 2번 → 요청 수 = 획득 수 ÷ 2
    def wait_per_req(s, c):
        vals = []
        for r in groups.get((s, c), []):
            a, w = num(r, "cache_acquires"), num(r, "cache_wait_us")
            if a and w is not None:
                vals.append(w / (a / 2))
        return statistics.median(vals) if vals else None
    table("⑨ 요청 하나가 캐시 락을 기다린 시간 (us, 모든 요청 평균)", shards, concs, wait_per_req,
          lambda v: f"{v:.4f}", "⑤의 요청당 CPU 시간과 비교")
    if wait_per_req(base, concs[-1]) is not None and med(base, concs[-1], "proxy_us_per_req"):
        wq, cpu = wait_per_req(base, concs[-1]), med(base, concs[-1], "proxy_us_per_req")
        print(f"  → 샤드 {base}개·동시 {concs[-1]}: 요청 하나에 락 대기 {wq:.3f}us / 프록시 CPU {cpu:.1f}us"
              f" = {100 * wq / cpu:.3f}%. 락을 없애도 처리량은 이 비율 이상 늘 수 없다 (암달의 법칙)")

    if has("main_cores"):
        table("⑩ 메인(accept) 스레드가 쓴 CPU (코어)", shards, concs, lambda s, c: med(s, c, "main_cores"),
              lambda v: f"{v:.2f}", "1.00에 가까우면 연결을 받는 이 스레드가 병목")
        table("⑩-2 요청 하나에 메인 스레드 CPU (us)", shards, concs,
              lambda s, c: med(s, c, "main_us_per_req"), lambda v: f"{v:.1f}")
    else:
        print("\n⑩ 메인(accept) 스레드 CPU: 이 CSV에는 없음 (7단계 exp_lock.sh로 다시 재면 나온다)")

    bp = bench_path(args.csv[0])
    if bp:
        brows = {int(r["shards"]): r for r in read_rows([bp])}
        cmax = concs[-1]
        print(f"\n⑪ 같은 CPU에서 자료구조만 잰 배율 대 프록시 전체 배율 (동시 {cmax})  ← {bp}")
        print(rjust("샤드", 12) + rjust("자료구조(연산/s)", 18) + rjust("배율", 10) + rjust("경합률", 10)
              + rjust("프록시 전체 배율", 18))
        for s in shards:
            b = brows.get(s)
            e2e = ratio(s, cmax, "rps")
            if b is None:
                continue
            print(rjust(str(s), 12) + rjust(f"{float(b['ops_per_sec']):,.0f}", 18)
                  + rjust(f"{float(b['ratio']):.2f}x", 10) + rjust(f"{float(b['contended_pct']):.1f}%", 10)
                  + rjust("-" if e2e is None else f"{e2e:.3f}x", 18))
    else:
        print("\n⑪ 자료구조 벤치마크 비교: 같은 이름의 _bench.csv가 없음 (7단계 exp_lock.sh가 함께 만든다)")

    print()
    failed = sum(int(r["failed"]) for r in rows)
    if failed:
        print(f"※ 실패한 요청이 {failed}개 있습니다. CSV의 failed 열을 확인하세요.")
    if nl_:
        lmax = max(num(r, "loader_cores") or 0 for r in rows)
        if lmax > 0.85 * nl_:
            print(f"※ 부하 도구가 CPU {nl_}개 중 {lmax:.2f}개를 썼습니다. "
                  "부하 도구가 먼저 포화됐을 수 있으니 높은 동시 연결 수의 결과는 주의해서 보세요.")
        else:
            print(f"부하 도구 CPU 사용 최대 {lmax:.2f}코어 / {nl_}개 → 부하 도구는 포화되지 않음")
    return 0


if __name__ == "__main__":
    sys.exit(main())
