import threading
import subprocess
import shutil
from dbus import SystemBus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib, Playerctl


class Py3status:
  def post_config_hook(self):
    """
    Setup background threads for PipeWire (Volume) and BlueZ (Bluetooth Wear).
    """
    # Initialize Playerctl Manager to track media players natively
    self.player_manager = Playerctl.PlayerManager()

    # Thread 1: PipeWire Volume Listener
    t_vol = threading.Thread(target=self._listen_for_pw_events)
    t_vol.daemon = True
    t_vol.start()

    # Thread 2: Bluetooth Wear Listener
    t_bt = threading.Thread(target=self._listen_for_bluez_events)
    t_bt.daemon = True
    t_bt.start()

  def _listen_for_pw_events(self):
    """Monitors PipeWire for volume/mute changes."""
    if not shutil.which("pw-mon"):
      return
    try:
      process = subprocess.Popen(
        ["pw-mon"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
      )
      for line in process.stdout:
        # Trigger a refresh on any property or volume change
        if any(x in line.lower() for x in ["props", "volume", "mute"]):
          self.py3.update()
    except Exception:
      pass

  def _listen_for_bluez_events(self):
    """Listens for BlueZ state changes (Pixel Buds in/out of ear)."""
    try:
      DBusGMainLoop(set_as_default=True)
      bus = SystemBus()

      bus.add_signal_receiver(
        self._on_bluez_signal,
        dbus_interface="org.freedesktop.DBus.Properties",
        signal_name="PropertiesChanged",
        bus_name="org.bluez",
      )

      loop = GLib.MainLoop()
      loop.run()
    except Exception:
      pass

  def _on_bluez_signal(self, interface, changed_props, invalidated):
    """
    If the Bluetooth MediaTransport state becomes 'idle',
    pause all players via native Playerctl.
    """
    if interface == "org.bluez.MediaTransport1":
      state = changed_props.get("State")
      if state == "idle":
        # Pause every player managed by Playerctl
        for player in self.player_manager.props.players:
          player.pause()

  def volume_wpctl(self):
    """
    Builds the status bar display using wpctl.
    """
    try:
      output = self.py3.command_output("wpctl get-volume @DEFAULT_AUDIO_SINK@")
      # Output: "Volume: 0.50 [MUTED]" or "Volume: 0.50"
      parts = output.split()

      vol_float = float(parts[1])
      vol_percent = int(vol_float * 100)
      muted = "[MUTED]" in output

      if muted:
        full_text = "  🔇  "
      else:
        full_text = f"🔊 {vol_percent}%"

      return {
        "full_text": full_text,
        "cached_until": self.py3.CACHE_FOREVER,
      }
    except Exception:
      return {
        "full_text": "VOL: ERR",
        "color": self.py3.COLOR_BAD,
        "cached_until": self.py3.CACHE_FOREVER,
      }

  def on_click(self, event):
    """Mouse controls for volume."""
    if event["button"] == 1:
      self.py3.command_run("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle")
    elif event["button"] == 4:
      self.py3.command_run("wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 2%+")
    elif event["button"] == 5:
      self.py3.command_run("wpctl set-volume @DEFAULT_AUDIO_SINK@ 2%-")

    self.py3.update()
