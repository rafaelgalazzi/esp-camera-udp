"""Generate initial Wi-Fi credentials without exposing them in compiler flags."""
import configparser
import uuid
from pathlib import Path


def generate(project):
    project = Path(project)
    source = project / "wifi.ini"
    ssid = password = ""
    if source.exists():
        config = configparser.ConfigParser(interpolation=None)
        try:
            config.read_string(source.read_text(encoding="utf-8-sig"))
            ssid = config.get("wifi", "ssid")
            password = config.get("wifi", "password")
        except (configparser.Error, UnicodeError):
            raise ValueError("wifi.ini must contain [wifi], ssid and password") from None
    if "\0" in ssid or "\0" in password:
        raise ValueError("Wi-Fi fields cannot contain NUL")
    if len(ssid.encode("utf-8")) > 32 or len(password.encode("utf-8")) > 63:
        raise ValueError("Wi-Fi limits: SSID 32 bytes; password 63 bytes")
    if password and not ssid:
        raise ValueError("Wi-Fi password requires a nonempty SSID")

    # Fixed-width octal escapes preserve UTF-8, quotes, backslashes and percent.
    def literal(value):
        return '"' + ''.join('\\%03o' % byte for byte in value.encode("utf-8")) + '"'

    content = '#pragma once\n#define WIFI_INI_CONFIGURED %d\n' % bool(ssid)
    content += '#define WIFI_INI_SSID %s\n' % literal(ssid)
    content += '#define WIFI_INI_PASSWORD %s\n' % literal(password)
    # Every build reapplies project credentials once; later USB changes survive reboot.
    content += '#define WIFI_BUILD_ID "%s"\n' % uuid.uuid4().hex
    target = project / "include" / "wifi_build_config.h"
    if not target.exists() or target.read_text(encoding="utf-8") != content:
        target.write_text(content, encoding="utf-8")


if "Import" in globals():
    Import("env")
    generate(env.subst("$PROJECT_DIR"))
