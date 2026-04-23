#!/usr/bin/env zsh
if [[ -z ${ZSH_VERSION} ]]
then
  >&2 echo "These scripts use zsh specific syntax."
  return 1
fi

source ${0:h}/aliases.zsh

fpath=( ${0:h}/autoload $fpath )
autoload -U ras g4d _g4d _ras

typeset -x CITC_NAME CITC_PWD CITC_ROOT CITC_USER PYTHONPATH _PYTHONPATH \
           CL CL_SYNCED VCS

function cl-update-prompt {
  typeset -g CL CL_SYNCED psvar
  psvar=( ${(f)3} )
  CL_SYNCED=$psvar[1]
  CL=$psvar[2]
  zle reset-prompt
  async_stop_worker prompt
}

function cl-precmd {
  typeset -g psvar
  local cl

  if [[ -z $VCS ]]
  then
    return
  elif [[ $VCS == g4 ]]
  then
    cl=$(p4 -F '%change%' changes -s pending \
                     -c "$(p4 -F'%clientName%' info)")
  elif [[ $VCS == hg ]]
  then
    cl=$(hg exportedcl)
  fi
  echo "${$(srcfs get_readonly):-ERROR}\n$cl"
}

function async-cl-precmd {
  async_start_worker prompt
  async_register_callback prompt cl-update-prompt
  async_job prompt cl-precmd
}


source ${0:h}/async.zsh
async_init
typeset -g -a precmd_functions
precmd_functions+=async-cl-precmd

function set_current_client {
  local prefix="/google/src/cloud"
  local splitted
  if [[ "$PWD" =~ "$prefix"/.*/.* ]]
  then
    splitted=(${(s,/,)PWD#$prefix})
    CITC_USER=$splitted[1]
    CITC_NAME=$splitted[2]
    if [[ ${#splitted} -eq 3 ]]
    then
      CITC_PWD="."
    else
      CITC_PWD=${(j,/,)splitted[4,-1]}
    fi

    CITC_ROOT="$prefix/$CITC_USER/$CITC_NAME/google3"
    [[ -f $CITC_ROOT/../.citc/p4_client_name ]] && VCS=g4 || VCS=hg
    unset MATCH
  else
    CITC_NAME=""
    CITC_ROOT=""
    CITC_PWD=""
    CITC_USER=""
    VCS=""
  fi
}

zshaddhistory () {
  local cmd=${${(z)1}[1]}
  [[ -z $CITC_NAME || ( $cmd != blaze && $cmd != borgcfg && $cmd != rabbit ) ]] && return 0
  print -sr -- "${${1%%$'\n'}%%  \# Client:*}  # Client:$CITC_NAME Snapshot:$(<$CITC_ROOT/../.citc/snapshot_version)"
  return 1
}

typeset -g -a chpwd_functions
chpwd_functions+=set_current_client
set_current_client
