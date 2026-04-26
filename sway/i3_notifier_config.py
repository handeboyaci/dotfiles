import subprocess
import time

from i3notifier.config import Config
from i3notifier.utils import RunAsyncFactory


class DefaultConfig(Config):
  pre_action_hooks = [
    # Start a script to listen for urgent workspaces & switch to it
    RunAsyncFactory(lambda _: subprocess.call("switch-to-urgent.py")),
    # Wait for the script become available
    lambda _: time.sleep(0.2),
  ]


def ChromeAppFactory(title, url, icon=None, second_key="body"):
  kChrome = "Google Chrome"
  lURL = len(url)
  icon = icon or "chrome"

  class ChromeApp(DefaultConfig):
    def should_apply(notification):
      return notification.body.startswith(url) and notification.app_name == kChrome

    def update_notification(notification):
      notification.body = notification.body[lURL:].strip()
      notification.app_name = title
      notification.app_icon = icon

    def get_keys(notification):
      return title, str(getattr(notification, second_key))

  return ChromeApp


class Chrome(DefaultConfig):
  def should_apply(notification):
    return notification.app_name == "Google Chrome"

  def update_notification(notification):
    splitted = notification.body.split("\n")
    notification.app_name = splitted[0]
    notification.app_icon = "google-chrome"
    notification.body = "\n".join(splitted[2:])

  def get_keys(notification):
    return notification.app_name


class NotifySend(DefaultConfig):
  expires = True

  def should_apply(notification):
    return notification.app_name == "notify-send"

  def update_notification(notification):
    notification.app_icon = "plugin-notification"


Gmail = ChromeAppFactory("Gmail", "mail.google.com", "gmail")

Chat = ChromeAppFactory("Chat", "chat.google.com", "google-chat")

Meet = ChromeAppFactory("Meet", "meet.google.com", "meet")

config_list = [
  Gmail,
  Chat,
  Meet,
  ChromeAppFactory("WhatsApp", "web.whatsapp.com", "whatsapp", "summary"),
  ChromeAppFactory("Twitter", "twitter.com", "twitter"),
  ChromeAppFactory("Instagram", "www.instagram.com", "photos"),
  Chrome,
  NotifySend,
  DefaultConfig,
]

theme = "widget"
