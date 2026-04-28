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

  local get_session_format () {
    local sep='},#{'
    local session_format=(session_name
                          session_attached
                          session_last_attached
                          session_created)
    echo "#{${(pj.$sep.)session_format}}"
  }

  local _session_format=$(get_session_format)

  local format_tmux_session () {
    read -r name is_attached attached created <<<${1//,/ }

    (( $is_attached )) && is_attached="+" || is_attached="-"
    created=$(date +"%A %H:%M" -d @$created)
    attached=$(date +"%A %H:%M" -d @$attached)

    printf "%s %-10s : Created %s | Last Attached %s\n" \
      $is_attached $name $created $attached
  }

  local run_selector () {
    (
      echo "> Create new session"
      for session in $tmux_sessions
      do
        format_tmux_session $session
      done
      for name in ${(k)custom_actions}; do
        echo "> $name"
      done
     ) | fzf
  }

  local tmux_sessions=($(tmx2 list-sessions -F $_session_format 2>/dev/null))

  [[ ${#tmux_sessions} -eq 0 && ${#custom_actions} -eq 0 ]] && exec tmx2

  local choice=$(run_selector)
  [[ -z $choice ]] && return

  local type=$(echo $choice | cut -d' ' -f 1)

  if [[ $type == ">" ]]
  then
    local name="${choice#* }"
    if [[ $name == "Create new session" ]]
    then
      exec tmx2 new
    elif [[ -n $custom_actions[$name] ]]
    then
      eval $custom_actions[$name]
    fi
  elif [[ $type == "+" || $type == "-" ]]
  then
    local val=$(echo $choice | cut -d' ' -f 2)
    exec tmx2 attach -t $val
  fi

}

_tmux_selector "$@"
