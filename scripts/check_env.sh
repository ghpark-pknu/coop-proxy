#!/usr/bin/env bash
# 개발환경 점검 스크립트
set -u

pass=0
fail=0
ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; pass=$((pass + 1)); }
ng()   { printf '  \033[31m✗\033[0m %s\n' "$1"; fail=$((fail + 1)); }
hint() { printf '      → %s\n' "$1"; }

echo "[1] 운영체제"
. /etc/os-release
if [[ "$VERSION_ID" == "24.04" ]]; then ok "$PRETTY_NAME"; else ng "$PRETTY_NAME"; hint "계획서 기준은 Ubuntu 24.04"; fi
if grep -qi microsoft /proc/version; then ok "WSL 커널 $(uname -r)"; else echo "  - WSL이 아닌 리눅스 ($(uname -r))"; fi

echo "[2] 프로젝트 위치"
case "$PWD" in
    /mnt/*) ng "$PWD"; hint "Windows 드라이브(/mnt/...)는 느림 → ~/projects 아래로 옮기기" ;;
    *)      ok "$PWD" ;;
esac

echo "[3] 개발 도구"
for t in gcc make gdb valgrind git curl nc ab tcpdump strace python3; do
    if command -v "$t" >/dev/null 2>&1; then ok "$t"; else ng "$t 없음"; fi
done
command -v gcc     >/dev/null && echo "      gcc     : $(gcc --version | head -1)"
command -v python3 >/dev/null && echo "      python3 : $(python3 --version 2>&1)"

echo "[4] Python 패키지"
if v=$(python3 -c 'import matplotlib; print(matplotlib.__version__)' 2>/dev/null); then
    ok "matplotlib $v"
else
    ng "matplotlib 없음"; hint "sudo apt install python3-matplotlib"
fi

echo "[5] 매뉴얼 페이지 (소켓·pthread 함수)"
if ls /usr/share/man/man2/socket.2* >/dev/null 2>&1 && ls /usr/share/man/man3/pthread_create.3* >/dev/null 2>&1; then
    ok "man 2 socket, man 3 pthread_create"
else
    ng "개발용 매뉴얼 없음"; hint "sudo apt install manpages-dev manpages-posix-dev"
fi
if grep -q "minimized" "$(command -v man)" 2>/dev/null; then
    ng "man 명령이 최소화 상태"; hint "sudo unminimize"
fi

echo "[6] git 설정"
if n=$(git config --global user.name);  then ok "user.name  = $n"; else ng "user.name 미설정";  hint 'git config --global user.name "이름"'; fi
if e=$(git config --global user.email); then ok "user.email = $e"; else ng "user.email 미설정"; hint 'git config --global user.email "메일"'; fi

echo "[7] VS Code 연동"
if command -v code >/dev/null 2>&1; then
    ok "code 명령 사용 가능"
else
    ng "code 명령 없음"; hint "Windows에 VS Code + WSL 확장 설치 후, PowerShell에서 wsl --shutdown 하고 다시 열기"
fi

echo "[8] 컴파일러 기능 (pthread · AddressSanitizer · ThreadSanitizer)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
static void *f(void *a) { return a; }
int main(void) { pthread_t t; pthread_create(&t, NULL, f, NULL); pthread_join(t, NULL); puts("ok"); return 0; }
EOF
for san in none address thread; do
    flags="-pthread"
    [[ $san != none ]] && flags="$flags -fsanitize=$san"
    if gcc $flags "$tmp/t.c" -o "$tmp/t" 2>"$tmp/err" && "$tmp/t" >/dev/null 2>>"$tmp/err"; then
        ok "gcc $flags"
    else
        ng "gcc $flags"
        head -3 "$tmp/err" | sed 's/^/      /'
        if [[ $san == thread ]] && grep -q "unexpected memory mapping" "$tmp/err"; then
            hint "sudo sysctl vm.mmap_rnd_bits=28   (또는 setarch \$(uname -m) -R ./프로그램)"
        fi
    fi
done

echo
echo "결과: 통과 $pass개, 실패 $fail개"
[[ $fail -eq 0 ]]
