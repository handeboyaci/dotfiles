# in jupyterlab vim extension, remove modifyEdgeSomething

git clone --recursive git@github.com:sencer/dotfiles.git $HOME/.dotfiles

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
  papirus-icon-theme rofi fonts-noto-color-emoji

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
for f in foot jj kanshi meet-stream.conf nvim nwg-bar pipewire rofi statusbar.conf sunshine sway swayidle swaylock systemd wireplumber xdg-desktop-portal
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


sudo systemctl mask --now ctrl-alt-del.target swayosd-libinput-backend.service
sudo systemctl daemon-reload

# Initialize Google Calendar OAuth token (requires ~/.dotfiles/tmp/google_calendar.credentials.json)
$HOME/.dotfiles/bin/gcal-auth

cd $HOME/.dotfiles/data
make
sudo make install
cd $HOME

systemctl --user enable --now mpris-proxy.service notification.service pixelbuds-pause.service swayidle.service

# Sunshine headless streaming setup
sudo usermod -aG input $USER
$HOME/software/sunshine.AppImage --install
systemctl --user enable --now sunshine-headless.socket

# Moonlight Meet webcam & microphone forwarding setup (Host)
sudo apt install -y linux-main-modules-v4l2loopback-$(uname -r) v4l2loopback-utils \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
sudo bash -c 'cat <<EOF > /etc/modprobe.d/v4l2loopback.conf
options v4l2loopback video_nr=10 card_label="Moonlight Virtual Camera" exclusive_caps=1
EOF'
sudo bash -c 'echo "v4l2loopback" > /etc/modules-load.d/v4l2loopback.conf'
sudo modprobe v4l2loopback
systemctl --user disable --now moonlight-cam.service 2>/dev/null || true
systemctl --user enable --now moonlight-cam.socket

# Moonlight Meet webcam & microphone forwarding setup (Client)
sudo apt install -y gstreamer1.0-tools gstreamer1.0-pipewire gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
sudo usermod -aG video $USER
# Copy ~/.dotfiles/tmp/meet-stream.key (mode 0600) between client and host
# Toggle streaming from client with $mod+s
# Select/switch webcam & microphone with $mod+Shift+s (or meet-stream --select)

