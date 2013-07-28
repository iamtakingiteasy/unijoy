unijoy 
======

A sample linux kernel module creating a new jsN device, which acts as union
of M other js-devices. 

Purpose 
=======

This module is intented to create a virtual joystick device which would act as
union of 0..n other joystick devices, proxifying and routing their events as if
they were emmited by virtual device.

The actual use-case of such curious functionality is an example of 
Thrustmaster HOTAS Warthog joystick, which flightstick and rudder are two separate
devices, plugged to a computer separetelly and independently.

This module is needed to use such devices in games which simply not aware about more
than one joystick at same time, like X3-Reunion/X3-Terran Conflict/X3-Albion Prelude.

Capabilities
============
This module can:
* Seemlessly proxify an event from any axis or button of real device as an event of
  it's own.
* Map single axis or button to more than a one destination axis or a button, making
  them trigger/update simultaneously.
* Have any order and number (within ABS_CNT and KEY_MAX limits) of axis and buttons    

Current flaws
=============
* it uses root of sysfs filesystem which is not good and must be changed in the future.
  As result, DO NOT hard-code string /sys/unijoy_ctl/merge in your wrappers.
* it uses thread in order to escape deadlocks when new event emmited from event handler
  (proxifying events) and when virtual device is updated from .connect and .disconnect
  handlers. This thread is a hack and should be removed once i became of alternative
  solution.
* Code is pretty raw, it suffered only three global refactorings and might have some
  un-obvious memleaks and case your whole system crashes. Be careful using it.
  If you experienced a crash or found a memleak, please do report abot them, it is a BUG.


Building
========

Simply, invoke `make` in root directory of this project. You will receive the similar output:

    user@noteshi ~/soft/mine/unijoy $ make
    make -C /lib/modules/3.9.2-tuxonice/build M=/home/user/soft/mine/unijoy modules
    make[1]: Entering directory `/usr/src/linux-3.9.2-tuxonice'
      CC [M]  /home/user/soft/mine/unijoy/unijoy.o
      Building modules, stage 2.
      MODPOST 1 modules
      CC      /home/user/soft/mine/unijoy/unijoy.mod.o
      LD [M]  /home/user/soft/mine/unijoy/unijoy.ko
    make[1]: Leaving directory `/usr/src/linux-3.9.2-tuxonice'
    

Using
=====

Inserting
---------

Invoke `insmod ./unijoy.ko` in directory with unijoy.ko file which should appear as result
of (previous) building step. If it doesn't, please report a BUG.

Observing control file
----------------------

Control file is located in `/sys/unijoy_ctl/merger` path. AGAIN! It is temprory and should be
relocated.

Try reading it's contents:

    user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger
    849162346430737       ONLINE   7  32 Thrustmaster Throttle - HOTAS Warthog
    849162346299665       ONLINE   4  19 Thustmaster Joystick - HOTAS Warthog
    855256926716177       ONLINE  37  57 A4TECH USB Device
    Current mappings:

The first column is ID of device. You will use it every time
you wish to refer to a specific device. It is based on busid,
vendor, device and version parameters of device, so it should be
persistent across different machines.

The second column is the status. Possible values are:

* ONLINE -- device is known to a system and working normally
* MERGED -- device is participating a merge
* DISCONNECTED -- device is known to a system, but currently plugged out

Missing devices are both unplugged and wasn't in a merge upon unplugging.

The third column is total axes of deivce

The fourth colum is total buttons of device

The rest of line is human-readable device name.

Adding devices to a merge
-------------------------

Syntax: `merge <ID>`:

    user@noteshi ~/soft/mine/unijoy $ echo merge 849162346299665 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo merge 849162346430737 > /sys/unijoy_ctl/merger

Now lets look at device file once again:

    user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger 
    849162346430737       MERGED   7  32 Thrustmaster Throttle - HOTAS Warthog
    849162346299665       MERGED   4  19 Thustmaster Joystick - HOTAS Warthog
    855256926716177       ONLINE  37  57 A4TECH USB Device
    Current mappings:


