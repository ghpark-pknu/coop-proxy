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

## 개발 환경

- Windows + WSL2, Ubuntu 24.04
- gcc 13, GNU make, gdb, Valgrind, AddressSanitizer, ThreadSanitizer
- VS Code (WSL 확장, C/C++ 확장, Python 확장)
- Python 3 + matplotlib (실험 도구)

환경 점검: `./scripts/check_env.sh`
