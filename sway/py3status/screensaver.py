import subprocess


class Py3status:
  def screensaver(self):
    # Check if swayidle service is active
    result = subprocess.run(
      ["systemctl", "--user", "is-active", "--quiet", "swayidle.service"]
    )
    running = result.returncode == 0

    if running:
      icon = "💤"
    else:
      icon = "👁"

    return {"full_text": icon, "cached_until": self.py3.CACHE_FOREVER}

  def on_click(self, event):
    if event["button"] == 1:
      result = subprocess.run(
        ["systemctl", "--user", "is-active", "--quiet", "swayidle.service"]
      )
      running = result.returncode == 0

      if running:
        subprocess.run(
          ["systemctl", "--user", "stop", "swayidle.service"],
          stdout=subprocess.DEVNULL,
          stderr=subprocess.DEVNULL,
        )
      else:
        subprocess.run(
          ["systemctl", "--user", "start", "swayidle.service"],
          stdout=subprocess.DEVNULL,
          stderr=subprocess.DEVNULL,
        )

      self.py3.update()
