export EDITOR=nvim
export VISUAL=nvim
export LANG=en_US.UTF-8

# $OSTYPE is set by zsh and bash; plain sh leaves it empty and takes the Linux branch.
case "$OSTYPE" in
  darwin*)
    # `brew shellenv` forks, so run it once; child shells inherit HOMEBREW_PREFIX.
    # zsh/zprofile re-promotes Homebrew after /etc/zprofile's path_helper.
    [ -z "$HOMEBREW_PREFIX" ] && [ -x /opt/homebrew/bin/brew ] && eval "$(/opt/homebrew/bin/brew shellenv)"
    # GNU tools under their plain names (ls, cp, find, sed, grep, tar, awk, dircolors, ...),
    # for scripts too. Explicit list: a blanket opt/*/gnubin would also shadow Apple's libtool.
    for _f in coreutils findutils gnu-sed grep gnu-tar gawk; do
      _d="$HOMEBREW_PREFIX/opt/$_f/libexec/gnubin"
      [ -d "$_d" ] && case ":$PATH:" in *":$_d:"*) ;; *) PATH="$_d:$PATH" ;; esac
    done
    unset _f _d
    export BROWSER=open
    ;;
  *)
    export BROWSER=xdg-open
    export TMPDIR="/tmp/$USER"
    export SUDO_ASKPASS=/usr/lib/openssh/gnome-ssh-askpass
    ;;
esac
# Needed on both OSes: ssh ControlPath (/tmp/%u/%C) and zsh TMPPREFIX.
[ -d "/tmp/$USER" ] || mkdir -p -m 700 "/tmp/$USER"

export PAGER=less
export LESS='-g -i -M -R -W -z-4 -X -F -x2'
export LESSOPEN="|$HOME/.dotfiles/zsh/lesspipe %s"
export LESSCLOSE="$HOME/.dotfiles/zsh/lesspipe %s %s"

export P4DIFF='nvim -d +"nnoremap <Leader>q :qa<CR>" +"normal gg"'
export P4MERGE='$HOME/.dotfiles/bin/p4merge'

export G4PENDINGSTYLE=relativepath
export REPLACE_BLAZE_WITH_DBIP=1
