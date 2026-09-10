#!/bin/bash

# Ensure the script is run with sudo
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (use sudo)"
  exit 1
fi

echo "Step 1: Fixing /dev/uinput permissions..."
# 1. Fix /dev/uinput permissions permanently
echo 'KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"' > /etc/udev/rules.d/60-sunshine-uinput.rules

echo "Step 2: Disabling aggressive isolation rule..."
# 2. Remove the isolation rule that blocks Sway from seeing the devices
if [ -f /etc/udev/rules.d/85-sunshine-input-isolation.rules ]; then
    mv /etc/udev/rules.d/85-sunshine-input-isolation.rules /etc/udev/rules.d/85-sunshine-input-isolation.rules.bak
    echo "Backed up and disabled aggressive isolation rule."
fi

echo "Step 3: Applying udev changes..."
# 3. Apply udev changes
udevadm control --reload-rules
udevadm trigger

# Get the actual user who invoked sudo
REAL_USER=${SUDO_USER:-$USER}
USER_ID=$(id -u "$REAL_USER")

echo "Step 4: Restarting Sunshine and Sway services for user $REAL_USER ($USER_ID)..."
# 4. Reload and restart the user services
# Note: We use -u to run as the real user since these are --user services
sudo -u "$REAL_USER" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$USER_ID/bus" systemctl --user daemon-reload
sudo -u "$REAL_USER" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$USER_ID/bus" systemctl --user restart sway-sunshine.service sunshine-headless.service

echo "-------------------------------------------------------"
echo "Done! To verify, connect via Moonlight and then run:"
echo "SWAYSOCK=/run/user/$USER_ID/sway-sunshine.sock swaymsg -t get_inputs"
echo "-------------------------------------------------------"
