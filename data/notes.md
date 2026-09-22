# in jupyterlab vim extension, remove modifyEdgeSomething

git clone --recursive git@github.com:sencer/dotfiles.git $HOME/.dotfiles

VERSION=20

sudo apt install \
  aptitude autoconf automake bat bison brightnessctl build-essential check cmake \
  curl dconf-editor dos2unix fd-find flex foot fzf gettext gir1.2-playerctl-2.0 \
  git gnome-calendar grim jq kanshi libcairo2-dev libgdk-pixbuf-2.0-dev libjson-c-dev \
  libcurl4-openssl-dev libdbusmenu-gtk3-dev libevdev-dev libfmt-dev libgtkmm-3.0-dev \
  libjsoncpp-dev libnl-3-dev libnl-genl-3-dev libpango1.0-dev libplayerctl-dev \
  libpulse-dev libspdlog-dev libstartup-notification0-dev libsystemd-dev libupower-glib-dev \
  libwayland-dev libwlroots-0.${VERSION}-dev wayland-protocols \
  libxcb-util-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xrm-dev \
  libxkbcommon-x11-dev meson ninja-build nwg-bar pkg-config playerctl py3status \
  python3-daemon python3-dateutil python3-googleapi python3-oauth2client python3-pip \
  ripgrep scdoc slurp swaybg swayidle swaylock swayosd tmux unzip wl-clipboard \
  wdisplays xdg-desktop-portal-wlr papirus-icon-theme rofi

mkdir -p $HOME/.local/bin
quiet_tmp=$(mktemp -d)
cd "$quiet_tmp"
curl -fLO https://github.com/sencer/quiet/releases/latest/download/quiet-x86_64-unknown-linux-gnu.tar.gz
tar -xzf quiet-x86_64-unknown-linux-gnu.tar.gz
install -m 0755 quiet quietctl $HOME/.local/bin
cd $HOME
rm -rf "$quiet_tmp"

pip install --break-system-packages py3notifier 

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
for f in foot jj kanshi nvim nwg-bar pipewire rofi sunshine sway swayidle swaylock swayosd systemd waybar xdg-desktop-portal
do
  rm -rf $f
  ln -s $HOME/.dotfiles/$f
done

cd $HOME/.local/share; rm -rf applications; ln -s $HOME/.dotfiles/applications
mkdir -p ~/.local/share/icons/hicolor/
cd ~/.local/share/icons/hicolor/; ln -s ~/.dotfiles/icons 256x256

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

nvim --headless -c "TSInstallSync all" -c qall

nvim --headless \
  -c 'MasonInstall bash-language-server basedpyright lua-language-server pyink ruff shellcheck shfmt stylua texlab uv vim-language-server vint' \
  -c qall


sudo systemctl mask ctrl-alt-del.target
sudo systemctl daemon-reload
systemctl --user enable --now kanshi.service notification.service swaybg.service swayidle.service swayosd-server.service

waybar_tmp=$(mktemp -d)
cd "$waybar_tmp"
curl -fLO https://github.com/sencer/Waybar/releases/download/v0.15.0-custom.3/waybar-x86_64-unknown-linux-gnu.tar.gz
tar -xzf waybar-x86_64-unknown-linux-gnu.tar.gz
install -m 0755 waybar-x86_64-unknown-linux-gnu/{waybar,pixelbuds-pause} $HOME/.local/bin
cd $HOME
rm -rf "$waybar_tmp"

# Source-build fallback for a different architecture or newer custom branch:
# git clone --branch custom-modules https://github.com/sencer/Waybar.git $HOME/software/waybar
# meson setup $HOME/software/waybar/build $HOME/software/waybar --prefix=$HOME/.local
# ninja -C $HOME/software/waybar/build install

playerctld daemon
