typeset -gU serverlist
typeset -xT SERVERLIST serverlist
typeset -x CURRENTSERVER

serverlist=( ${=${${${${(@M)${(f)"$(cat ~/.ssh/config 2> /dev/null)"}:#Host *}#Host }:#*\**}:#*\?*}} )

function set_current_server {
  CURRENTSERVER=${serverlist[(r)${${PWD#$HOME/}%%/*}]}
}

set_current_server  # set it immediately
chpwd_functions+=(set_current_server) # and everytime chpwd

local dir="${0:h}"
alias server="${dir}/server.zsh"
alias ras="noglob ${dir}/ras.zsh"

# setup some aliases

alias rfind='ras -e find'
alias rgrep='ras grep'
alias rvim='ras -t vim'
alias rless='ras -t less'
alias rmv='ras mv -iv'
alias rcp='ras cp -ivdRp'
alias rscp='ras rsync -azPe "ssh -q"'
