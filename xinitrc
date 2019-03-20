eval $(/usr/bin/gnome-keyring-daemon --start --components=pkcs11,secrets,ssh)
export SSH_AUTH_SOCK
feh --bg-fill $HOME/.config/wallpaper.jpg

TZ="America/New_York"
