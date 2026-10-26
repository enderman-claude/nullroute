#!/bin/sh
# Test runner for NULLROUTE. Pure POSIX sh.
#
# Each test pipes a list of commands into the game and checks the output.
# Run with a different shell to check POSIX compatibility:
#   NR_TEST_SHELL="bash --posix" sh tests/run.sh
#   NR_TEST_SHELL=busybox\ sh   sh tests/run.sh      (busybox needs the 'sh' applet)

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME="$ROOT/game/bin/nullroute"
SHELL_UNDER_TEST=${NR_TEST_SHELL:-sh}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/nullroute-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

PASS=0
FAIL=0
N=0

# play STATE_NAME < commands : run the game with the given save dir.
play() {
	# shellcheck disable=SC2086
	$SHELL_UNDER_TEST "$GAME" -s "$TMP/$1" 2>&1
}

# check NAME HAYSTACK NEEDLE: passes if HAYSTACK contains NEEDLE.
check() {
	N=$((N + 1))
	case $2 in
		*"$3"*)
			PASS=$((PASS + 1))
			printf 'ok %d - %s\n' "$N" "$1" ;;
		*)
			FAIL=$((FAIL + 1))
			printf 'not ok %d - %s\n' "$N" "$1"
			printf '    expected to find: %s\n' "$3"
			printf '%s\n' "$2" | sed 's/^/    | /' | tail -20 ;;
	esac
}

# refute NAME HAYSTACK NEEDLE: passes if HAYSTACK does NOT contain NEEDLE.
refute() {
	N=$((N + 1))
	case $2 in
		*"$3"*)
			FAIL=$((FAIL + 1))
			printf 'not ok %d - %s\n' "$N" "$1"
			printf '    did not expect: %s\n' "$3" ;;
		*)
			PASS=$((PASS + 1))
			printf 'ok %d - %s\n' "$N" "$1" ;;
	esac
}

# --- 1. help and basic dispatch ---------------------------------------------
out=$(printf 'help\nexit\n' | play t_help)
check "help lists the network section" "$out" "Network"
check "help lists download" "$out" "download PATH"

out=$(printf 'bogus\nexit\n' | play t_unknown)
check "unknown commands are reported" "$out" "bogus: command not found"

# --- 2. a new game starts with the rig and chapter 1 -------------------------
out=$(printf 'objectives\nexit\n' | play t_fresh)
check "intro story is shown on a new game" "$out" "Three nights ago a job landed"
check "chapter 1 brief is shown" "$out" "CHAPTER 1: COLD START"
check "saved world copy exists" "$(ls "$TMP/t_fresh/world/hosts" 2>/dev/null)" "crumbco-pos"

# --- 3. hosts must be discovered before they can be used ---------------------
out=$(printf 'connect crumbco-pos clerk dough\nexit\n' | play t_discover)
check "connect refuses an undiscovered host" "$out" "not in your scan results"
out=$(printf 'scan\ncrack crumbco-pos clerk\nexit\n' | play t_discover)
check "scan reveals linked hosts" "$out" "crumbco-pos"
check "crack finds the clerk password" "$out" "Password found after"
check "cracked password is the expected word" "$out" "dough"

# --- 4. the sandbox cannot be escaped ---------------------------------------
out=$(printf 'scan\ncrack crumbco-pos clerk\nconnect crumbco-pos clerk\ncat /../../../../etc/passwd\ncat /etc/passwd\ncd /\nexit\n' | play t_sandbox)
check "path traversal stays inside the host" "$out" "cat: /../../../../etc/passwd: No such file"
refute "real /etc/passwd contents never appear" "$out" "root:x:0:0"
check "absolute paths resolve inside the host" "$out" "cat: /etc/passwd: No such file"

# --- 5. permissions ---------------------------------------------------------
out=$(printf 'scan\ncrack crumbco-pos clerk\nconnect crumbco-pos clerk\ncat /root/anything\nexit\n' | play t_perm)
check "/root is denied to a normal account" "$out" "Permission denied: /root/anything"

