# shellcheck disable=SC2034  # NR_* and state globals are read by other lib files
# core.sh - paths, persistent state and small helpers.
# Sourced by bin/nullroute. Plain POSIX sh only.

NR_VERSION="0.1.0"
NR_RIG="rig"                       # the player's own machine
NR_PLAYER="ghost"

# Set by nr_paths after option parsing. The default follows the XDG base
# directory spec, falling back to ~/.local/state.
NR_STATE=""
NR_DATA=""

nr_paths() {
	NR_STATE="${1:-${NULLROUTE_HOME:-${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/nullroute}}"
	NR_DATA="$NR_STATE/world"
}

die() {
	printf 'nullroute: %s\n' "$*" >&2
	exit 1
}

say() {
	printf '%s\n' "$*"
}

# Hosts live in world/hosts/<name>/ with a meta file, a users file and an fs/ tree.
host_dir() {
	printf '%s/hosts/%s' "$NR_DATA" "$1"
}

fs_of() {
	printf '%s/fs' "$(host_dir "$1")"
}

host_exists() {
	[ -f "$(host_dir "$1")/meta" ]
}

# meta_get HOST KEY prints the first value for KEY.
meta_get() {
	sed -n "/^$2=/ { s/^$2=//p; q; }" "$(host_dir "$1")/meta"
}

# meta_all HOST KEY prints every value for KEY, one per line.
meta_all() {
	sed -n "s/^$2=//p" "$(host_dir "$1")/meta"
}

# Hash a password. The game uses POSIX cksum: it is not secure, but it is
# available everywhere and keeps the world files readable.
pw_hash() {
	printf '%s' "$1" | cksum | awk '{ print $1 }'
}

# state_get KEY / state_set KEY VALUE. The state file is plain key=value.
state_get() {
	[ -f "$NR_STATE/state" ] || return 1
	sed -n "/^$1=/ { s/^$1=//p; q; }" "$NR_STATE/state"
}

state_set() {
	_sf="$NR_STATE/state"
	_st="$NR_STATE/state.tmp.$$"
	touch "$_sf"
	grep -v "^$1=" "$_sf" > "$_st" || true
	printf '%s=%s\n' "$1" "$2" >> "$_st"
	mv "$_st" "$_sf"
}

# Space-separated word lists stored in state keys (discovered, owned).
list_has() {
	case " $1 " in
		*" $2 "*) return 0 ;;
	esac
	return 1
}

state_has() {
	list_has "$(state_get "$1")" "$2"
}

state_add() {
	_cur=$(state_get "$1")
	if list_has "$_cur" "$2"; then
		return 0
	fi
	state_set "$1" "${_cur:+$_cur }$2"
}

mission_now() {
	_m=$(state_get mission)
	printf '%s' "${_m:-1}"
}

# Credentials recovered by cracking, stored as "host user password" lines.
save_cred() {
	_cf="$NR_STATE/creds"
	_ct="$NR_STATE/creds.tmp.$$"
	if [ -f "$_cf" ]; then
		grep -v "^$1 $2 " "$_cf" > "$_ct" || true
	else
		: > "$_ct"
	fi
	printf '%s %s %s\n' "$1" "$2" "$3" >> "$_ct"
	mv "$_ct" "$_cf"
}

saved_cred() {
	[ -f "$NR_STATE/creds" ] || return 0
	awk -v h="$1" -v u="$2" '$1 == h && $2 == u { p = $3 } END { if (p != "") print p }' "$NR_STATE/creds"
}

# story_show NAME prints world/story/NAME.txt, if it exists.
story_show() {
	_sf="$NR_DATA/story/$1.txt"
	if [ -f "$_sf" ]; then
		say ""
		cat "$_sf"
		say ""
	fi
}
