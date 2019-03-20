#!/bin/zsh

zparseopts -D -- d=dry i=onlyif e=escape t=term s:=server h=help -help=help

function usage {
  cat << EOF
  ras [-d] [-e] [-t] [-s server] command
  -d         dry run
  -e         escape special chars
  -t         run ssh as ssh -t (with a terminal)
  -s server  specify a server manually
  -i         run local command if no server is found [doesn't make sense with -s]
  -h --help  this screen
EOF
}


if (( $+term[1] )); then
  term='-t'
fi

if (( $+escape[1] )); then
  cmd="${(b)@}"
else
  cmd="${@}"
fi

if (( $+server[2] )); then
  server=${server[2]}
elif [[ -n $CURRENTSERVER ]]; then
  server=$CURRENTSERVER
  if [[ $PWD == ${HOME}/${server} ]]; then
    cmd="$cmd"
  else
    cmd="cd ${PWD#$HOME/$server/}; $cmd"
  fi
else
  if (( $+onlyif[1] )); then
    eval $cmd
  else
    echo "No remote server specified."
  fi
  exit 1
fi

if [[ $# -lt 1 || -n $help ]]; then
  usage
  exit 1
fi

if (( $+dry[1] )); then
  echo ssh -q $term $server "\"$cmd\""
else
  ssh -q $term $server "$cmd"
fi
