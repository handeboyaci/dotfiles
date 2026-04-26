import json
import threading
import time
from urllib.request import urlopen


class Py3status:
  def post_config_hook(self):
    self.prayer_times = {}
    self.all_prayer_data = {}
    self.location = None
    self.lock = threading.Lock()
    self.fetching = False

    # Start background thread to fetch data
    t = threading.Thread(target=self._fetch_data)
    t.daemon = True
    t.start()

  def _fetch_data(self, month=None, year=None):
    from datetime import datetime
    import os

    cache_path = "/usr/local/google/home/sselcuk/.dotfiles/tmp/namaz_cache.json"

    # 1. Check cache first
    cache_loaded = False
    if os.path.exists(cache_path):
      try:
        with open(cache_path, "r") as f:
          cache_data = json.load(f)
          data = {"data": cache_data["data"]}
          city = cache_data["location"]["city"]
          country = cache_data["location"]["country"]

        today = time.strftime("%d-%m-%Y")
        found = False
        with self.lock:
          for item in data["data"]:
            d_str = item["date"]["gregorian"]["date"]
            self.all_prayer_data[d_str] = item["timings"]
            if d_str == today:
              self.prayer_times = item["timings"]
              self.location = {"city": city, "country": country}
              found = True

        if found:
          with open("/usr/local/google/home/sselcuk/.namaz_debug.log", "a") as f:
            f.write(f"{datetime.now()}: Found today's times in cache. Using them.\n")
          self.py3.update()
          cache_loaded = True
      except Exception as e:
        with open("/usr/local/google/home/sselcuk/.namaz_debug.log", "a") as f:
          f.write(f"{datetime.now()}: Error reading cache at startup: {str(e)}\n")

    # 2. Proceed with network fetch to update cache
    try:
      # Geolocation
      response = urlopen("https://ipapi.co/json/")
      loc_data = json.loads(response.read().decode())
      city = loc_data["city"]
      country = loc_data["country_name"]

      # Aladhan API with Method 15 (Moonsighting Committee)
      import urllib.parse

      city_encoded = urllib.parse.quote(city)
      country_encoded = urllib.parse.quote(country)
      url = f"https://api.aladhan.com/v1/calendarByCity?city={city_encoded}&country={country_encoded}&method=15&iso8601=true"
      if month:
        url += f"&month={month}"
      if year:
        url += f"&year={year}"
      response = urlopen(url)
      data = json.loads(response.read().decode())

      # Save to cache
      cache_data = {
        "data": data["data"],
        "location": {"city": city, "country": country},
      }
      os.makedirs(os.path.dirname(cache_path), exist_ok=True)
      with open(cache_path, "w") as f:
        json.dump(cache_data, f)

      with open("/usr/local/google/home/sselcuk/.namaz_debug.log", "a") as f:
        f.write(f"{datetime.now()}: Data fetched and cached for {city}, {country}\n")

    except Exception as e:
      with open("/usr/local/google/home/sselcuk/.namaz_debug.log", "a") as f:
        f.write(f"{datetime.now()}: Network fetch failed: {str(e)}\n")
      if not cache_loaded:
        return
      else:
        return

    # Process data
    today = time.strftime("%d-%m-%Y")
    found = False
    with self.lock:
      for item in data["data"]:
        d_str = item["date"]["gregorian"]["date"]
        self.all_prayer_data[d_str] = item["timings"]
        if d_str == today:
          self.prayer_times = item["timings"]
          self.location = {"city": city, "country": country}
          found = True

    with open("/usr/local/google/home/sselcuk/.namaz_debug.log", "a") as f:
      f.write(f"{datetime.now()}: Times found for today: {found}\n")

    with self.lock:
      self.fetching = False
    self.py3.update()

  def namaz(self):
    with self.lock:
      times = self.prayer_times
      loc = self.location

    if not times:
      return {"full_text": "Namaz: Fetching...", "cached_until": self.py3.time_in(5)}

    if getattr(self, "show_full_day", False):

      def get_time(t_str):
        if "T" in t_str:
          t_str = t_str.split("T")[1]
        parts = t_str.split(":")
        return f"{parts[0]}:{parts[1]}"

      day_str = " | ".join(
        [
          get_time(times.get(p, ""))
          for p in ["Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"]
        ]
      )
      city = loc.get("city", "") if loc else ""
      if city:
        full_text = f"{city}: [{day_str}]"
      else:
        full_text = f"[{day_str}]"
      return {"full_text": full_text, "cached_until": self.py3.CACHE_FOREVER}

    # Calculate remaining time
    cur_time = time.localtime()
    cur_minutes = cur_time.tm_hour * 60 + cur_time.tm_min

    next_prayer = None
    next_prayer_minutes = None

    prayers = ["Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"]

    for p in prayers:
      t_str = times.get(p)
      if t_str:
        if "T" in t_str:
          t_str = t_str.split("T")[1]
        t_parts = t_str.split()[0].split(":")
        p_minutes = int(t_parts[0]) * 60 + int(t_parts[1])

        if p_minutes > cur_minutes:
          next_prayer = p
          next_prayer_minutes = p_minutes
          break

    if not next_prayer:
      import datetime

      today_dt = datetime.date.today()
      tomorrow_dt = today_dt + datetime.timedelta(days=1)
      tomorrow_str = tomorrow_dt.strftime("%d-%m-%Y")

      with self.lock:
        tomorrow_times = self.all_prayer_data.get(tomorrow_str)

      if tomorrow_times:
        fajr_str = tomorrow_times.get("Fajr")
        if fajr_str:
          if "T" in fajr_str:
            fajr_str = fajr_str.split("T")[1]
          fajr_parts = fajr_str.split()[0].split(":")
          fajr_minutes = int(fajr_parts[0]) * 60 + int(fajr_parts[1])

          kalan = (1440 - cur_minutes) + fajr_minutes
          saat = kalan // 60
          dk = kalan % 60
          full_text = f"{saat}sa{dk}dk"

          return {"full_text": full_text, "cached_until": self.py3.time_in(60)}

      # Trigger background fetch for next month
      with self.lock:
        fetching = getattr(self, "fetching", False)
        if not fetching:
          self.fetching = True

      if not fetching:
        tomorrow_month = tomorrow_dt.strftime("%m")
        tomorrow_year = tomorrow_dt.strftime("%Y")

        t = threading.Thread(
          target=self._fetch_data, args=(tomorrow_month, tomorrow_year)
        )
        t.daemon = True
        t.start()

      return {
        "full_text": "Fetching next month...",
        "cached_until": self.py3.time_in(5),
      }

    kalan = next_prayer_minutes - cur_minutes
    saat = kalan // 60
    dk = kalan % 60

    full_text = f"{saat}sa{dk}dk"

    return {"full_text": full_text, "cached_until": self.py3.time_in(60)}

  def on_click(self, event):
    if event["button"] == 1:
      self.show_full_day = not getattr(self, "show_full_day", False)
      self.py3.update()
    elif event["button"] == 3:
      # Force refetch in background
      t = threading.Thread(target=self._fetch_data)
      t.daemon = True
      t.start()
      self.py3.update()