# --- 6. flags are whitelisted (grep -f must not read real files) ------------
out=$(printf 'scan\ncrack crumbco-pos clerk\nconnect crumbco-pos clerk\ngrep -f /etc/passwd clerk notes.txt\nexit\n' | play t_grep)
check "grep rejects unknown flags" "$out" "usage: grep [-ivn] PATTERN FILE"
refute "grep -f does not leak real files" "$out" "root:x:0:0"

# --- 7. the rig's home structure is protected -------------------------------
out=$(printf 'rm -r /home\nrm -r /home/ghost/loot\nls /home\nexit\n' | play t_rig)
check "rm -r on /home is refused on the rig" "$out" "Permission denied: /home"
check "the rig still lists its home folder" "$out" "ghost"

# --- 8. wrong passwords are rejected and cost trace -------------------------
out=$(printf 'scan\nconnect crumbco-pos clerk wrong\nexit\n' | play t_wrongpw)
check "wrong password is rejected" "$out" "Access denied."

# --- 9. full playthrough, chapters 1 to 5, ends in publish ------------------
cat > "$TMP/walkthrough.txt" <<'EOF'
scan
crack crumbco-pos clerk
connect crumbco-pos clerk
download /home/clerk/cashdrawer.log
scan
crack meridian-gw admin
connect meridian-gw admin
download /srv/routes.cfg
scan
exploit fleet-ctl ssh
download /root/fleet_override.key
scan
crack audit-vault auditor
connect audit-vault auditor
download /var/audit/audit_trail.log
scan
crack vell-drop vell
connect vell-drop vell
download /home/vell/vell_manifest.txt
publish
EOF
out=$(play t_walk < "$TMP/walkthrough.txt")
check "chapter 1 completes" "$out" "CHAPTER 1 COMPLETE"
check "chapter 2 completes" "$out" "CHAPTER 2 COMPLETE"
check "chapter 3 completes" "$out" "CHAPTER 3 COMPLETE"
check "chapter 4 completes" "$out" "CHAPTER 4 COMPLETE"
check "chapter 5 completes" "$out" "CHAPTER 5 COMPLETE"
check "publish ending is reached" "$out" "THE END. Thanks for playing NULLROUTE."

# A finished game refuses to resume.
out=$(printf 'status\n' | play t_walk)
check "a finished game says so on relaunch" "$out" "Your last game ended (publish)"

# --- 10. saves persist across launches -------------------------------------
out=$(printf 'scan\ncrack crumbco-pos clerk\nconnect crumbco-pos clerk\ndownload /home/clerk/cashdrawer.log\nexit\n' | play t_persist)
check "chapter 1 completes in the first session" "$out" "CHAPTER 1 COMPLETE"
out=$(printf 'status\nexit\n' | play t_persist)
check "second launch resumes at chapter 2" "$out" "Chapter:   2 - Dead Drop"
check "discovered hosts persist" "$out" "crumbco-pos"
out=$(printf 'mail\nexit\n' | play t_persist)
check "mail for chapter 2 was delivered" "$out" "The gateway"

# --- 11. trace -------------------------------------------------------------
{
	printf 'scan\ncrack crumbco-pos clerk\nconnect crumbco-pos clerk\ndownload /home/clerk/cashdrawer.log\n'
	printf 'scan\ncrack meridian-gw admin\nconnect meridian-gw admin\n'
	i=0
	while [ $i -lt 40 ]; do
		printf 'ls\n'
		i=$((i + 1))
	done
	printf 'whoami\nexit\n'
} > "$TMP/trace.txt"
out=$(play t_trace < "$TMP/trace.txt")
check "trace drops the player back to the rig" "$out" "TRACE COMPLETE"
check "after a trace drop the player is on the rig" "$out" "ghost"

# --- 12. --new resets progress ---------------------------------------------
out=$($SHELL_UNDER_TEST "$GAME" -n -s "$TMP/t_persist" < /dev/null 2>&1)
check "-n starts a new game" "$out" "Three nights ago a job landed"

# --- summary ----------------------------------------------------------------
printf '\n%d passed, %d failed (shell: %s)\n' "$PASS" "$FAIL" "$SHELL_UNDER_TEST"
[ "$FAIL" -eq 0 ]
