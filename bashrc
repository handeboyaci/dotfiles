# If not running interactively, don't do anything
case $- in
    *i*) ;;
      *) return;;
esac

HISTCONTROL=ignoreboth
HISTSIZE=1000
HISTFILESIZE=2000
shopt -s histappend checkwinsize globstar

eval "$(dircolors -b ~/.dotfiles/zsh/dircolors)"

[ -f /usr/share/bash-completion/bash_completion ] && source /usr/share/bash-completion/bash_completion
[ -f /usr/share/doc/fzf/examples/key-bindings.bash ] && source /usr/share/doc/fzf/examples/key-bindings.bash

source $HOME/.dotfiles/zsh/aliases

export EDITOR=nvim
export VISUAL=nvim
export BROWSER=xdg-open
export LANG=en_US.UTF-8

export TMPDIR=/tmp/$USER
export TMPPREFIX=/tmp/$USER/zsh
mkdir -p -m 700 "$TMPDIR"
mkdir -p -m 700 "$TMPPREFIX"

export PAGER=less
export LESS='-g -i -M -R -W -z-4'
export LESSOPEN="|$HOME/bin/lesspipe %s"
export LESSCLOSE="$HOME/bin/lesspipe %s %s"

function dedup {
    declare -a new=() copy=("${DIRSTACK[@]:1}")
    declare -A seen
    local v i
    seen[$PWD]=1
    for v in "${copy[@]}"
    do if [ -z "${seen[$v]}" ]
       then new+=("$v")
            seen[$v]=1
       fi
    done
    dirs -c
    for ((i=${#new[@]}-1; i>=0; i--))
    do      builtin pushd -n "${new[i]}" >/dev/null
    done
}

function cd {
    if (("$#" > 0)); then
        if [ "$1" == "-" ]; then
            popd > /dev/null
        else
            pushd "$@" > /dev/null
            dedup
        fi
    else
        cd $HOME
    fi
}

complete -d cd

# theme
RESET=$'\[\e[0m\]'
GREEN=$'\[\e[38;2;114;178;51m\e[1m\]'
ORANGE=$'\[\e[38;2;255;100;0m\e[1m\]'
RED=$'\[\e[38;2;255;100;0m\e[1m\]'
PS_LINE=$(printf -- ' %.0s' {1..200})

function __ps1_update {
  EXIT=$?
  (( $EXIT )) || EXIT=""
  CHARS=${#EXIT}

  SUDO=""
  [[ $UID == 0 ]] && SUDO="# "

  [[ -n $SSH_CONNECTION ]] && HOST="@${HOSTNAME%%.*}"

  JOBS=""
  local jobs=$(jobs -srp | wc -l)
  ((jobs)) && JOBS="$jobs bg jobs"
  CHARS=$((CHARS+${#JOBS}))
}

PROMPT_COMMAND='__ps1_update'

PS1="\${PS_LINE:0:$COLUMNS}\[\e[0G\]\[\e[38;2;255;0;0m\]$SUDO$GREEN\u\$HOST$RESET\[\e[1m\] | $GREEN\w \[\e[\$((COLUMNS-CHARS))G\]$RED\$JOBS \$EXIT\n${RESET}${ORANGE}❯${RESET} "

export PATH=$HOME/.dotfiles/bin:$HOME/.cargo/bin:$HOME/.local/share/conda/bin:$PATH
