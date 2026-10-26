# shellcheck disable=SC2034  # NR_QUIT is read by shell.sh
# missions.sh - mission progression, the mail client, and the endings.
#
# world/missions.txt has one line per chapter:  id|title|type|argument
# Only "loot" missions exist: the chapter is done when the named file has
# been downloaded to ~/loot on the rig.
#
# Story text lives in world/story/:
#   intro.txt          shown on a new game
#   brief-N.txt        shown when chapter N starts
#   complete-N.txt     shown when chapter N is finished
#   mail-N.txt         delivered to ~/mail when chapter N starts

mission_field() {
	grep "^$1|" "$NR_DATA/missions.txt" | awk -F'|' -v f="$2" '{ print $f }'
}

rig_loot_dir() {
	printf '%s/home/%s/loot' "$(fs_of "$NR_RIG")" "$NR_PLAYER"
}

rig_mail_dir() {
	printf '%s/home/%s/mail' "$(fs_of "$NR_RIG")" "$NR_PLAYER"
}

# deliver_mail N copies story/mail-N.txt into the rig's mail folder.
deliver_mail() {
	_src="$NR_DATA/story/mail-$1.txt"
	[ -f "$_src" ] || return 0
	_dir=$(rig_mail_dir)
	mkdir -p "$_dir"
	cp "$_src" "$_dir/$(printf '%03d' "$1").txt"
}

# mission_check runs after every command and completes a chapter when its
# goal is met. The next chapter is unlocked and its mail delivered.
mission_check() {
	[ "$(state_get won)" = 1 ] && return 0
	_m=$(mission_now)
	_type=$(mission_field "$_m" 3)
	_arg=$(mission_field "$_m" 4)
	[ -n "$_type" ] || return 0
	case $_type in
		loot)
			[ -f "$(rig_loot_dir)/$_arg" ] || return 0 ;;
		*)
			return 0 ;;
	esac
	complete_mission "$_m"
}

complete_mission() {
	_m=$1
	say ""
	say "=== CHAPTER $_m COMPLETE: $(mission_field "$_m" 2) ==="
	story_show "complete-$_m"
	_next=$((_m + 1))
	if [ -z "$(mission_field "$_next" 1)" ]; then
		state_set won 1
		say "Your evidence is ready. Decide what to do with it:"
		say "  publish   send it to the press"
		say "  sell      take Vell's offer"
		return 0
	fi
	state_set mission "$_next"
	deliver_mail "$_next"
	say "A new chapter has started. You have new mail (type 'mail')."
	story_show "brief-$_next"
}

cmd_objectives() {
	_m=$(mission_now)
	if [ "$(state_get won)" = 1 ]; then
		say "All chapters complete. Choose 'publish' or 'sell'."
		return 0
	fi
	_title=$(mission_field "$_m" 2)
	_arg=$(mission_field "$_m" 4)
	say "Chapter $_m: $_title"
	say "Goal: download '$_arg' to your rig (~/loot)."
	story_show "brief-$_m"
}

# mail with no argument lists messages. mail N prints one message.
cmd_mail() {
	_dir=$(rig_mail_dir)
	if [ $# -eq 0 ]; then
		_any=0
		for _f in "$_dir"/*.txt; do
			[ -f "$_f" ] || continue
			_any=1
			_id=$(basename "$_f" .txt)
			_subj=$(sed -n '/^Subject: /{ s/^Subject: //p; q; }' "$_f")
			printf '  %s  %s\n' "$_id" "$_subj"
		done
		[ "$_any" = 1 ] || say "No mail."
		return 0
	fi
	case $1 in
		*[!0-9]*|'')
			say "usage: mail [N]"
			return 1 ;;
	esac
	_f="$_dir/$(printf '%03d' "$1").txt"
	if [ ! -f "$_f" ]; then
		say "mail: no message $1"
		return 1
	fi
	cat "$_f"
}

cmd_status() {
	_m=$(mission_now)
	say "Chapter:   $_m - $(mission_field "$_m" 2)"
	say "Discovered: $(state_get discovered || true)"
	say "Rooted:     $(state_get owned || true)"
	if [ -f "$NR_STATE/creds" ]; then
		say "Passwords:  $(wc -l < "$NR_STATE/creds" | tr -d ' ') saved"
	fi
	if [ "$SESS_HOST" != "$NR_RIG" ]; then
		say "Session:    $SESS_USER@$SESS_HOST (trace $SESS_TRACE%)"
	fi
}

# ending publish|sell: the final choice. Ends the game.
ending() {
	if [ "$(state_get won)" != 1 ]; then
		say "Nothing to $1 yet. Keep digging."
		return 1
	fi
	story_show "ending-$1"
	say "THE END. Thanks for playing NULLROUTE."
	state_set ended "$1"
	NR_QUIT=1
}

# game_init sets up a fresh save on first run.
game_init() {
	mkdir -p "$NR_STATE" || die "cannot create $NR_STATE"
	if [ ! -d "$NR_DATA" ]; then
		cp -R "$NR_WORLD" "$NR_DATA" || die "cannot copy world data to $NR_DATA"
		state_set mission 1
		story_show intro
		deliver_mail 1
		story_show brief-1
	fi
}
