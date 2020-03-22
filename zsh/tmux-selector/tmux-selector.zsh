[[ -n $TMUX ]] && return

function _tmux_selector() {

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
      echo "Create new session"
      for session in $tmux_sessions
      do
        format_tmux_session $session
      done
     ) | fzf | cut -d' ' -f 2
  }

  local tmux_sessions=($(tmux list-sessions -F $_session_format 2>/dev/null))

  [[ ${#tmux_sessions} -eq 0 ]] && exec tmux

  local choice=$(run_selector)

  if [[ $choice == "new" ]]
  then
    exec tmux new
  elif [[ ! -z $choice ]]
  then
    exec tmux attach -t $choice
  fi

}

_tmux_selector
