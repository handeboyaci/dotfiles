# in jupyterlab vim extension, remove modifyEdgeSomething

git clone --recursive git@github.com:sencer/dotfiles.git $HOME/.dotfiles

sudo apt install aptitude autoconf autoconf automake bat bison brightnessctl \
  build-essential check cmake curl dconf-editor dconf-editor dos2unix fd-find flex \
  foot fzf gettext gir1.2-playerctl-2.0 git gnome-calendar grim \
  gtk-desktop-portal-wlr jq kanshi libcairo2-dev libgdk-pixbuf-2.0-dev \
  libjson-c-dev libpango1.0-dev libstartup-notification0-dev libwlroots-0.19-dev \
  libxcb-util-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xrm-dev \
  libxkbcommon-x11-dev meson neovim ninja-build nwg-bar pkg-config py3status \
  python3-daemon python3-dateutil python3-googleapi python3-oauth2client \
  python3-pip ripgrep scdoc slurp swaybg swayidle swaylock swayosd tmux unzip \
  wl-clipboard wdisplays

pip install --break-system-packages i3-notifier py3notifier 
git clone https://github.com/davatorium/rofi.git
cd rofi
git checkout 1.6.1
git submodule update --init
meson setup build --prefix=/usr
ninja -C build install
cd ..

git clone --recursive https://github.com/swaywm/sway.git
sudo apt install libwlroots-$VERSION-dev
git grep 'wlroots_version.*$VERSION' $(git rev-list --all)
git am $HOME/.dotfiles/data/fullscreen.patch
meson setup build/
ninja -C build/
sudo ninja -C build/ install

cd $HOME
rm -rf .zshenv $HOME/.ssh
for f in zshenv gitconfig inputrc tmux.conf fonts
do
  ln -s $HOME/.dotfiles/$f .$f
done


cd $HOME/.config
for f in i3 rofi foot kanshi nvim rofi sway swayidle swaylock swayosd systemd xdg-desktop-portal
do
  ln -s $HOME/.dotfiles/$f
done

cd $HOME/.local/share; rm -rf applications; ln -s $HOME/.dotfiles/applications
cd ~/.local/share/icons/hicolor/; ln -s ~/.dotfiles/icons 256x256


mkdir software
cd software

cd $HOME/.dotfiles/tmux/tmux-thumbs
sh tmux-thumbs-install.sh

git clone https://github.com/neovim/neovim.git
cd neovim
make CMAKE_BUILD_TYPE=Release CMAKE_INSTALL_PREFIX=/usr  -j8
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

nvim --headless -c "TSInstall all" 

nvim --headless \
  -c 'packadd mason.nvim' \
  -c 'lua require("mason").setup{}' \
  -c 'MasonInstall lua-language-server pyink basedpyright ruff uv shellcheck shfmt vim-language-server vint' \
  -c qall


sudo systemctl mask ctrl-alt-del.target
sudo systemctl daemon-reload
