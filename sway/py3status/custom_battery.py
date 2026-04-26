import dbus
import dbus.mainloop.glib
import threading
import subprocess
from gi.repository import GLib


class Py3status:
  def post_config_hook(self):
    self.percentage = "N/A"
    self.state = "Unknown"
    self.lock = threading.Lock()
    self.low_battery_notified = False

    # Start background thread for D-Bus listening
    t = threading.Thread(target=self._listen_for_upower_events)
    t.daemon = True
    t.start()

  def _listen_for_upower_events(self):
    try:
      dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
      bus = dbus.SystemBus()

      # Listen to DisplayDevice which represents the main battery
      bus.add_signal_receiver(
        self._on_properties_changed,
        dbus_interface="org.freedesktop.DBus.Properties",
        signal_name="PropertiesChanged",
        path="/org/freedesktop/UPower/devices/DisplayDevice",
      )

      # Read initial state
      self._update_state(bus)

      loop = GLib.MainLoop()
      loop.run()
    except Exception:
      pass

  def _update_state(self, bus):
    try:
      obj = bus.get_object(
        "org.freedesktop.UPower",
        "/org/freedesktop/UPower/devices/DisplayDevice",
      )
      iface = dbus.Interface(obj, "org.freedesktop.DBus.Properties")

      percentage = iface.Get("org.freedesktop.UPower.Device", "Percentage")
      state = iface.Get("org.freedesktop.UPower.Device", "State")

      with self.lock:
        self.percentage = int(percentage)
        self.state = int(state)

      self.py3.update()
    except Exception:
      pass

  def _on_properties_changed(self, interface, changed_props, invalidated):
    if interface == "org.freedesktop.UPower.Device":
      with self.lock:
        if "Percentage" in changed_props:
          self.percentage = int(changed_props["Percentage"])
        if "State" in changed_props:
          self.state = int(changed_props["State"])

        # Check for low battery and send notification
        if (
          isinstance(self.percentage, int)
          and self.percentage <= 5
          and not getattr(self, "low_battery_notified", False)
        ):
          try:
            subprocess.run(
              [
                "notify-send",
                "-u",
                "critical",
                "Low Battery",
                f"Battery is at {self.percentage}%",
              ],
              check=True,
            )
            self.low_battery_notified = True
          except Exception:
            pass
        elif isinstance(self.percentage, int) and self.percentage > 5:
          self.low_battery_notified = False

      self.py3.update()

  def battery(self):
    with self.lock:
      percent = self.percentage
      state = self.state

    # State mapping for UPower:
    # 1: Charging, 2: Discharging, 4: Fully charged
    prefix = ""
    if state == 1:
      full_text = "⚡"
    elif state == 4:
      full_text = "🔌"
    else:
      blocks = "▁⡀⣀⣄⣤⣦⣶⣷⣾⣿█"
      if isinstance(percent, int):
        idx = min((percent + 5) // 10, len(blocks) - 1)
        full_text = blocks[idx]
      else:
        full_text = "🔋"

    return {"full_text": full_text, "cached_until": self.py3.CACHE_FOREVER}
