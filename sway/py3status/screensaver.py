import shutil
import subprocess


class Py3status:
  def screensaver(self):
    # Check if swayidle is running
    result = subprocess.run(
      ["pkill", "-0", "swayidle"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
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
        ["pkill", "-0", "swayidle"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
      )
      running = result.returncode == 0

      if running:
        subprocess.run(
          ["pkill", "swayidle"],
          stdout=subprocess.PIPE,
          stderr=subprocess.PIPE,
        )
      else:
        if shutil.which("swayidle"):
          subprocess.Popen(
            ["swayidle"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
          )

      self.py3.update()
