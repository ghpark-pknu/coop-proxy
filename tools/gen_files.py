#!/usr/bin/env python3
"""테스트 파일 생성기

원서버가 제공할 파일들을 만든다. 같은 seed면 항상 같은 내용이 만들어지므로
다른 날 다시 만들어도 실험 조건이 같다.

만드는 파일
  small.txt   1KB 텍스트         ─┐
  page.html   간단한 HTML 페이지  ├ 1단계 중계 확인용
  big.bin     5MB 이진 파일       ─┘ (여러 번의 recv/send에 걸쳐 중계되는지 확인)
  obj/NNNN.bin  크기가 다양한 객체 N개 (3단계 부하 실험용, 1KB~512KB)
  manifest.json obj 목록과 크기 (부하 생성기가 읽음)

사용 예
  python3 tools/gen_files.py                       # ~/coop-data/www 에 생성
  python3 tools/gen_files.py --count 500 --seed 7
"""
import argparse
import json
import math
import os
import random


def main():
    ap = argparse.ArgumentParser(description="테스트 파일 생성기")
    ap.add_argument("--dir", default=os.path.expanduser("~/coop-data/www"),
                    help="만들 폴더 (기본 ~/coop-data/www)")
    ap.add_argument("--count", type=int, default=200, help="obj 파일 수 (기본 200)")
    ap.add_argument("--min", type=int, default=1024, help="obj 최소 크기 (기본 1KB)")
    ap.add_argument("--max", type=int, default=512 * 1024, help="obj 최대 크기 (기본 512KB)")
    ap.add_argument("--seed", type=int, default=2026, help="난수 seed (기본 2026)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    os.makedirs(os.path.join(args.dir, "obj"), exist_ok=True)

    def write(rel, data):
        with open(os.path.join(args.dir, rel), "wb") as f:
            f.write(data)

    # 1단계 확인용 고정 파일
    write("small.txt", ("coop-proxy test line\n" * 52)[:1024].encode())
    write("page.html", (
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>origin</title></head>\n"
        "<body><h1>원서버 테스트 페이지</h1><p>이 페이지가 보이면 프록시 중계 성공</p></body></html>\n"
    ).encode("utf-8"))
    write("big.bin", rng.randbytes(5 * 1024 * 1024))

    # 부하 실험용 객체: 크기를 로그 균등 분포로 뽑는다
    # (작은 파일이 많고 큰 파일이 적은, 실제 웹과 비슷한 모양)
    lo, hi = math.log(args.min), math.log(args.max)
    objects = []
    for i in range(args.count):
        size = int(math.exp(rng.uniform(lo, hi)))
        rel = f"obj/{i:04d}.bin"
        write(rel, rng.randbytes(size))
        objects.append({"path": "/" + rel, "size": size})

    with open(os.path.join(args.dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"seed": args.seed, "objects": objects}, f, indent=1)

    total = sum(o["size"] for o in objects)
    print(f"생성 완료: {args.dir}")
    print(f"  고정 파일 3개 (small.txt, page.html, big.bin 5MB)")
    print(f"  obj 파일 {args.count}개, 합계 {total / 1024 / 1024:.1f}MB")


if __name__ == "__main__":
    main()