Adding buttons to the merge device
----------------------------------

Syntax: `add_button <ID> <source button #> [dest button #]`

(dest button # is optional, last unused would be picked if missing)

    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665  0  0 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665  1  1 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665  4  2 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 14  3 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 15  4 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 16  5 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 17  6 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 18  7 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 10  8 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 11  9 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 12 10 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 13 11 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 8  12 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 9  13 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 10 14 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 11 15 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665  2 16 > /sys/unijoy_ctl/merger

And observe the result:

    user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger 
    849162346430737       MERGED   7  32 Thrustmaster Throttle - HOTAS Warthog
    849162346299665       MERGED   4  19 Thustmaster Joystick - HOTAS Warthog
    855256926716177       ONLINE  37  57 A4TECH USB Device
    Current mappings:
    BTN #  0 ->   0 of 849162346299665  ONLINE
    BTN #  1 ->   1 of 849162346299665  ONLINE
    BTN #  4 ->   2 of 849162346299665  ONLINE
    BTN # 14 ->   3 of 849162346299665  ONLINE
    BTN # 15 ->   4 of 849162346299665  ONLINE
    BTN # 16 ->   5 of 849162346299665  ONLINE
    BTN # 17 ->   6 of 849162346299665  ONLINE
    BTN # 18 ->   7 of 849162346299665  ONLINE
    BTN # 10 ->   8 of 849162346299665  ONLINE
    BTN # 11 ->   9 of 849162346299665  ONLINE
    BTN # 12 ->  10 of 849162346299665  ONLINE
    BTN # 13 ->  11 of 849162346299665  ONLINE
    BTN #  8 ->  12 of 849162346430737  ONLINE
    BTN #  9 ->  13 of 849162346430737  ONLINE
    BTN # 10 ->  14 of 849162346430737  ONLINE
    BTN # 11 ->  15 of 849162346430737  ONLINE
    BTN #  2 ->  16 of 849162346299665  ONLINE

Removing buttons from the merge device
--------------------------------------

Syntax: `del_button <dest button #>`

    user@noteshi ~/soft/mine/unijoy $ echo del_button 11 > /sys/unijoy_ctl/merger 
    
And result:

    user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger 
    849162346430737       MERGED   7  32 Thrustmaster Throttle - HOTAS Warthog
    849162346299665       MERGED   4  19 Thustmaster Joystick - HOTAS Warthog
    855256926716177       ONLINE  37  57 A4TECH USB Device
    Current mappings:
    BTN #  0 ->   0 of 849162346299665  ONLINE
    BTN #  1 ->   1 of 849162346299665  ONLINE
    BTN #  4 ->   2 of 849162346299665  ONLINE
    BTN # 14 ->   3 of 849162346299665  ONLINE
    BTN # 15 ->   4 of 849162346299665  ONLINE
    BTN # 16 ->   5 of 849162346299665  ONLINE
    BTN # 17 ->   6 of 849162346299665  ONLINE
    BTN # 18 ->   7 of 849162346299665  ONLINE
    BTN # 10 ->   8 of 849162346299665  ONLINE
    BTN # 11 ->   9 of 849162346299665  ONLINE
    BTN # 12 ->  10 of 849162346299665  ONLINE
    BTN #  8 ->  12 of 849162346430737  ONLINE
    BTN #  9 ->  13 of 849162346430737  ONLINE
    BTN # 10 ->  14 of 849162346430737  ONLINE
    BTN # 11 ->  15 of 849162346430737  ONLINE
    BTN #  2 ->  16 of 849162346299665  ONLINE

Adding axes to the merge device
-------------------------------

Syntax: `add_axis <ID> <source axis #> [dest axis #]`

(dest axis # is optional, last unused would be picked if missing)

    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346299665  0  0 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346299665  1  1 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346299665  2  2 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346299665  3  3 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346430737  2  4 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346430737  5  5 > /sys/unijoy_ctl/merger
    user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346430737  6  6 > /sys/unijoy_ctl/merger
    
And result:

    user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger 
    849162346430737       MERGED   7  32 Thrustmaster Throttle - HOTAS Warthog
    849162346299665       MERGED   4  19 Thustmaster Joystick - HOTAS Warthog
    855256926716177       ONLINE  37  57 A4TECH USB Device
    Current mappings:
    BTN #  0 ->   0 of 849162346299665  ONLINE
    BTN #  1 ->   1 of 849162346299665  ONLINE
    BTN #  4 ->   2 of 849162346299665  ONLINE
    BTN # 14 ->   3 of 849162346299665  ONLINE
    BTN # 15 ->   4 of 849162346299665  ONLINE
    BTN # 16 ->   5 of 849162346299665  ONLINE
    BTN # 17 ->   6 of 849162346299665  ONLINE
    BTN # 18 ->   7 of 849162346299665  ONLINE
    BTN # 10 ->   8 of 849162346299665  ONLINE
    BTN # 11 ->   9 of 849162346299665  ONLINE
    BTN # 12 ->  10 of 849162346299665  ONLINE
    BTN #  8 ->  12 of 849162346430737  ONLINE
    BTN #  9 ->  13 of 849162346430737  ONLINE
    BTN # 10 ->  14 of 849162346430737  ONLINE
    BTN # 11 ->  15 of 849162346430737  ONLINE
    BTN #  2 ->  16 of 849162346299665  ONLINE
    AXS #  0 ->   0 of 849162346299665  ONLINE
    AXS #  1 ->   1 of 849162346299665  ONLINE
    AXS #  2 ->   2 of 849162346299665  ONLINE
    AXS #  3 ->   3 of 849162346299665  ONLINE
    AXS #  2 ->   4 of 849162346430737  ONLINE
    AXS #  5 ->   5 of 849162346430737  ONLINE
    AXS #  6 ->   6 of 849162346430737  ONLINE


Removing axes from the merge device
-----------------------------------

Syntax: `del_axis <dest axis #>`

    user@noteshi ~/soft/mine/unijoy $ echo del_axis 3 > /sys/unijoy_ctl/merger
    
And result:
    
    user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger 
    849162346430737       MERGED   7  32 Thrustmaster Throttle - HOTAS Warthog
    849162346299665       MERGED   4  19 Thustmaster Joystick - HOTAS Warthog
    855256926716177       ONLINE  37  57 A4TECH USB Device
    Current mappings:
    BTN #  0 ->   0 of 849162346299665  ONLINE
    BTN #  1 ->   1 of 849162346299665  ONLINE
    BTN #  4 ->   2 of 849162346299665  ONLINE
    BTN # 14 ->   3 of 849162346299665  ONLINE
    BTN # 15 ->   4 of 849162346299665  ONLINE
    BTN # 16 ->   5 of 849162346299665  ONLINE
    BTN # 17 ->   6 of 849162346299665  ONLINE
    BTN # 18 ->   7 of 849162346299665  ONLINE
    BTN # 10 ->   8 of 849162346299665  ONLINE
    BTN # 11 ->   9 of 849162346299665  ONLINE
    BTN # 12 ->  10 of 849162346299665  ONLINE
    BTN #  8 ->  12 of 849162346430737  ONLINE
    BTN #  9 ->  13 of 849162346430737  ONLINE
    BTN # 10 ->  14 of 849162346430737  ONLINE
    BTN # 11 ->  15 of 849162346430737  ONLINE
    BTN #  2 ->  16 of 849162346299665  ONLINE
    AXS #  0 ->   0 of 849162346299665  ONLINE
    AXS #  1 ->   1 of 849162346299665  ONLINE
    AXS #  2 ->   2 of 849162346299665  ONLINE
    AXS #  2 ->   4 of 849162346430737  ONLINE
    AXS #  5 ->   5 of 849162346430737  ONLINE
    AXS #  6 ->   6 of 849162346430737  ONLINE


Testing setup
-------------

I recommend jstest utility from linuxconsoletools package (sometimes provided under `joystick` package).

    user@noteshi ~/soft/mine/unijoy $ jstest /dev/input/js3
