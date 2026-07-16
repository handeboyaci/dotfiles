export EDITOR=nvim
export VISUAL=nvim
export BROWSER=xdg-open
export LANG=en_US.UTF-8

export TMPDIR=/tmp/$USER
mkdir -p -m 700 "$TMPDIR"

export PAGER=less
export LESS='-g -i -M -R -W -z-4 -X -F -x2'
export LESSOPEN="|$HOME/.dotfiles/zsh/lesspipe %s"
export LESSCLOSE="$HOME/.dotfiles/zsh/lesspipe %s %s"

export P4DIFF='nvim -d +"nnoremap <Leader>q :qa<CR>" +"normal gg"'
export P4MERGE='$HOME/.dotfiles/bin/p4merge'

export SUDO_ASKPASS=/usr/lib/openssh/gnome-ssh-askpass

export G4PENDINGSTYLE=relativepath
export REPLACE_BLAZE_WITH_DBIP=1
