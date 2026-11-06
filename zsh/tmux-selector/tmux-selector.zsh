[[ -n $TMUX ]] && return

function _tmux_selector() {
  local -A custom_actions
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --extra)
        local item="$2"
        local name="${item%%|*}"
        local action="${item#*|}"
        custom_actions[$name]="$action"
        shift 2
        ;;
      *)
        shift
        ;;
    esac
  done

  zmodload -F zsh/datetime b:strftime 2>/dev/null

  local _session_format="#{session_name},#{session_attached},#{session_last_attached},#{session_created}"

  local tmux_cmd="tmux"
  local raw_output
  raw_output=$(tmux list-sessions -F "$_session_format" 2>&1)
  local list_status=$?

  local -a tmux_sessions=()
  if (( list_status == 0 )); then
    tmux_sessions=( ${(f)raw_output} )
  elif [[ "$raw_output" == *"protocol version mismatch"* ]] && (( $+commands[tmx2] )); then
    tmux_cmd="tmx2"
  fi

  _exec_tmux() {
    if [[ "$tmux_cmd" == "tmx2" ]] && (( $+commands[tmx2] )); then
      exec tmx2 "$@"
    fi

    local d="${HOME}/.tmp"
    [[ -d "$d" ]] || mkdir -m 700 -p "$d"

    local -a env_args=()
    local s="${d}/.${USER}.ssh_auth_sock"
    if [[ -n "$SSH_AUTH_SOCK" ]]; then
      [[ "$SSH_AUTH_SOCK" -ef "$s" ]] || ln -snf "$SSH_AUTH_SOCK" "$s"
      env_args+=( "SSH_AUTH_SOCK=$s" )
    else
      rm -f -- "$s"
    fi

    local t="${d}/.${USER}.fwd_ssh_auth_sock"
    if [[ -n "$FWD_SSH_AUTH_SOCK" ]]; then
      [[ "$FWD_SSH_AUTH_SOCK" -ef "$t" ]] || ln -snf "$FWD_SSH_AUTH_SOCK" "$t"
      env_args+=( "FWD_SSH_AUTH_SOCK=$t" )
    else
      rm -f -- "$t"
    fi

    exec env "${env_args[@]}" tmux "$@"
  }

  [[ ${#tmux_sessions} -eq 0 && ${#custom_actions} -eq 0 ]] && _exec_tmux

  local format_tmux_session() {
    local name is_attached attached created
    read -r name is_attached attached created <<<${1//,/ }

    (( $is_attached )) && is_attached="+" || is_attached="-"
    strftime -s created "%A %H:%M" $created 2>/dev/null || created=$(date +"%A %H:%M" -d @$created 2>/dev/null)
    strftime -s attached "%A %H:%M" $attached 2>/dev/null || attached=$(date +"%A %H:%M" -d @$attached 2>/dev/null)

    printf "%s %-10s : Created %s | Last Attached %s\n" \
      $is_attached $name $created $attached
  }

  local run_selector() {
    (
      echo "> Create new session"
      for session in "${tmux_sessions[@]}"; do
        format_tmux_session "$session"
      done
      for name in ${(k)custom_actions}; do
        echo "> $name"
      done
    ) | fzf
  }

  local choice
  choice=$(run_selector)
  if [[ -z $choice ]]; then
    unfunction _exec_tmux format_tmux_session run_selector 2>/dev/null
    return
  fi

  local type=$(echo $choice | cut -d' ' -f 1)

  if [[ $type == ">" ]]; then
    local name="${choice#* }"
    if [[ $name == "Create new session" ]]; then
      _exec_tmux new
    elif [[ -n $custom_actions[$name] ]]; then
      eval $custom_actions[$name]
    fi
  elif [[ $type == "+" || $type == "-" ]]; then
    local val=$(echo $choice | cut -d' ' -f 2)
    _exec_tmux attach -t "$val"
  fi
}

_tmux_selector "$@"
unfunction _tmux_selector 2>/dev/null
