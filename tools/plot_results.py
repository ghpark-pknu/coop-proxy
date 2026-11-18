#!/usr/bin/env python3
"""실험 결과 그래프 (7단계) — 보고서에 넣을 PNG를 만든다.

  python3 tools/plot_results.py lock    results/lock_X.csv       → results/lock_X_figs/
  python3 tools/plot_results.py cluster results/cluster_X.csv    → results/cluster_X_figs/

같은 조건을 여러 번 잰 값은 중앙값을 점으로, 최소~최대를 세로 막대(오차 막대)로 그린다.

그림 규칙 (보고서 인쇄용 정적 그림)
  - 색은 계열마다 정해진 순서로만 쓴다 (1 파랑, 2 주황, 3 청록, 4 노랑, 5 분홍): 색맹 시뮬레이션으로
    이웃한 색끼리 구별되는지 검증된 순서. 샤드 1·2·4·8·16 = 1~5번 색, 항상 같은 샤드 = 같은 색
  - y축은 하나만 (단위가 다르면 그림을 나눈다), 계열이 둘 이상이면 범례
  - 선 1.5pt, 점에는 바탕색 테두리, 격자는 옅은 실선
한글 글꼴: 나눔고딕·Noto Sans CJK KR을 찾고, 없으면 Windows의 맑은 고딕(/mnt/c/Windows/Fonts/malgun.ttf)을
WSL에서 직접 불러온다. 그것도 없으면 'sudo apt install fonts-nanum'을 안내한다.
"""
import argparse
import os
import re
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt                     # noqa: E402
from matplotlib import font_manager                 # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter  # noqa: E402

from exp_common import (bench_path, group_by, median_of, num, read_env, read_rows,  # noqa: E402
                        sum_sq)

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
SURFACE, TEXT, TEXT2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"


def setup_style():
    for path in ("/mnt/c/Windows/Fonts/malgun.ttf", "/usr/share/fonts/truetype/nanum/NanumGothic.ttf"):
        if os.path.exists(path):
            font_manager.fontManager.addfont(path)
    names = {f.name for f in font_manager.fontManager.ttflist}
    family = next((c for c in ("NanumGothic", "Malgun Gothic", "Noto Sans CJK KR", "Noto Sans KR",
                               "Apple SD Gothic Neo", "UnDotum") if c in names), None)
    if family is None:
        print("※ 한글 글꼴을 찾지 못해 한글이 네모로 나올 수 있습니다: sudo apt install fonts-nanum", file=sys.stderr)
    plt.rcParams.update({
        "font.family": [family] if family else ["DejaVu Sans"],
        "font.size": 9.5,
        "axes.unicode_minus": False,
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": AXIS, "axes.linewidth": 0.8, "axes.labelcolor": TEXT2,
        "axes.titlesize": 10.5, "axes.titlecolor": TEXT, "axes.titlelocation": "left", "axes.titlepad": 8,
        "axes.spines.top": False, "axes.spines.right": False,
        "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "grid.linestyle": "-",
        "axes.axisbelow": True,
        "xtick.color": AXIS, "ytick.color": AXIS, "xtick.labelcolor": TEXT2, "ytick.labelcolor": TEXT2,
        "legend.frameon": False, "legend.labelcolor": TEXT2, "legend.fontsize": 9,
        "lines.linewidth": 1.5, "lines.markersize": 6,
    })


def count_axis(ax):
    """y축 눈금: 축이 10,000에 닿으면 모두 K 단위(0, 5K, 10K), 아니면 모두 그대로(0, 2,000, 4,000).
    한 축 안에서 두 표기를 섞지 않는다"""
    compact = ax.get_ylim()[1] >= 10000
    ax.yaxis.set_major_formatter(FuncFormatter(
        lambda v, _p: (f"{v / 1000:g}K" if v else "0") if compact else f"{v:,.0f}"))


def log2_axis(ax, values, fmt=str):
    ax.set_xscale("log", base=2)
    ax.set_xticks(values)
    ax.set_xticklabels([fmt(v) for v in values])
    ax.minorticks_off()


