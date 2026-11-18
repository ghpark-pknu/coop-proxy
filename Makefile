# =====================================================================
#  협력형 웹 캐싱 프록시 클러스터 — Makefile
#
#  make                 디버그 빌드      → build/debug/proxy
#  make MODE=asan       메모리 오류 검사 → build/asan/proxy
#  make MODE=tsan       데이터 경쟁 검사 → build/tsan/proxy
#  make MODE=release    성능 측정용      → build/release/proxy
#  make hello           환경 검증용 서버 → build/debug/hello_server
#  make rtest           랑데부 해싱 단위 테스트 → build/debug/rendezvous_test
#  make ctest           캐시(샤딩) 단위 테스트·벤치마크 → build/debug/cache_test
#  make clean           build/ 폴더 전체 삭제
#
#  주의: 명령 줄(레시피)의 들여쓰기는 반드시 "탭" 문자여야 한다.
# =====================================================================

CC      := gcc
MODE    ?= debug

# 공통 옵션
#  -std=gnu17      C17 + GNU 확장(POSIX 함수 선언을 쓰기 편함)
#  -Wall ...       경고를 최대한 켠다. 경고도 버그 후보로 취급한다
#  -g              디버깅 정보(gdb, 새니타이저 보고서의 줄 번호)
#  -pthread        pthread 사용 (컴파일과 링크 양쪽에 필요)
#  -MMD -MP        헤더 의존성 파일(.d)을 자동 생성 → 헤더만 고쳐도 다시 빌드됨
CFLAGS  := -std=gnu17 -Wall -Wextra -Wshadow -Wformat=2 -g -pthread \
           -Iinclude -MMD -MP
LDFLAGS := -pthread
LDLIBS  :=

# 빌드 모드별 옵션
ifeq ($(MODE),debug)
  CFLAGS  += -O0
else ifeq ($(MODE),asan)
  CFLAGS  += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  CFLAGS  += -O1 -fsanitize=thread
  LDFLAGS += -fsanitize=thread
else ifeq ($(MODE),release)
  CFLAGS  += -O2 -DNDEBUG
else
  $(error MODE는 debug, asan, tsan, release 중 하나여야 합니다 (입력값: $(MODE)))
endif

# 모드마다 결과물 폴더를 따로 둔다 → 모드를 바꿔도 make clean이 필요 없다
BUILD   := build/$(MODE)
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o)
DEPS    := $(OBJS:.o=.d) $(BUILD)/hello_server.d $(BUILD)/tests/rendezvous_test.d \
           $(BUILD)/tests/cache_test.d
TARGET  := $(BUILD)/proxy

.PHONY: all hello rtest ctest clean

ifeq ($(SRCS),)
all:
	@echo "src/ 에 아직 C 소스가 없습니다. 환경 검증은 'make hello' 로 하세요."
else
all: $(TARGET)
endif

# 링크: 오브젝트 파일들 → 실행 파일
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# 컴파일: src/xxx.c → build/<모드>/xxx.o  ($@ = 목표, $< = 첫 번째 재료)
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# 환경 검증용 서버 (본 프로젝트와 별개)
hello: $(BUILD)/hello_server

$(BUILD)/hello_server: sandbox/hello_server.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# 랑데부 해싱 단위 테스트: 테스트 코드 + router.o (네트워크 코드 없이 해시 함수만 검사)
rtest: $(BUILD)/rendezvous_test

$(BUILD)/rendezvous_test: $(BUILD)/tests/rendezvous_test.o $(BUILD)/router.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# 캐시 단위 테스트: 테스트 코드 + cache.o (네트워크 없이 캐시만 검사, 벤치마크의 Zipf 계산에 -lm)
ctest: $(BUILD)/cache_test

$(BUILD)/cache_test: $(BUILD)/tests/cache_test.o $(BUILD)/cache.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lm

$(BUILD)/tests/%.o: tests/%.c | $(BUILD)/tests
	$(CC) $(CFLAGS) -c -o $@ $<

# 결과물 폴더가 없으면 만든다 (| 뒤는 "순서만 보장하는" 재료)
$(BUILD):
	mkdir -p $@

$(BUILD)/tests:
	mkdir -p $@

clean:
	rm -rf build

-include $(DEPS)
