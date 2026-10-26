# shellcheck disable=SC2034  # session globals (SESS_*) are shared across lib files
# net.sh - network commands: scan, connect, crack, exploit, download.
#
# Hosts are only reachable once discovered (scan from a linked host). A host
# may also be locked behind a mission number (meta key "unlock").
# Every command run on a remote host adds that host's security rating to the
# session trace. Reaching TRACE_LIMIT drops the player back to the rig.

TRACE_LIMIT=100

# session_open HOST USER: switch the session to HOST as USER.
session_open() {
	SESS_HOST=$1
	SESS_USER=$2
	SESS_SEC=$(meta_get "$1" security)
	SESS_SEC=${SESS_SEC:-1}
	SESS_TRACE=0
	if [ -d "$(fs_of "$1")/home/$2" ]; then
		SESS_CWD=/home/$2
	elif [ "$2" = root ] && [ -d "$(fs_of "$1")/root" ]; then
		SESS_CWD=/root
	else
		SESS_CWD=/
	fi
}

# The rig is always known. Other hosts must have been found by scanning.
host_known() {
	[ "$1" = "$NR_RIG" ] || state_has discovered "$1"
}

check_password() {
	_want=$(sed -n "/^$2:/ { s/^$2://p; q; }" "$(host_dir "$1")/users")
	[ -n "$_want" ] && [ "$_want" = "$(pw_hash "$3")" ]
}

stty_quiet() {
	[ -t 0 ] && stty -echo 2>/dev/null
	return 0
}

stty_loud() {
	[ -t 0 ] && stty echo 2>/dev/null
	return 0
}

cmd_scan() {
	_links=$(meta_get "$SESS_HOST" links | tr ',' ' ')
	if [ -z "$_links" ]; then
		say "No outbound links from $SESS_HOST."
		return 0
	fi
	_now=$(mission_now)
	_found=0
	say "Scanning links from $SESS_HOST ..."
	for _h in $_links; do
		host_exists "$_h" || continue
		_unlock=$(meta_get "$_h" unlock)
		[ "${_unlock:-1}" -le "$_now" ] || continue
		state_add discovered "$_h"
		_found=$((_found + 1))
		printf '  %-14s %-16s %s\n' "$_h" "$(meta_get "$_h" ip)" "$(meta_get "$_h" name)"
	done
	if [ "$_found" -eq 0 ]; then
		say "  nothing new."
	fi
}

