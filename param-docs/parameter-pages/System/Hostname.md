# Parameter `Hostname`
Default Value: `undefined`

!!! Warning
    This is an **Expert Parameter**! Only change it if you understand what it does!

Hostname for the device.
It gets automatically transferred to `/wlan.ini` on the SD-Card at the next startup.

On Ethernet boards (ESP32-S3-ETH) the hostname is used for the LAN connection (DHCP, mDNS) in the same way. Default there: `watermeter`.
