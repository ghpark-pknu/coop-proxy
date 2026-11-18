# 협력형 웹 캐싱 프록시 클러스터의 구현과 성능 분석
— 소켓 API로 구현한 미니 CDN

컴퓨터네트워크 Term Project · 부경대학교

## 폴더 구조

```
coop-proxy/
├─ src/          프록시 C 소스
├─ include/      헤더 파일
├─ tools/        실험 도구 (Python): 원서버, 파일 생성기, 부하 생성기, 그래프
├─ conf/         노드 설정, 차단 목록
├─ web/          관제 화면 (dashboard.html, 프록시가 /__dashboard로 제공)
├─ scripts/      check_env.sh 등 보조 스크립트
├─ sandbox/      환경 검증용 hello_server.c (본 프로젝트와 별개)
├─ www/          원서버가 제공할 테스트 파일 (git 제외, 스크립트로 생성)
├─ results/      실험 결과 (git 제외)
└─ build/        빌드 결과물 (git 제외)
```

## 빌드

| 명령 | 용도 | 결과물 |
|---|---|---|
| `make` | 개발·디버깅 | `build/debug/proxy` |
| `make MODE=asan` | 메모리 오류 검사 | `build/asan/proxy` |
| `make MODE=tsan` | 데이터 경쟁 검사 | `build/tsan/proxy` |
| `make MODE=release` | 성능 측정 | `build/release/proxy` |
| `make hello` | 환경 검증용 서버 | `build/debug/hello_server` |
| `make rtest` | 랑데부 해싱 단위 테스트 | `build/debug/rendezvous_test` |
| `make ctest` | 캐시(샤딩) 단위 테스트·마이크로 벤치마크 | `build/debug/cache_test` |
| `make MODE=release loadc` | C 부하 도구 (처리량 측정) | `build/release/loadc` |
| `make clean` | 빌드 결과물 삭제 | |

## 실행

```bash
python3 tools/gen_files.py                      # 테스트 파일 생성 → ~/coop-data/www
python3 tools/origin.py --port 9000             # 터미널 A: 원서버
./build/debug/proxy -p 8080 -b conf/blocklist.txt   # 터미널 B: 프록시
curl -i -x http://127.0.0.1:8080 http://127.0.0.1:9000/page.html   # 터미널 C (X-Cache 확인)
./scripts/test_stage1.sh                        # 1단계(중계) 자동 점검
./scripts/test_stage2.sh                        # 2단계(캐시) 자동 점검
./scripts/test_stage3.sh                        # 3단계(클러스터) 자동 점검
./scripts/test_stage4.sh                        # 4단계(관제·부하 생성기) 자동 점검
./scripts/test_stage5.sh                        # 5단계(락 전략: 전역 mutex / 샤딩) 자동 점검
./scripts/test_stage6.sh                        # 6단계 측정 도구(loadc) 점검
./scripts/test_stage7.sh                        # 7단계 실험 분석 도구 점검 (실험 스크립트를 아주 작게 실제로 돌림)
```

> 코드를 통째로 다른 시점으로 바꾼 뒤(`git reset`, 다른 커밋 `checkout`, 압축 덮어쓰기)에는
> `make clean`부터 한다. make는 파일 수정 시각으로만 판단하므로, 옛 `.o`가 남아 섞일 수 있다.

### 클러스터 (미니 CDN)

```bash
./scripts/cluster.sh start                      # 원서버 + node1·node2·node3 (conf/cluster.conf)
./build/debug/rendezvous_test rank http://127.0.0.1:9000/obj/0000.bin   # 담당 노드 확인
curl -i -x http://127.0.0.1:8081 http://127.0.0.1:9000/obj/0000.bin    # X-Cache 경로 확인
./scripts/cluster.sh stop node2                 # 장애 시험
./scripts/cluster.sh stop                       # 전체 종료 (노드별 통계 출력)
CLUSTER=0 ./scripts/cluster.sh start            # 비교용: 독립 프록시 3개
make rtest && ./build/debug/rendezvous_test     # 랑데부 해싱 단위 테스트·분포·재배치 비교
```

