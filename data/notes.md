# in jupyterlab vim extension, remove modifyEdgeSomething

git clone --recursive https://github.com/sencer/dotfiles.git $HOME/.dotfiles

VERSION=20

sudo apt install \
  aptitude autoconf automake bat bison brightnessctl build-essential check cmake \
  curl dconf-editor dos2unix fd-find flex foot fzf gettext \
  git gnome-calendar grim jq kanshi libbrotli-dev libbz2-dev libcairo2-dev \
  libevdev-dev libfreetype-dev libgdk-pixbuf-2.0-dev libjson-c-dev \
  libpango1.0-dev libpng-dev libstartup-notification0-dev libsystemd-dev \
  libwayland-dev libwlroots-0.${VERSION}-dev wayland-protocols \
  libxcb-util-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xrm-dev \
  libxkbcommon-x11-dev meson ninja-build nwg-bar pkg-config playerctl \
  python3-pip ripgrep scdoc slurp swaybg swayidle swaylock swayosd tree-sitter-cli \
  tmux unzip wl-clipboard  wdisplays xdg-desktop-portal-wlr zlib1g-dev \
  papirus-icon-theme fonts-noto-color-emoji

mkdir -p $HOME/.local/bin
quiet_tmp=$(mktemp -d)
cd "$quiet_tmp"
curl -fLO https://github.com/sencer/quiet/releases/latest/download/quiet-x86_64-unknown-linux-gnu.tar.gz
tar -xzf quiet-x86_64-unknown-linux-gnu.tar.gz
install -m 0755 quiet quietctl $HOME/.local/bin
cd $HOME
rm -rf "$quiet_tmp"

launcher_tmp=$(mktemp -d)
cd "$launcher_tmp"
curl -fLO https://github.com/sencer/launcher/releases/latest/download/launcher-x86_64-unknown-linux-gnu.tar.gz
tar -xzf launcher-x86_64-unknown-linux-gnu.tar.gz
install -m 0755 launcher $HOME/.local/bin
cd $HOME
rm -rf "$launcher_tmp"

sudo bash -c '
file="/etc/xdg/swayosd/backend.toml"
mkdir -p "$(dirname "$file")"
if grep -qs "^[# ]*ignore_caps_lock_key" "$file"; then
    sed -i "s/^[# ]*ignore_caps_lock_key.*/ignore_caps_lock_key = true/" "$file"
elif grep -qs "^\[input\]" "$file"; then
    sed -i "/^\[input\]/a ignore_caps_lock_key = true" "$file"
else
    printf "\n[input]\nignore_caps_lock_key = true\n" >> "$file"
fi
' && sudo systemctl restart swayosd-libinput-backend


mkdir -p $HOME/software
cd $HOME/software

git clone --recursive https://github.com/swaywm/sway.git
cd sway
git checkout $(git grep "wlroots_version.*$VERSION" $(git rev-list --all)|head -1|cut -d: -f1)
git am $HOME/.dotfiles/data/fullscreen.patch
meson setup build/
ninja -C build/
sudo ninja -C build/ install

cd $HOME
rm -f .zshenv
ln -s $HOME/.dotfiles/zsh/zshenv .zshenv
for f in bashrc profile gitconfig inputrc tmux.conf fonts
do
  rm -rf .$f
  ln -s $HOME/.dotfiles/$f .$f
done


cd $HOME/.config
for f in foot jj kanshi meet-stream.conf nvim nwg-bar pipewire statusbar.conf sunshine sway swayidle swaylock systemd wireplumber xdg-desktop-portal
do
  rm -rf $f
  ln -s $HOME/.dotfiles/$f
done

cd $HOME/.local/share; rm -rf applications; ln -s $HOME/.dotfiles/applications
mkdir -p ~/.local/share/icons/hicolor/
cd ~/.local/share/icons/hicolor/; ln -s ~/.dotfiles/icons 256x256

mkdir -p $HOME/.gemini/jetski/cli
for f in keybindings.json settings.json; do
  rm -f $HOME/.gemini/jetski/cli/$f
  ln -s $HOME/.dotfiles/jetski/$f $HOME/.gemini/jetski/cli/$f
done

cd $HOME/.dotfiles/tmux/tmux-thumbs
sh tmux-thumbs-install.sh

cd $HOME/software
git clone https://github.com/neovim/neovim.git
cd neovim
make CMAKE_BUILD_TYPE=Release CMAKE_INSTALL_PREFIX=/usr -j8
sudo make install
cd ..

sudo rm -f \
  /usr/share/nvim/runtime/autoload/netrw.vim \
  /usr/share/nvim/runtime/autoload/netrwFileHandlers.vim \
  /usr/share/nvim/runtime/autoload/netrwSettings.vim \
  /usr/share/nvim/runtime/autoload/netrw_gitignore.vim \
  /usr/share/nvim/runtime/doc/pi_netrw.txt \
  /usr/share/nvim/runtime/plugin/netrwPlugin.vim \
  /usr/share/nvim/runtime/syntax/netrw.vim

nvim --headless -c "lua require('nvim-treesitter').install({'all'}):wait(300000) vim.cmd('qa')"

nvim --headless \
  -c 'MasonInstall bash-language-server basedpyright lua-language-server ltex-ls pyink shellcheck shfmt texlab uv vim-language-server vint' \
  -c qall

sudo systemctl mask --now ctrl-alt-del.target swayosd-libinput-backend.service
sudo systemctl daemon-reload

# Initialize Google Calendar OAuth token (requires ~/.dotfiles/tmp/google_calendar.credentials.json)
$HOME/.dotfiles/bin/gcal-auth

