unijoy 
======

A sample linux kernel module creating a new jsN device, which acts as union
of M other js-devices. 

Building
========

Simply, `make`


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

`insmod ./unijoy.ko`

Observing control file
----------------------

Control file is located in `/sys/unijoy_ctl/merger` path.

Try reading it's contents:

        user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger 
        849162346430737	ONLINE	7	32	Thrustmaster Throttle - HOTAS Warthog
        849162346299665	ONLINE	4	19	Thustmaster Joystick - HOTAS Warthog
        855256926716177	ONLINE	37	57	A4TECH USB Device
				Operating as /dev/input/js3

The first column is ID of device. You will use it every time
you wish to refer to a specific device. It is pretty unique.

The second column is the status. Possible values are:

* ONLINE -- device is known to a system and working normally
* MERGED -- device is participating a merge
* WASMERGED -- device is known to a system, but currently plugged out

Missing devices are both unplugged and wasn't in a merge upon unplugging.

The third column is total axes of deivce

The fourth colum is total buttons of device

The rest of string is human-readable device name.

The last line is a human-readable string indicating under which name merge a
device is operating.

Adding devices to a merge
-------------------------

Syntax: `merge <ID>`:

        user@noteshi ~/soft/mine/unijoy $ echo merge 849162346299665 > /sys/unijoy_ctl/merger
		    user@noteshi ~/soft/mine/unijoy $ echo merge 849162346430737 > /sys/unijoy_ctl/merger

Now lets look at device file once again:

        user@noteshi ~/soft/mine/unijoy $ cat /sys/unijoy_ctl/merger
				849162346430737	MERGED	7	32	Thrustmaster Throttle - HOTAS Warthog
        849162346299665	MERGED	4	19	Thustmaster Joystick - HOTAS Warthog
        855256926716177	ONLINE	37	57	A4TECH USB Device
				Operating as /dev/input/js3

Adding buttons to the merge device
----------------------------------

Syntax: `add_button <ID> <source button #> [dest button #]`

(dest button # is optional, last unused would be picked if missing)

        user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 0 0 > /sys/unijoy_ctl/merger
        user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 1 1 > /sys/unijoy_ctl/merger
        user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346299665 2 5 > /sys/unijoy_ctl/merger
        user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 0 2 > /sys/unijoy_ctl/merger
				user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 1 3 > /sys/unijoy_ctl/merger
				user@noteshi ~/soft/mine/unijoy $ echo add_button 849162346430737 2 4 > /sys/unijoy_ctl/merger


Removing buttons from the merge device
--------------------------------------

Syntax: `del_button <dest button #>`

        user@noteshi ~/soft/mine/unijoy $ echo del_button 2 > /sys/unijoy_ctl/merger


Adding axes to the merge device
-------------------------------

Syntax: `add_axis <ID> <source axis #> [dest axis #]`

(dest axis # is optional, last unused would be picked if missing)

				user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346430737 0 0 > /sys/unijoy_ctl/merger
				user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346430737 1 1 > /sys/unijoy_ctl/merger
				user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346299665 0 2 > /sys/unijoy_ctl/merger
				user@noteshi ~/soft/mine/unijoy $ echo add_axis 849162346299665 1 3 > /sys/unijoy_ctl/merger

Removing axes from the merge device
-----------------------------------

Syntax: `del_axis <dest axis #>`

        user@noteshi ~/soft/mine/unijoy $ echo del_axis 2 > /sys/unijoy_ctl/merger

Testing setup
-------------

I recommend jstest utility from linuxconsoletools package (sometimes provided under `joystick` package).

        user@noteshi ~/soft/mine/unijoy $ jstest /dev/input/js3