def line(ax, xs, ys, color, label=None, lo=None, hi=None, jitter=1.0):
    """중앙값을 선과 점으로, 최소~최대를 같은 색의 가는 세로 막대로 (값이 없는 점은 건너뜀)"""
    keep = [i for i, y in enumerate(ys) if y is not None]
    xj = [xs[i] * jitter for i in keep]
    ys = [ys[i] for i in keep]
    if lo is not None:
        lo = [lo[i] for i in keep]
        hi = [hi[i] for i in keep]
        ax.vlines(xj, lo, hi, color=color, linewidth=1.0, alpha=0.55, zorder=2)
    ax.plot(xj, ys, color=color, marker="o", markeredgecolor=SURFACE, markeredgewidth=1.2,
            label=label, zorder=3, solid_capstyle="round", solid_joinstyle="round")


def save(fig, out_dir, name, note):
    path = os.path.join(out_dir, name)
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"  {path}  — {note}")


def stem_dir(csv_path):
    d = re.sub(r"\.csv$", "_figs", csv_path)
    os.makedirs(d, exist_ok=True)
    return d


def stats_by(groups, key, field):
    """(중앙값, 최소, 최대)"""
    vals = [num(r, field) for r in groups.get(key, [])]
    vals = [v for v in vals if v is not None]
    if not vals:
        return None, None, None
    return statistics.median(vals), min(vals), max(vals)