| 옵션 | 뜻 | 기본값 |
|---|---|---|
| `-p PORT` | 대기 포트 | 8080 |
| `-n NAME` | 노드 이름 | node1 |
| `-c MS` | 원서버 연결 제한 시간 | 3000 |
| `-t MS` | 송수신 제한 시간 | 5000 |
| `-s SIZE` | 캐시 용량 (예: 64M, 0이면 캐시 끔) | 64M |
| `-o SIZE` | 객체 하나의 상한 | 1M |
| `-S N` | 캐시 샤드 수 1~64 (1 = 전역 mutex, 2 이상 = 샤드마다 mutex) | 1 |
| `-v SEC` | 검증 주기 | 60 |
| `-b FILE` | 차단 목록 | 없음 |
| `-C FILE` | 노드 목록 (주면 클러스터 모드, `-n` 이름이 목록에 있어야 함) | 없음 |
| `-r MS` | 연결 실패한 노드를 건너뛰는 시간 | 5000 |
| `-q` | 요청 기록 끄기 | 꺼짐 |

### 실시간 관제와 부하 생성 (4단계)

```bash
./scripts/cluster.sh start                      # 원서버 + 클러스터 (관제 화면 주소를 출력)
# 브라우저: http://localhost:8081/__dashboard     (노드 아무 곳이나: 8082, 8083도 같은 화면)
curl -s http://127.0.0.1:8081/__stats | python3 -m json.tool    # 통계 JSON
python3 tools/loadgen.py -d 60 -c 4 --rate 300  # 60초 동안 초당 300개 → 그래프가 움직인다
python3 tools/loadgen.py -n 3000 -c 1 --stats   # 노드 통계와 맞춰 보기 (불변식 4개 확인)
python3 tools/loadgen.py --dry-run -n 20000     # 요청열 분석: Zipf 이론값, 원서버 요청 예측
CLUSTER=0 ./scripts/cluster.sh restart          # 비교 실험: 같은 요청열을 독립 프록시 3개에
# 브라우저: http://localhost:8081/__dashboard?nodes=127.0.0.1:8081,127.0.0.1:8082,127.0.0.1:8083
```

| 관제 경로 | 응답 |
|---|---|
| `/__stats` | 통계 JSON (결과별 요청 수, 응답 시간 히스토그램, 캐시, 클러스터 노드 목록) |
| `/__dashboard` | 실시간 관제 화면 (모든 노드의 `/__stats`를 1초마다 조회해 합친다) |
| `/` | `/__dashboard`로 이동 (302) |

부하 생성기 주요 옵션: `-n` 요청 수 / `-d` 시간(초) / `-c` 동시 연결 / `--rate` 초당 상한 /
`--zipf` 지수(기본 0.8) / `--seed` 시드(같으면 같은 요청열) / `--warmup` 예열 /
`--stats` 노드 통계 확인 / `--csv`, `--json` 결과 저장 / `--no-retry` 진입 노드 재연결 끔.

### 캐시 락 전략 (5단계)

```bash
./build/debug/proxy -p 8080 -S 8                # 캐시를 샤드 8개로: 샤드마다 mutex
PROXY_ARGS="-S 8" ./scripts/cluster.sh start    # 클러스터 노드 모두 -S 8
curl -s http://127.0.0.1:8080/__stats | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['cache']['shard_objects'], d['locks'])"
make MODE=release ctest && ./build/release/cache_test bench          # 네트워크 없이 캐시만: 샤드 수별 처리량
./build/release/cache_test bench -t 8 -d 3 -S 1,2,4,8,16,32 -z 0     # 스레드 8, 3초, 균등 분포
```

| 샤드 수 | 락 | 용량 | 교체 |
|---|---|---|---|
| `-S 1` (기본) | 전역 mutex 1개 | 캐시 전체 | 캐시 전체에서 LRU (2~4단계와 같음) |
| `-S N` | 샤드마다 mutex | 샤드마다 전체의 1/N | 샤드 안에서만 LRU (근사 LRU) |

- URL마다 `mix64(FNV-1a(URL)) % N`으로 샤드를 정한다 (버킷 번호와 다른 비트를 쓰기 위해 한 번 더 섞는다).
- `-S`가 2 이상이면 샤드 하나의 용량(`-s` ÷ N)이 객체 상한(`-o`) 이상이어야 한다. 아니면 실행을 거부한다
  (예: `-s 4M -S 8`은 샤드가 512KB라서 `-o 512K` 이하를 함께 줘야 한다).
