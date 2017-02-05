# gatotray #
`gatotray` is a tiny CPU monitor displaying several stats graphically
(usage, temperature, frequency) in small space, and tight on resources.
Since version 3.0, It can also run as a screensaver.


## License ##

(c) 2011-2017 by gatopeich, licensed under a Creative Commons Attribution 3.0
Unported License: http://creativecommons.org/licenses/by/3.0/
Briefly: Use it however suits you better and just give me due credit.


## Features ##

* Goals: usability, lightness, looks, compatibility.

* Designed to run continuously and give a good idea of the CPU's status in a
  glimpse.

* Can run as a `xscreensaver` plugin, so you can see your CPUs at work from a
  prudential distance.

* Works in almost any Linux desktop, as long as it is Freedesktop compatible:
  XFCE, GNOME, GTK+, KDE, and more.

* It uses an innovative logarithmic time scale, providing an intuitive idea of
  CPU usage in reduced space. It looks good too. Colors vary with frequency and
  temperature.

* When available, temperature is represented in a thermometer, which blinks when
  too hot.

* Tooltip shows current stats in text form.

* On click, it opens a `top` window with detailed system usage. This command can
  be customized as a shortcut to your favorite monitoring tool.

* Preferences dialog allows customization of colors and options.

* Transparent background for better desktop/theme integration.


## Performance & Resource Consumption ##

gatotray aims to be a reliable and lightweight application, suitable for usage
in the most resource constrained systems. Here are some measures comparing
different versions running on:
   - A Core 2 Duo U9400 @ 800 MHz (max=1.4GHz) reporting 2793 bogomips.
   - GTK+ version 2.20.1(-0ubuntu2) (indirect dependencies vary with distro)

After 7 hours this is a pretty version of what we get with the command
"ps -o bsdtime,rss,etime,pid,command -C gatotray|sort -n".
```
CPU%  CPUtime    RSS  ElapsedTime  Version and options:
0.17     0:45   6984     07:11:57  gatotray v2.0 64 bits opaque 21x21
0.19     0:48   7464     07:11:14  gatotray v2.0 64 bits transparent 21x21
0.19     0:49   6176     07:10:13  gatotray v2.0 32 bits opaque 21x21
0.21     0:54   6560     07:09:48  gatotray v2.0 32 bits transparent 21x21
```
So gatotray v2.0 eats roughly less than 6 bogomips in its several configurations
, transparency costing ~10% additional CPU, and running the 32 bit version
saving a bit under 1kB RSS memory.


### watchRSS ###
Script "watchRSS" used to track memory and CPU usage in a simple way follows:

```
#!sh

#!/bin/bash
$@ &
pid=$!
while watch="`ps -o bsdtime $pid` `grep RSS /proc/$pid/status`"; do
	[ "$watch" != "$old" ] && echo `ps -o etime $pid` CPU$watch
	old="$watch"
	sleep .1
done
```