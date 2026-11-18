#!/usr/bin/env python3
"""실험 ② 결과 요약: 독립 프록시 3개 대 협력형 클러스터 3노드 (7단계)
scripts/exp_cluster.sh가 만든 CSV를 노드 하나의 캐시 용량별 표로 보여 준다. 반복은 중앙값으로 합친다.

  python3 tools/cluster_summary.py results/cluster_20261118_140000.csv
"""
import argparse
import sys
import unicodedata

from exp_common import group_by, median_of, read_env, read_rows

NAMES = {"independent": "독립", "cluster": "클러스터"}


def dwidth(s):
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in s)


def rjust(s, w):
    return " " * max(0, w - dwidth(s)) + s


def main():
    ap = argparse.ArgumentParser(description="독립 대 클러스터 실험 요약")
    ap.add_argument("csv", nargs="+")
    args = ap.parse_args()
    rows = read_rows(args.csv)
    if not rows:
        print("결과가 없습니다")
        return 1
    env = read_env(args.csv[0])
    g = group_by(rows, "mode", "size_bytes")
    sizes = sorted({int(k[1]) for k in g})
    label = {int(r["size_bytes"]): r["size"] for r in rows}
    med = lambda mode, sz, key: median_of(g.get((mode, str(sz)), []), key)

    if env:
        print(f"조건: 노드 3개, 객체 {env.get('objects', '?')}개({env.get('objects_mb', '?')}MB), "
              f"원서버 지연 {env.get('delay', '?')}초, 요청 {env.get('requests', '?')}개(예열 {env.get('warmup', '?')}개 제외), "
              f"동시 {env.get('conc', '?')}, 반복 {env.get('repeat', '?')}번의 중앙값"
              + (f", 요청 속도 상한 초당 {float(env['rate']):g}개" if float(env.get("rate") or 0) > 0 else ""))

    cols = [("히트율", "hit_ratio", lambda v: f"{100 * v:.1f}%"),
            ("원서버/1000", "origin_per_1000", lambda v: f"{v:.1f}"),
            ("평균 ms", "mean_ms", lambda v: f"{v:.2f}"),
            ("p50 ms", "p50_ms", lambda v: f"{v:.2f}"),
            ("HIT 요청만 평균 ms (다른 노드를 거치는 비용)", "hit_mean_ms", lambda v: f"{v:.3f}"),
            ("MISS 요청만 평균 ms (원서버 왕복 포함)", "miss_mean_ms", lambda v: f"{v:.2f}"),
            ("p99 ms", "p99_ms", lambda v: f"{v:.1f}"),
            ("캐시 객체", "objects", lambda v: f"{v:.0f}")]
    for title, key, fmt in cols:
        print(f"\n{title}")
        print(rjust("노드당 용량", 12) + rjust("독립", 12) + rjust("클러스터", 12) + rjust("독립 ÷ 클러스터", 18))
        for sz in sizes:
            a, b = med("independent", sz, key), med("cluster", sz, key)
            ratio = f"{a / b:.2f}" if a is not None and b else "-"
            print(rjust(label[sz], 12) + rjust("-" if a is None else fmt(a), 12)
                  + rjust("-" if b is None else fmt(b), 12) + rjust(ratio, 18))

    print("\n읽는 법")
    print("  - 클러스터는 URL마다 담당 노드 한 곳에만 저장한다 → 노드 3개의 용량이 합쳐진 하나의 캐시처럼 동작")
    print("  - 독립 프록시는 같은 객체를 노드마다 따로 받아 따로 저장한다 → 실제 용량은 노드 하나만큼")
    print("  - 대신 클러스터는 요청의 약 2/3가 다른 노드를 한 번 더 거친다 (HIT여도 왕복이 하나 늘어난다)")
    failed = sum(int(r["failed"]) for r in rows)
    mism = sum(1 for r in rows if r.get("loadgen_rc") == "3")
    if failed:
        print(f"※ 실패한 요청이 {failed}개 있습니다")
    if mism:
        print(f"※ 노드 통계 확인이 맞지 않은 측정이 {mism}번 있습니다 (loadgen 종료 코드 3)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
