# shellcheck disable=SC2034  # session globals are shared across lib files
# vfs.sh - filesystem commands that operate inside a host's sandboxed fs/.
#
# Session globals (set by net.sh):
#   SESS_HOST   host the player is currently on
#   SESS_USER   account the player is logged in as (root for exploits)
#   SESS_CWD    virtual working directory on SESS_HOST, always absolute
#   SESS_SEC    security rating of SESS_HOST (adds to trace)
#   SESS_TRACE  trace percentage for the current connection
#
# Path results: vpath_resolve sets VPATH (normalised, absolute, never escapes /)
# and vfs_target sets REAL (the real location inside the sandbox).

# vpath_resolve PATH: normalise PATH relative to SESS_CWD into VPATH.
vpath_resolve() {
	case $1 in
		/*) _p=$1 ;;
		*) _p=$SESS_CWD/$1 ;;
	esac
	_out=
	_oldifs=$IFS
	set -f
	IFS=/
	for _seg in $_p; do
		case $_seg in
			''|.) : ;;
			..) _out=${_out%/*} ;;
			*) _out=$_out/$_seg ;;
		esac
	done
	IFS=$_oldifs
	set +f
	VPATH=${_out:-/}
}

# vfs_access VPATH read|write: enforce the sandbox permissions.
vfs_access() {
	_ok=1
	case $1 in
		/root|/root/*|/secure|/secure/*)
			[ "$SESS_USER" = root ] || _ok=0 ;;
	esac
	if [ "$2" = write ]; then
		case $1 in
			/etc|/etc/*|/bin|/bin/*)
				[ "$SESS_USER" = root ] || _ok=0 ;;
		esac
		# The rig's home structure is protected so that a stray rm -r cannot
		# delete the mail and loot folders the game depends on.
		if [ "$SESS_HOST" = "$NR_RIG" ]; then
			case $1 in
				/home|"/home/$NR_PLAYER"|"/home/$NR_PLAYER/loot"|"/home/$NR_PLAYER/mail")
					_ok=0 ;;
			esac
		fi
	fi
	if [ "$_ok" = 1 ]; then
		return 0
	fi
	say "Permission denied: $1"
	return 1
}

# vfs_target ARG read|write: resolve ARG, check access, set VPATH and REAL.
vfs_target() {
	vpath_resolve "$1"
	REAL="$(fs_of "$SESS_HOST")$VPATH"
	vfs_access "$VPATH" "$2"
}

cmd_pwd() {
	say "$SESS_CWD"
}

cmd_cd() {
	vfs_target "${1:-/home/$SESS_USER}" read || return 1
	if [ ! -d "$REAL" ]; then
		say "cd: no such directory: ${1:-~}"
		return 1
	fi
	SESS_CWD=$VPATH
}

cmd_ls() {
	vfs_target "${1:-$SESS_CWD}" read || return 1
	if [ -d "$REAL" ]; then
		ls -1 "$REAL"
	elif [ -e "$REAL" ]; then
		say "$VPATH"
	else
		say "ls: cannot access '${1:-.}': No such file or directory"
		return 1
	fi
}

cmd_cat() {
	[ $# -gt 0 ] || { say "usage: cat FILE..."; return 1; }
	_rc=0
	for _a; do
		if ! vfs_target "$_a" read; then
			_rc=1
			continue
		fi
		if [ -f "$REAL" ]; then
			cat "$REAL" || _rc=1
		else
			say "cat: $_a: No such file"
			_rc=1
		fi
	done
	return $_rc
}

cmd_head() {
	[ $# -eq 1 ] || { say "usage: head FILE"; return 1; }
	vfs_target "$1" read || return 1
	[ -f "$REAL" ] || { say "head: $1: No such file"; return 1; }
	head -n 10 "$REAL"
}

cmd_tail() {
	[ $# -eq 1 ] || { say "usage: tail FILE"; return 1; }
	vfs_target "$1" read || return 1
	[ -f "$REAL" ] || { say "tail: $1: No such file"; return 1; }
	tail -n 10 "$REAL"
}

cmd_wc() {
	[ $# -eq 1 ] || { say "usage: wc FILE"; return 1; }
	vfs_target "$1" read || return 1
	[ -f "$REAL" ] || { say "wc: $1: No such file"; return 1; }
	wc < "$REAL"
}

# grep takes only the -i, -v and -n flags. Passing other flags would let
# grep read files from the real machine (for example -f).
cmd_grep() {
	_flags=
	case $1 in
		-i|-v|-n) _flags=$1; shift ;;
	esac
	[ $# -eq 2 ] || { say "usage: grep [-ivn] PATTERN FILE"; return 1; }
	vfs_target "$2" read || return 1
	[ -f "$REAL" ] || { say "grep: $2: No such file"; return 1; }
	if [ -n "$_flags" ]; then
		grep "$_flags" -- "$1" "$REAL"
	else
		grep -- "$1" "$REAL"
	fi
}

# find [PATH] [-name GLOB]: prints virtual paths only.
cmd_find() {
	_start=$SESS_CWD
	_name=
	if [ $# -gt 0 ] && [ "$1" != -name ]; then
		_start=$1
		shift
	fi
	if [ "${1:-}" = -name ]; then
		_name=${2:-}
	fi
	vfs_target "$_start" read || return 1
	[ -d "$REAL" ] || { say "find: $_start: No such directory"; return 1; }
	_root=$(fs_of "$SESS_HOST")
	if [ -n "$_name" ]; then
		_list=$(find "$REAL" -name "$_name")
	else
		_list=$(find "$REAL")
	fi
	printf '%s\n' "$_list" | while IFS= read -r _l; do
		_v=${_l#"$_root"}
		[ -n "$_v" ] && printf '%s\n' "$_v"
	done
	return 0
}

cmd_cp() {
	[ $# -eq 2 ] || { say "usage: cp SOURCE DEST"; return 1; }
	vfs_target "$1" read || return 1
	[ -f "$REAL" ] || { say "cp: $1: not a file"; return 1; }
	_src=$REAL
	vfs_target "$2" write || return 1
	if [ -d "$REAL" ]; then
		vfs_target "$VPATH/${1##*/}" write || return 1
	fi
	cp "$_src" "$REAL"
}