# ---------------------------------------------------------------- 실험 ①: 락 전략
def plot_lock(csv_path):
    rows = read_rows([csv_path])
    env = read_env(csv_path)
    origin = env.get("origin", "127.0.0.1:19600")
    keys, zipf = int(env.get("keys", 200)), float(env.get("zipf", 0.8))
    g = {(int(s), int(c)): v for (s, c), v in group_by(rows, "shards", "conc").items()}
    shards = sorted({k[0] for k in g})
    concs = sorted({k[1] for k in g})
    base, cmax = shards[0], concs[-1]
    out = stem_dir(csv_path)
    cond = (f"{env.get('cpu_model', '')} · 프록시 CPU {env.get('proxy_cpus', '?')} · "
            f"1KB 객체 {keys}개 HIT만 · Zipf {zipf} · 반복 {max(len(v) for v in g.values())}번의 중앙값(점)과 최소~최대(막대)")
    label = lambda s: f"샤드 {s}개" + (" (전역 mutex)" if s == 1 else "")
    color = lambda i: SERIES[i % len(SERIES)]
    jit = lambda i: 2 ** ((i - (len(shards) - 1) / 2) * 0.035)     # 오차 막대가 겹치지 않게 조금씩 비킴
    print(f"그림 → {out}/")

    # 그림 1: 처리량과 p99 (단위가 달라 두 칸으로)
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.9))
    for i, s in enumerate(shards):
        for ax, field in ((axes[0], "rps"), (axes[1], "p99_ms")):
            st = [stats_by(g, (s, c), field) for c in concs]
            line(ax, concs, [m for m, _, _ in st], color(i), label(s) if ax is axes[0] else None,
                 [lo for _, lo, _ in st], [hi for _, _, hi in st], jit(i))
    for ax, title in ((axes[0], "처리량 (요청/s)"), (axes[1], "응답 시간 p99 (ms)")):
        log2_axis(ax, concs)
        ax.set_xlabel("동시 연결 수")
        ax.set_title(title)
        ax.set_ylim(bottom=0)
    count_axis(axes[0])
    fig.legend(loc="upper center", ncol=len(shards), bbox_to_anchor=(0.5, 1.08))
    fig.text(0.0, -0.04, cond, color=MUTED, fontsize=7.5)
    save(fig, out, "lock_throughput_p99.png", "샤드 수별 처리량과 p99")

    # 그림 2: 캐시 락·통계 락 경합률과 Σp² 예측 (같은 단위 %)
    c_obs = [stats_by(g, (s, cmax), "cache_contended_pct")[0] for s in shards]
    s_obs = [stats_by(g, (s, cmax), "stats_contended_pct")[0] for s in shards]
    pos = lambda vals: [v if v is not None and v > 0 else None for v in vals]   # 로그 눈금: 0은 못 그린다
    enough = all((num(r, "cache_contended") or 0) >= 100 for r in g.get((base, cmax), []))
    if not enough:
        print(f"  (경합률 그림 생략: 샤드 {base}개·동시 {cmax}의 캐시 락 경합이 100회 미만이라 비율을 믿을 수 없음)")
    if c_obs[0] and enough:
        fig, ax = plt.subplots(figsize=(6.4, 3.9))
        pred = [c_obs[0] * sum_sq(origin, keys, zipf, s) / sum_sq(origin, keys, zipf, base) for s in shards]
        ax.plot(shards, pred, color=MUTED, linewidth=1.0, label="예측: 샤드 1개 값 × Σ(샤드별 요청 몫)²", zorder=2)
        line(ax, shards, pos(c_obs), color(0), f"캐시 락 (동시 {cmax})")
        if any(pos(s_obs)):
            line(ax, shards, pos(s_obs), color(1), f"통계 락 (동시 {cmax})")
        log2_axis(ax, shards)
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1.0, 2.0, 5.0)))
        ax.yaxis.set_minor_formatter(NullFormatter())
        ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}%"))
        ax.set_xlabel("캐시 샤드 수")
        ax.set_title("락 경합률: 락을 잡으려 할 때 다른 스레드가 잡고 있던 비율")
        ax.legend(loc="lower left")
        save(fig, out, "lock_contention.png", "샤드 수별 경합률, 예측 대 관측")

    # 그림 3: 자료구조만 잰 배율 대 프록시 전체 배율
    bp = bench_path(csv_path)
    if bp:
        b = {int(r["shards"]): float(r["ratio"]) for r in read_rows([bp])}
        e2e = [stats_by(g, (s, cmax), "rps")[0] / stats_by(g, (base, cmax), "rps")[0] for s in shards]
        bench = [b.get(s) for s in shards]
        fig, ax = plt.subplots(figsize=(6.4, 3.9))
        xs = range(len(shards))
        w = 0.26
        bars1 = ax.bar([x - w / 2 - 0.04 for x in xs], bench, w, color=color(0),
                       label=f"자료구조만 (cache_test bench, 스레드 {read_rows([bp])[0]['threads']}개)")
        bars2 = ax.bar([x + w / 2 + 0.04 for x in xs], e2e, w, color=color(1),
                       label=f"프록시 전체 (loadc, 동시 {cmax})")
        for bars in (bars1, bars2):
            for r in bars:
                ax.text(r.get_x() + r.get_width() / 2, r.get_height(), f"{r.get_height():.2f}",
                        ha="center", va="bottom", fontsize=7.5, color=TEXT2)
        ax.axhline(1.0, color=AXIS, linewidth=0.8, zorder=1)
        ax.set_xticks(list(xs))
        ax.set_xticklabels([str(s) for s in shards])
        ax.set_xlabel("캐시 샤드 수")
        ax.set_title(f"샤드 {base}개 대비 처리량 배율: 같은 CPU, 같은 URL")
        ax.set_ylim(0, max(v for v in bench + e2e if v) * 1.35)     # 위쪽은 범례 자리
        ax.grid(axis="x", visible=False)
        ax.legend(loc="upper left")
        save(fig, out, "lock_layers.png", "자료구조 수준 대 프록시 전체")

    # 그림 4: 요청 하나의 프록시 CPU 시간 (샤드 1개) — 메인 스레드와 나머지
    tot = [stats_by(g, (base, c), "proxy_us_per_req")[0] for c in concs]
    main = [stats_by(g, (base, c), "main_us_per_req")[0] for c in concs]
    wait = [None] * len(concs)
    for j, c in enumerate(concs):
        ws = [num(r, "cache_wait_us") / (num(r, "cache_acquires") / 2)
              for r in g.get((base, c), []) if num(r, "cache_acquires")]
        wait[j] = statistics.median(ws) if ws else None
    if all(t is not None for t in tot):
        fig, ax = plt.subplots(figsize=(6.4, 3.9))
        xs = range(len(concs))
        if all(m is not None for m in main):
            rest = [t - m for t, m in zip(tot, main)]
            ax.bar(xs, main, 0.45, color=color(1), label="메인(accept) 스레드: 연결 받기·작업 스레드 만들기",
                   edgecolor=SURFACE, linewidth=1.0)
            ax.bar(xs, rest, 0.45, bottom=main, color=color(0), label="작업 스레드와 커널: 요청 처리·전송·종료",
                   edgecolor=SURFACE, linewidth=1.0)
            ax.legend(loc="upper left")
        else:
            ax.bar(xs, tot, 0.45, color=color(0))
        for x, t in zip(xs, tot):
            ax.text(x, t, f"{t:.0f}", ha="center", va="bottom", fontsize=8, color=TEXT2)
        ax.set_xticks(list(xs))
        ax.set_xticklabels([str(c) for c in concs])
        ax.set_xlabel("동시 연결 수")
        ax.set_title(f"요청 하나에 쓴 프록시 CPU 시간 (us), 샤드 {base}개")
        ax.set_ylim(0, max(tot) * 1.55)                              # 위쪽은 범례 자리
        ax.grid(axis="x", visible=False)
        if wait[-1] is not None:
            fig.text(0.0, -0.04, f"같은 조건에서 요청 하나가 캐시 락을 기다린 시간: 동시 {cmax}일 때 {wait[-1]:.3f}us "
                                 f"(CPU 시간의 {100 * wait[-1] / tot[-1]:.3f}%)", color=MUTED, fontsize=7.5)
        save(fig, out, "lock_cpu_per_request.png", "요청당 CPU 시간 구성")


