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
```

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

## 개발 환경

- Windows + WSL2, Ubuntu 24.04
- gcc 13, GNU make, gdb, Valgrind, AddressSanitizer, ThreadSanitizer
- VS Code (WSL 확장, C/C++ 확장, Python 확장)
- Python 3 + matplotlib (실험 도구)

환경 점검: `./scripts/check_env.sh`