cmd_mv() {
	[ $# -eq 2 ] || { say "usage: mv SOURCE DEST"; return 1; }
	vfs_target "$1" write || return 1
	[ -e "$REAL" ] || { say "mv: $1: No such file"; return 1; }
	_src=$REAL
	vfs_target "$2" write || return 1
	if [ -d "$REAL" ]; then
		vfs_target "$VPATH/${1##*/}" write || return 1
	fi
	mv "$_src" "$REAL"
}

cmd_rm() {
	_rec=0
	case ${1:-} in
		-r|-rf) _rec=1; shift ;;
	esac
	[ $# -gt 0 ] || { say "usage: rm [-r] PATH..."; return 1; }
	_rc=0
	for _a; do
		if ! vfs_target "$_a" write; then
			_rc=1
			continue
		fi
		if [ "$VPATH" = / ]; then
			say "rm: refusing to remove /"
			_rc=1
		elif [ -d "$REAL" ]; then
			if [ "$_rec" = 1 ]; then
				rm -r "$REAL" || _rc=1
			else
				say "rm: $_a: is a directory (use rm -r)"
				_rc=1
			fi
		elif [ -e "$REAL" ]; then
			rm "$REAL" || _rc=1
		else
			say "rm: $_a: No such file"
			_rc=1
		fi
	done
	return $_rc
}

cmd_mkdir() {
	[ $# -gt 0 ] || { say "usage: mkdir DIR..."; return 1; }
	for _a; do
		vfs_target "$_a" write || return 1
		mkdir "$REAL" || return 1
	done
}

cmd_touch() {
	[ $# -gt 0 ] || { say "usage: touch FILE..."; return 1; }
	for _a; do
		vfs_target "$_a" write || return 1
		if [ ! -e "$REAL" ]; then
			: > "$REAL" || return 1
		fi
	done
}

cmd_echo() {
	say "$*"
}

cmd_whoami() {
	say "$SESS_USER"
}

cmd_hostname() {
	say "$SESS_HOST"
}

cmd_uname() {
	say "NullOS 4.1 nullroute-posix $SESS_HOST x86_64"
}

cmd_id() {
	if [ "$SESS_USER" = root ]; then
		say "uid=0(root) gid=0(root) groups=0(root)"
	else
		say "uid=1000($SESS_USER) gid=1000($SESS_USER) groups=1000($SESS_USER)"
	fi
}
