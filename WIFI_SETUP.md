# Runtime Wi-Fi setup (no physical button)

Production firmware now prefers Wi-Fi credentials stored in NVS. Build-time
`WIFI_SSID` / `WIFI_PASSWORD` remain optional fallback values; omit both for a
first-boot setup experience. Other backend/MQTT build settings are still required.

1. Flash the updated firmware once. Keep existing recording storage intact.
2. With no saved/fallback SSID, setup AP starts immediately. Otherwise it starts
   after 60 seconds without Wi-Fi association. Missing Internet, MQTT or NTP alone
   does not trigger setup while Wi-Fi remains connected.
3. Open Serial Monitor at 921600 baud. The `[SETUP]` line prints the setup SSID
   and device-specific random password. Record this password on a device label
   for future use without USB. It is persisted in NVS, not the user's Wi-Fi password.
4. Join `RespiCare-Setup-<device_id>` using a phone/laptop. Keep the connection
   even if the phone reports no Internet. Open **http://192.168.4.1/**.
   Captive-portal DNS is supported but automatic browser opening is OS-dependent.
5. Enter the exact 2.4 GHz SSID/password (blank password for an open network).
   Submit and wait up to 30 seconds. Wi-Fi channel changes may briefly interrupt
   the setup connection; reconnect to setup AP if necessary.
6. On success credentials are persisted as a single NVS record, setup closes
   after 10 seconds, and normal NTP/MQTT initialization continues. Invalid
   credentials are not persisted; the previous network is retried.

To change networks without a button, power on where the saved network is
unavailable and wait for setup. No web/MQTT command for opening setup is added.
Setup is separate from upload retry/recovery; it does not clear failed jobs or WAVs.

Hardware verification before deployment:
- Boot without credentials; join AP and submit correct credentials; check MQTT online.
- Reboot and confirm connection without setup or reflashing.
- Move outside saved network; verify AP appears after 60 seconds.
- Submit wrong password; verify AP remains accessible and saved credentials survive reboot.
- Keep Wi-Fi connected while blocking Internet; confirm no setup AP is opened.
- Check recording/upload flow and recovery still behave as before.

Do not expose the setup HTTP server through router port forwarding. The local
setup AP is password protected; provisioning is intended for a trusted operator.
