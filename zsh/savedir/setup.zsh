: ${SAVEDDIRS:=$HOME/.saveddirs}

[[ -f $SAVEDDIRS ]] || touch $SAVEDDIRS

fpath=( ${0:h} $fpath )

autoload -U save
autoload -U del

alias show="cat $SAVEDDIRS"

source "$SAVEDDIRS"
