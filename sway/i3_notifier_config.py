import time
import i3ipc

from i3notifier.config import Config
from i3notifier.utils import RunAsyncFactory


def switch_to_urgent(timeout=3.0):
  def on_workspace_urgent(i3, data):
    i3.main_quit()
    i3.command(f"workspace {data.current.name}")

  def on_window_urgent(i3, data):
    i3.main_quit()
    i3.command("[urgent=latest] focus")

  try:
    i3 = i3ipc.Connection(auto_reconnect=True)
    i3.on(i3ipc.Event.WORKSPACE_URGENT, on_workspace_urgent)
    i3.on(i3ipc.Event.WINDOW_URGENT, on_window_urgent)
    i3.main(timeout)
  except Exception:
    pass


class DefaultConfig(Config):
  pre_action_hooks = [
    # Start listening for urgent workspaces & switch to it
    RunAsyncFactory(lambda _: switch_to_urgent()),
    # Wait for the listener to become available
    lambda _: time.sleep(0.2),
  ]


def ChromeAppFactory(title, url, icon=None, second_key="body"):
  kChrome = "Google Chrome"
  lURL = len(url)
  icon = icon or "chrome"

  class ChromeApp(DefaultConfig):
    def should_apply(notification):
      return (
        notification.body.startswith(url) and notification.app_name == kChrome
      )

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
    notification.app_name = notification.summary
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
