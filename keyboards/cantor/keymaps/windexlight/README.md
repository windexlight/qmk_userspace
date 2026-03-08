Big gotcha if swapping which half of the keyboard is connected to USB:

Am using `USB_WAIT_FOR_ENUMERATION = yes` in rules.mk, because it seems to fix issues with the keyboard sometimes not being detected on a fresh boot.

If the firmware running on the secondary side is built with this enabled, it will not communicate to the primary side, presumably because it just
gets stuck waiting for USB and never starts up properly. So, when swapping the cable, you MUST build a firmware version without that enabled and flash
it to the secondary side.