cmd_connect() {
	[ $# -ge 2 ] || { say "usage: connect HOST USER [PASSWORD]"; return 1; }
	_h=$1
	_u=$2
	_pw=${3:-}
	host_exists "$_h" || { say "connect: unknown host '$_h'"; return 1; }
	host_known "$_h" || { say "connect: $_h is not in your scan results"; return 1; }
	if [ "$_h" = "$NR_RIG" ]; then
		session_open "$NR_RIG" "$NR_PLAYER"
		say "Back on your rig."
		return 0
	fi
	if [ -z "$_pw" ]; then
		_pw=$(saved_cred "$_h" "$_u")
	fi
	if [ -z "$_pw" ]; then
		printf 'Password for %s@%s: ' "$_u" "$_h"
		stty_quiet
		IFS= read -r _pw || _pw=
		stty_loud
		say ""
	fi
	if check_password "$_h" "$_u" "$_pw"; then
		session_open "$_h" "$_u"
		save_cred "$_h" "$_u" "$_pw"
		say "Connected to $_h as $_u."
	else
		say "Access denied."
		SESS_TRACE=$((SESS_TRACE + 10))
		return 1
	fi
}

cmd_disconnect() {
	if [ "$SESS_HOST" = "$NR_RIG" ]; then
		say "Already on your rig."
		return 0
	fi
	session_open "$NR_RIG" "$NR_PLAYER"
	say "Disconnected. Back on your rig."
}

# exploit HOST SERVICE: a working exploit gives a root shell.
cmd_exploit() {
	[ $# -eq 2 ] || { say "usage: exploit HOST SERVICE"; return 1; }
	_h=$1
	_svc=$2
	if ! host_exists "$_h" || ! host_known "$_h"; then
		say "exploit: unknown host '$_h'"
		return 1
	fi
	say "[*] Probing $_svc on $_h ..."
	if ! meta_all "$_h" service | grep -q "^$_svc:[0-9]*:open$"; then
		say "[-] $_svc is closed or filtered on $_h."
		return 1
	fi
	if [ "$(meta_get "$_h" vuln)" != "$_svc" ]; then
		say "[-] No working exploit for $_svc on $_h."
		return 1
	fi
	say "[*] Injecting payload ..."
	session_open "$_h" root
	state_add owned "$_h"
	say "[+] Root shell on $_h. Trace is now at ${SESS_TRACE}%."
}

cmd_portscan() {
	[ $# -eq 1 ] || { say "usage: portscan HOST"; return 1; }
	if ! host_exists "$1" || ! host_known "$1"; then
		say "portscan: unknown host '$1'"
		return 1
	fi
	say "Services on $1:"
	meta_all "$1" service | while IFS=: read -r _sv _pt _st; do
		printf '  %-6s %-8s %s\n' "$_pt" "$_sv" "$_st"
	done
}

# crack HOST USER: offline dictionary attack against an SSH account.
cmd_crack() {
	[ $# -eq 2 ] || { say "usage: crack HOST USER"; return 1; }
	_h=$1
	_u=$2
	if ! host_exists "$_h" || ! host_known "$_h"; then
		say "crack: unknown host '$_h'"
		return 1
	fi
	if ! meta_all "$_h" service | grep -q '^ssh:[0-9]*:open$'; then
		say "crack: ssh is closed on $_h, nothing to attack."
		return 1
	fi
	_want=$(sed -n "/^$_u:/ { s/^$_u://p; q; }" "$(host_dir "$_h")/users")
	if [ -z "$_want" ]; then
		say "crack: no account '$_u' on $_h"
		return 1
	fi
	say "[*] Dictionary attack on $_u@$_h ($(wc -l < "$NR_DATA/wordlist" | tr -d ' ') words)"
	_n=0
	while IFS= read -r _w; do
		_n=$((_n + 1))
		if [ "$(pw_hash "$_w")" = "$_want" ]; then
			say "[+] Password found after $_n attempts: $_w"
			save_cred "$_h" "$_u" "$_w"
			return 0
		fi
	done < "$NR_DATA/wordlist"
	say "[-] Wordlist exhausted. Try something else."
	return 1
}

# download PATH: copy a file from the current remote host into ~/loot.
cmd_download() {
	if [ "$SESS_HOST" = "$NR_RIG" ]; then
		say "download: connect to a host first."
		return 1
	fi
	[ $# -eq 1 ] || { say "usage: download PATH"; return 1; }
	vfs_target "$1" read || return 1
	if [ ! -f "$REAL" ]; then
		say "download: $1: No such file"
		return 1
	fi
	_name=${1##*/}
	_dst="$(fs_of "$NR_RIG")/home/$NR_PLAYER/loot"
	mkdir -p "$_dst"
	cp "$REAL" "$_dst/$_name"
	say "Downloaded $_name ($(wc -c < "$REAL" | tr -d ' ') bytes) to ~/loot/"
}

# trace_tick: called after every command. Remote activity raises the trace.
trace_tick() {
	[ "$SESS_HOST" = "$NR_RIG" ] && return 0
	SESS_TRACE=$((SESS_TRACE + SESS_SEC))
	if [ "$SESS_TRACE" -ge "$TRACE_LIMIT" ]; then
		say ""
		say "!!! TRACE COMPLETE: the owner of $SESS_HOST located your rig."
		say "!!! Connection dropped. Your connection was closed by the remote host."
		session_open "$NR_RIG" "$NR_PLAYER"
	fi
}
