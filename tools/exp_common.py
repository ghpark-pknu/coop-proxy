"""실험 결과 도구가 함께 쓰는 함수 (7단계) — lock_summary.py, cluster_summary.py, plot_results.py

- 결과 CSV와 실험 환경 파일(.env) 읽기
- 프록시와 똑같은 해시(FNV-1a, mix64)로 URL이 들어갈 샤드를 계산해서
  경합률 예측값 Σ(샤드별 요청 몫)²을 구한다
"""
import csv
import os
import re
import statistics

MASK = (1 << 64) - 1


# ---------------------------------------------------------------- 해시 (include/hash.h와 같은 계산)
def fnv1a(s):
    h = 1469598103934665603
    for b in s.encode():
        h ^= b
        h = (h * 1099511628211) & MASK
    return h


def mix64(k):
    k ^= k >> 33
    k = (k * 0xFF51AFD7ED558CCD) & MASK
    k ^= k >> 33
    k = (k * 0xC4CEB9FE1A85EC53) & MASK
    k ^= k >> 33
    return k


def shard_of(url, shards):
    """cache.c의 shard_for와 같다: mix64(FNV-1a(URL)) % 샤드 수"""
    return mix64(fnv1a(url)) % shards


def zipf_weights(n, s):
    w = [(r + 1) ** -s for r in range(n)]
    t = sum(w)
    return [x / t for x in w]


def shard_shares(origin, keys, zipf, shards):
    """loadc가 보내는 요청(순위 r → http://원서버/obj/r.bin)이 샤드마다 차지하는 비율"""
    w = zipf_weights(keys, zipf)
    p = [0.0] * shards
    for r in range(keys):
        p[shard_of(f"http://{origin}/obj/{r:04d}.bin", shards)] += w[r]
    return p


def sum_sq(origin, keys, zipf, shards):
    """두 요청이 같은 샤드에 떨어질 확률 Σ p_s². 경합이 드문 영역에서 경합률은 이 값에 비례한다"""
    return sum(x * x for x in shard_shares(origin, keys, zipf, shards))


def hot_share(origin, keys, zipf, shards):
    """1위 객체가 든 샤드의 요청 몫"""
    p = shard_shares(origin, keys, zipf, shards)
    return p[shard_of(f"http://{origin}/obj/0000.bin", shards)]


# ---------------------------------------------------------------- 파일
def read_rows(paths):
    rows = []
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            rows += list(csv.DictReader(f))
    return rows


def read_env(csv_path):
    """results/lock_X.csv 옆의 results/lock_X.env를 읽는다 (없으면 빈 dict).
    한 줄에 "키=값 키=값"이 여러 개 있을 수 있고 값에 공백이 있을 수 있다"""
    env_path = re.sub(r"\.csv$", ".env", csv_path)
    env = {}
    if not os.path.exists(env_path):
        return env
    with open(env_path, encoding="utf-8") as f:
        for line in f:
            for m in re.finditer(r"(\w+)=(.*?)(?=\s+\w+=|$)", line.strip()):
                env[m.group(1)] = m.group(2).strip()
    return env


def bench_path(csv_path):
    p = re.sub(r"\.csv$", "_bench.csv", csv_path)
    return p if os.path.exists(p) else None


def num(r, key):
    """CSV 칸을 숫자로. 칸이 없거나(옛 형식) 비어 있으면 None"""
    v = r.get(key)
    if v in (None, ""):
        return None
    return float(v)


def group_by(rows, *keys):
    g = {}
    for r in rows:
        g.setdefault(tuple(r[k] for k in keys), []).append(r)
    return g


def median_of(rs, key):
    vals = [num(r, key) for r in rs]
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def ncpus(spec):
    """taskset 형식 "0-1,4-5" → 4"""
    n = 0
    for part in (spec or "").split(","):
        part = part.strip()
        if not part or not part[0].isdigit():
            continue
        a, _, b = part.partition("-")
        n += int(b or a) - int(a) + 1
    return n