# ---------------------------------------------------------------- 실험 ②: 독립 대 클러스터
def size_label(b):
    b = int(b)
    return f"{b // 1048576}M" if b >= 1048576 else f"{b // 1024}K"


def plot_cluster(csv_path):
    rows = read_rows([csv_path])
    env = read_env(csv_path)
    g = {(r_mode, int(sz)): v for (r_mode, sz), v in group_by(rows, "mode", "size_bytes").items()}
    sizes = sorted({k[1] for k in g})
    modes = [m for m in ("independent", "cluster") if any(k[0] == m for k in g)]
    names = {"independent": "독립 프록시 3개", "cluster": "협력형 클러스터 3노드"}
    out = stem_dir(csv_path)
    total_mb = env.get("objects_mb")
    cond = (f"노드 3개 · 원서버 지연 {env.get('delay', '?')}초 · 요청 {env.get('requests', '?')}개 "
            f"(예열 {env.get('warmup', '?')}개 제외) · 동시 {env.get('conc', '?')} · Zipf {env.get('zipf', '?')}"
            + (f" · 객체 전체 {total_mb}MB" if total_mb else ""))
    print(f"그림 → {out}/")

    def draw(ax, field, scale=1.0):
        for i, m in enumerate(modes):
            st = [stats_by(g, (m, s), field) for s in sizes]
            line(ax, sizes, [None if v is None else v * scale for v, _, _ in st], SERIES[i], names[m],
                 [None if v is None else v * scale for _, v, _ in st],
                 [None if v is None else v * scale for _, _, v in st])
        log2_axis(ax, sizes, size_label)
        ax.set_xlabel("노드 하나의 캐시 용량")

    fig, axes = plt.subplots(1, 2, figsize=(10, 3.9))
    draw(axes[0], "hit_ratio", 100)
    axes[0].set_title("히트율 (%)")
    axes[0].set_ylim(0, 100)
    draw(axes[1], "origin_per_1000")
    axes[1].set_title("원서버 요청 수 (클라이언트 요청 1000개당)")
    axes[1].set_ylim(bottom=0)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, bbox_to_anchor=(0.5, 1.08))
    fig.text(0.0, -0.04, cond, color=MUTED, fontsize=7.5)
    save(fig, out, "cluster_hit_origin.png", "히트율과 원서버 부하")

    fig, axes = plt.subplots(1, 2, figsize=(10, 3.9))
    draw(axes[0], "mean_ms")
    axes[0].set_title("평균 응답 시간 (ms)")
    draw(axes[1], "hit_mean_ms")
    axes[1].set_title("HIT 요청만의 평균 응답 시간 (ms)")
    for ax in axes:
        ax.set_ylim(bottom=0)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, bbox_to_anchor=(0.5, 1.08))
    fig.text(0.0, -0.04, cond, color=MUTED, fontsize=7.5)
    save(fig, out, "cluster_latency.png", "응답 시간")


def main():
    ap = argparse.ArgumentParser(description="실험 결과 그래프")
    ap.add_argument("kind", choices=("lock", "cluster"))
    ap.add_argument("csv")
    args = ap.parse_args()
    setup_style()
    (plot_lock if args.kind == "lock" else plot_cluster)(args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
