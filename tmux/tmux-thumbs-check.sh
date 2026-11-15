#!/usr/bin/env bash
CURRENT_DIR="$HOME/.dotfiles/tmux/tmux-thumbs"
RELEASE_DIR="${CURRENT_DIR}/target/release"
THUMBS_BINARY="${RELEASE_DIR}/thumbs"
VERSION=$(grep 'version =' "${CURRENT_DIR}/Cargo.toml" | grep -o "\".*\"" | sed 's/"//g')

USER_NAME=${USER:-$(id -un)}
tmux show -g | grep '@thumbs-' > "/tmp/thumbs-options-${USER_NAME}.txt" || true

prompt_update() {
  local mode="$1"
  local cmd="cd ${CURRENT_DIR} && bash ./tmux-thumbs-install.sh ${mode}"
  if tmux list-clients &>/dev/null; then
    tmux split-window "${cmd}"
  else
    tmux set-hook -g 'client-session-changed[99]' "split-window '${cmd}'; set-hook -ug 'client-session-changed[99]'"
  fi
  exit
}

if [ ! -f "$THUMBS_BINARY" ]; then
  prompt_update "install"
elif [[ $(${THUMBS_BINARY} --version) != "thumbs ${VERSION}" ]]; then
  prompt_update "update"
fi