- `/__stats`의 `locks`: 락을 잡은 횟수(`acquires`), 그중 다른 스레드가 잡고 있어 기다린 횟수(`contended`),
  기다린 시간 합(`wait_us`). `cache`는 모든 샤드의 합, `stats`는 요청마다 한 번 잡는 통계 락.
  `cache`에는 `shards`, `shard_objects`, `shard_bytes`(샤드별 객체 수·바이트)가 추가된다.

### 고부하 측정과 락 전략 실험 (6단계)

```bash
make MODE=release all loadc
./build/release/loadc -x 127.0.0.1:8080 -c 32 -d 10 -w 2 --prefill --pid $(pgrep -f "proxy -p 8080")
QUICK=1 ./scripts/exp_lock.sh                   # 빠른 확인 (약 30초)
./scripts/exp_lock.sh                           # 본 실험: 샤드 1·2·4·8·16 × 동시 1~64, 반복 3번 (약 25분)
python3 tools/lock_summary.py results/lock_*.csv   # 결과 CSV를 다시 표로
```

- `loadc`: 스레드 N개가 "연결 → 요청 → 응답 끝까지 받기 → 닫기"를 쉬지 않고 반복한다 (닫힌 루프).
  응답 시간 백분위, 결과(X-Cache)별 개수, 부하 도구와 프록시(`--pid`)의 CPU 사용량,
  요청 하나에 쓴 프록시 CPU 시간을 낸다. 응답 내용 검증은 `loadgen.py`의 몫이다.
- `exp_lock.sh`: 물리 코어를 반으로 나눠 프록시와 부하 도구를 따로 묶는다 (`taskset`).
  1KB 객체 200개를 미리 캐시에 넣고 HIT만 재므로 "요청 처리 + 캐시 락"만 남는다.
  결과는 `results/lock_날짜_시각.csv`와 실험 환경 `.env`.

### 실험 분석과 그래프 (7단계)

```bash
./scripts/exp_lock.sh                                   # 실험 ①: 락 전략 (메인 스레드 CPU, 같은 CPU의 자료구조 벤치마크 포함)
SHARDS="1 2 4 16" CONCS="8 16 32 64" REPEAT=7 DUR=15 ./scripts/exp_lock.sh   # 정밀 측정: 조건을 줄이고 반복을 늘림
python3 tools/lock_summary.py results/lock_X.csv        # 표 ①~⑪ (6단계 형식 CSV도 읽음)
python3 tools/plot_results.py lock results/lock_X.csv   # 그림 → results/lock_X_figs/

QUICK=1 ./scripts/exp_cluster.sh                        # 실험 ②: 독립 프록시 3개 대 클러스터 3노드 (빠른 확인)
./scripts/exp_cluster.sh                                # 본 실험: 노드당 512K~16M, 원서버 지연 50ms (약 10분)
python3 tools/cluster_summary.py results/cluster_X.csv
python3 tools/plot_results.py cluster results/cluster_X.csv
```

| 도구 | 하는 일 |
|---|---|
| `tools/lock_summary.py` | 반복의 중앙값으로 표: 처리량과 흩어짐, 배율, 경합률, **경합률 감소의 예측 Σ(샤드별 요청 몫)² 대 관측**, 요청당 락 대기 시간, 메인(accept) 스레드 CPU, 자료구조 배율 대 프록시 전체 배율 |
| `tools/cluster_summary.py` | 노드당 용량별 히트율, 원서버 요청(1000개당), 평균·p50·p99, HIT만의 응답 시간(다른 노드를 거치는 비용), 캐시 객체 수 |
| `tools/plot_results.py` | 보고서용 PNG. 한글 글꼴이 없으면 Windows의 맑은 고딕을 WSL에서 불러오고, 그래도 없으면 `sudo apt install fonts-nanum` 안내 |
| `tools/exp_common.py` | 위 도구들이 함께 쓰는 함수: 결과 파일 읽기, 프록시와 같은 해시(FNV-1a, mix64)로 샤드 계산 |

## 개발 환경

- Windows + WSL2, Ubuntu 24.04
- gcc 13, GNU make, gdb, Valgrind, AddressSanitizer, ThreadSanitizer
- VS Code (WSL 확장, C/C++ 확장, Python 확장)
- Python 3 + matplotlib (실험 도구)

환경 점검: `./scripts/check_env.sh`
