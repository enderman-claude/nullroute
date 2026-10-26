# shellcheck disable=SC2034  # NR_QUIT is read by missions.sh and shell.sh
# shell.sh - the read-eval loop, command dispatch and help.
#
# Input is split on the default IFS (spaces and tabs). Quoting is not
# supported: each word is one argument. This keeps the parser simple and
# POSIX-only.

NR_QUIT=0

session_reset() {
	session_open "$NR_RIG" "$NR_PLAYER"
}

prompt() {
	_sym='$'
	[ "$SESS_USER" = root ] && _sym='#'
	if [ "$SESS_HOST" = "$NR_RIG" ]; then
		printf '%s@%s:%s%s ' "$SESS_USER" "$SESS_HOST" "$SESS_CWD" "$_sym"
	else
		printf '[trace %s%%] %s@%s:%s%s ' "$SESS_TRACE" "$SESS_USER" "$SESS_HOST" "$SESS_CWD" "$_sym"
	fi
}

cmd_help() {
	cat <<'HELP'
NULLROUTE commands

  Network
    scan                    list hosts linked from the current machine
    connect HOST USER [PW] log in to a discovered host (PW optional once cracked)
    disconnect              leave the remote host and return to your rig
    portscan HOST           list services on a discovered host
    crack HOST USER         dictionary attack against an SSH account
    exploit HOST SERVICE    use the vulnerability in SERVICE to get a root shell

  Files (on the current host)
    ls [PATH]  cd [PATH]  pwd       cat FILE...      head FILE  tail FILE
    grep [-ivn] PAT FILE  find [PATH] [-name GLOB]  wc FILE
    cp SRC DST   mv SRC DST   rm [-r] PATH...   mkdir DIR...   touch FILE...
    download PATH           copy a file from the remote host to ~/loot on your rig

  Session
    whoami  id  hostname  uname  echo TEXT
    mail [N]                read your mail
    objectives              show the current chapter brief
    status                  show progress and session info
    help                    show this text
    exit                    leave the remote host, or quit on your rig

Everything you do is timed. Remote activity raises your trace. If it reaches
100% you are thrown off the host. Plan your moves.
HELP
}

# run_line LINE: parse one command and run it.
run_line() {
	set -f
	# shellcheck disable=SC2086
	set -- $1
	set +f
	[ $# -eq 0 ] && return 0
	_cmd=$1
	shift

	case $_cmd in
		help)                      cmd_help ;;
		scan)                      cmd_scan "$@" ;;
		connect)                   cmd_connect "$@" ;;
		disconnect)                cmd_disconnect "$@" ;;
		portscan)                  cmd_portscan "$@" ;;
		crack)                     cmd_crack "$@" ;;
		exploit)                   cmd_exploit "$@" ;;
		download)                  cmd_download "$@" ;;
		ls)                        cmd_ls "$@" ;;
		cd)                        cmd_cd "$@" ;;
		pwd)                       cmd_pwd ;;
		cat)                       cmd_cat "$@" ;;
		head)                      cmd_head "$@" ;;
		tail)                      cmd_tail "$@" ;;
		wc)                        cmd_wc "$@" ;;
		grep)                      cmd_grep "$@" ;;
		find)                      cmd_find "$@" ;;
		cp)                        cmd_cp "$@" ;;
		mv)                        cmd_mv "$@" ;;
		rm)                        cmd_rm "$@" ;;
		mkdir)                     cmd_mkdir "$@" ;;
		touch)                     cmd_touch "$@" ;;
		echo)                      cmd_echo "$@" ;;
		whoami)                    cmd_whoami ;;
		id)                        cmd_id ;;
		hostname)                  cmd_hostname ;;
		uname)                     cmd_uname ;;
		mail)                      cmd_mail "$@" ;;
		objectives)                cmd_objectives ;;
		status)                    cmd_status ;;
		publish)                   ending publish ;;
		sell)                      ending sell ;;
		exit|logout|quit)
			if [ "$SESS_HOST" = "$NR_RIG" ]; then
				say "Closing the rig. Progress is saved."
				NR_QUIT=1
			else
				cmd_disconnect
			fi
			return 0 ;;
		*)
			say "$_cmd: command not found (type 'help')" ;;
	esac

	if [ "$NR_QUIT" = 0 ]; then
		trace_tick
		mission_check
	fi
}

# repl reads commands until EOF or exit. Prompts are shown only on a terminal,
# so scripted sessions (piped stdin, used by the tests) print clean output.
repl() {
	_tty=0
	[ -t 0 ] && _tty=1
	if [ "$_tty" = 1 ]; then
		say "NULLROUTE $NR_VERSION. Type 'help' for commands, 'objectives' for your brief."
	fi
	while [ "$NR_QUIT" = 0 ]; do
		if [ "$_tty" = 1 ]; then
			prompt
		fi
		IFS= read -r _line || { say ""; break; }
		run_line "$_line"
	done
}