cd $HOME/.dotfiles/data
make
sudo make install
cd $HOME

systemctl --user enable --now mpris-proxy.service notification.service bt-media-helper.service swayidle.service swayosd-server.service

# Sunshine headless streaming setup
sudo usermod -aG input $USER
$HOME/software/sunshine.AppImage --install
systemctl --user enable --now sunshine-headless.socket

# Fullmoon unified companion (Host: Sunshine clipboard sync + on-demand webcam & mic receiver)
sudo apt install -y linux-main-modules-v4l2loopback-$(uname -r) v4l2loopback-utils \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
sudo bash -c 'cat <<EOF > /etc/modprobe.d/v4l2loopback.conf
options v4l2loopback video_nr=10 card_label="Moonlight Virtual Camera" exclusive_caps=1 max_buffers=8
EOF'
sudo bash -c 'echo "v4l2loopback" > /etc/modules-load.d/v4l2loopback.conf'
sudo modprobe v4l2loopback
systemctl --user disable --now fullmoon.service 2>/dev/null || true
systemctl --user enable --now fullmoon.socket

# Fullmoon unified companion (Client: Moonlight wrapper + on-demand webcam & mic streamer)
sudo apt install -y gstreamer1.0-tools gstreamer1.0-pipewire gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
sudo usermod -aG video $USER
# Sync secret key to host: $HOME/.dotfiles/data/fullmoon --sync-key (or scp -p ~/.dotfiles/tmp/meet-stream.key ev:~/.dotfiles/tmp/)
# Launch Moonlight normally (automatic two-way clipboard sync + on-demand media streaming)
# Select/switch webcam & microphone with $mod+s


# =============================================================================
# macOS (Apple silicon): shell + tmux + nvim + Ghostty. Everything above is Linux.
# =============================================================================
# System Settings first: Caps Lock -> Control; free Ctrl+Space (Keyboard Shortcuts ->
# Input Sources); "Use F1, F2, etc. keys as standard function keys" = ON.

xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
# This session only. Do NOT add it to ~/.zprofile as the installer suggests:
# profile + zsh/zprofile handle PATH (and ~/.zprofile is not read once ZDOTDIR is set).
eval "$(/opt/homebrew/bin/brew shellenv)"
brew analytics off

# macOS ships BSD ls/cp/sed/grep/find/tar/awk. The GNU ones below are required: profile
# puts their libexec/gnubin dirs ahead of /usr/bin, so they run under their plain names.
# zsh: Apple's /bin/zsh has no zsh/pcre (Homebrew's is built with --enable-pcre).
# rsync: Apple's lacks --info=progress2.
brew install \
  zsh bash \
  coreutils gnu-sed grep findutils gnu-tar gawk \
  tmux fzf fd ripgrep less rsync \
  tree-sitter-cli node python \
  git gh jj rust \
  uv jq bat curl wget dos2unix
brew install --cask ghostty

# Neovim: the config uses dev-only features (TextPutPost, multicursor), so stable 0.12.x
# fails at startup. Try stable first; if the check does not print 1, use HEAD.
brew install neovim
nvim --clean --headless -c 'echo exists("##TextPutPost")' -c qa   # must print 1
# brew uninstall neovim && brew install --HEAD neovim

grep -qx /opt/homebrew/bin/zsh /etc/shells || echo /opt/homebrew/bin/zsh | sudo tee -a /etc/shells
chsh -s /opt/homebrew/bin/zsh
gh auth login

git clone --recursive https://github.com/sencer/dotfiles.git $HOME/.dotfiles
cp $HOME/.dotfiles/fonts/*.ttf $HOME/.dotfiles/fonts/fira/*.ttf $HOME/Library/Fonts/
cd $HOME/.dotfiles/tmux/tmux-thumbs && cargo build --release

# Symlinks. Existing files are moved to a backup dir, never deleted (ssh keys are gitignored!).
D=$HOME/.dotfiles; B=$HOME/.dotfiles-backup-$(date +%Y%m%d-%H%M%S)
link() {
  if [ -L "$2" ]; then rm "$2"; elif [ -e "$2" ]; then mkdir -p "$B" && mv "$2" "$B/"; fi
  mkdir -p "$(dirname "$2")" && ln -s "$1" "$2"
}
if [ -d $HOME/.ssh ] && [ ! -L $HOME/.ssh ]; then
  for k in $HOME/.ssh/*; do [ -e "$k" ] && cp -np "$k" $D/ssh/ || true; done
fi
for f in bashrc profile gitconfig inputrc tmux.conf ssh; do link $D/$f $HOME/.$f; done
for f in ghostty jj nvim; do link $D/$f $HOME/.config/$f; done
for f in keybindings.json settings.json; do link $D/jetski/$f $HOME/.gemini/jetski/cli/$f; done
link $D/zsh/zshenv $HOME/.zshenv   # last: this one activates everything

# Then, in a new Ghostty window: the two `nvim --headless` bootstrap commands above.

## ITerm
# When Ghostty cannot be installed, configure iTerm2 instead:
$HOME/.dotfiles/iterm2/setup.sh
# Notes:
# - Uses a Dynamic Profile (iterm2/dotfiles.json) for colors, FiraCode-Medium 12,
#   10% transparency, Left Option = Esc+ / Right Option = Normal, xterm-256color,
#   and Shift+Enter / Ctrl+Shift+Space CSI u sequences.
# - Sets iTerm2 defaults for clipboard access (OSC 52 writes), zero margins, and
#   disables quit/close prompts.
# - Limitations vs Ghostty: iTerm2 does not support a separate Light italic font
#   weight, and OSC 52 clipboard reads (tmux refresh-client -l) may still prompt.
