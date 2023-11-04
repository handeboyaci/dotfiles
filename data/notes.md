apt install build-essential cmake autoconf pkg-config git i3 jq xautolock aptitude xcape libfontconfig-dev tmux gnome-flashback xss-lock ninja-build gettext unzip curl fzf flex bison check automake meson libglib2.0-dev libcairo2-dev libpango1.0-dev libxkbcommon-dev libgdk-pixbuf2.0-dev libxcb-xkb-dev libxcb-randr0-dev libxcb-xinerama0-dev libxcb1-dev libxcb-util-dev libxkbcommon-x11-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-xrm-dev libstartup-notification0-dev python3-pip xsecurelock compton dconf-editor gnome-calendar npm python3-venv
pip install --user --break-system-packages py3status ipython numpy scipy pandas seaborn matplotlib i3-notifier py3notifier google-api-python-client oauth2client pynvim python-daemon

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
cargo install alacritty fd-find ripgrep

mkdir software
cd software

git clone https://github.com/neovim/neovim.git
cd neovim
make CMAKE_BUILD_TYPE=Release CMAKE_INSTALL_PREFIX=/usr  -j8
sudo make install
cd ..

git clone https://github.com/regolith-linux/i3-gnome-flashback.git
cd i3-gnome-flashback
sudo make install
cd ..

git clone https://github.com/davatorium/rofi.git
cd rofi
git checkout 1.6.1
git submodule update --init
meson setup build --prefix=/usr
ninja -C build install
cd ..

sudo update-alternatives --install /usr/bin/x-terminal-emulator x-terminal-emulator $HOME/.cargo/bin/alacritty 50

cd ~
scp -r sselcuk3.nyc.corp.google.com:.dotfiles/ .
rm -f .zshenv
ln -s ~/.dotfiles/zsh/zshenv .zshenv
ln -s ~/.dotfiles/tmux.conf .tmux.conf
ln -s ~/.dotfiles/fonts .fonts
rm -rf ~/.ssh
ln -s ~/.dotfiles/ssh .ssh
cd .config
ln -s ~/.dotfiles/i3
ln -s ~/.dotfiles/alacritty.yml
ln -s ~/.dotfiles/rofi
cd ~/.local/share
ln -s ~/.dotfiles/applications

gsettings set org.gnome.desktop.wm.keybindings switch-input-source "[]"
gsettings set org.gnome.desktop.wm.keybindings switch-input-source-backward "[]"
gsettings set org.gnome.desktop.input-sources xkb-options "['caps:ctrl_modifier', 'nbsp:level2', 'grp:alt_space_toggle']"


cd $HOME/.dotfiles/tmux/tmux-thumbs
sh tmux-thumbs-install.sh

sudo rm -f \
  /usr/share/nvim/runtime/autoload/netrw.vim \
  /usr/share/nvim/runtime/autoload/netrwFileHandlers.vim \
  /usr/share/nvim/runtime/autoload/netrwSettings.vim \
  /usr/share/nvim/runtime/autoload/netrw_gitignore.vim \
  /usr/share/nvim/runtime/doc/pi_netrw.txt \
  /usr/share/nvim/runtime/plugin/netrwPlugin.vim \
  /usr/share/nvim/runtime/syntax/netrw.vim

cd ~/.config
ln -s ~/.dotfiles/nvim

nvim --headless -c "TSInstall all" -c qall
# Update the mason install list
nvim --headless \
  -c 'packadd mason.nvim' \
  -c 'lua require("mason").setup{}' \
  -c 'MasonInstall ltex-ls lua-language-server pyink pyright ruff-lsp shellcheck shellharden shfmt texlab vim-language-server vint' \
  -c qall
