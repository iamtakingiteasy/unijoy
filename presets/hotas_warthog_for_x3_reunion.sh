#!/bin/sh


device_file="/sys/unijoy_ctl/merger"

echo unmerge 849162346299665 > $device_file
echo unmerge 849162346430737 > $device_file

echo merge 849162346299665 > $device_file
echo merge 849162346430737 > $device_file

# joystick
echo add_axis 849162346299665  0  0 > $device_file
echo add_axis 849162346299665  1  1 > $device_file
echo add_axis 849162346299665  2  2 > $device_file
echo add_axis 849162346299665  3  3 > $device_file

echo add_button 849162346299665  0  0 > $device_file
echo add_button 849162346299665  1  1 > $device_file
echo add_button 849162346299665  4  2 > $device_file
echo add_button 849162346299665 14  3 > $device_file
echo add_button 849162346299665 15  4 > $device_file
echo add_button 849162346299665 16  5 > $device_file
echo add_button 849162346299665 17  6 > $device_file
echo add_button 849162346299665 18  7 > $device_file
echo add_button 849162346299665 10  8 > $device_file
echo add_button 849162346299665 11  9 > $device_file
echo add_button 849162346299665 12 10 > $device_file
echo add_button 849162346299665 13 11 > $device_file
echo add_button 849162346299665  2 16 > $device_file

# throttle
echo add_axis 849162346430737  2  4 > $device_file
echo add_axis 849162346430737  5  5 > $device_file
echo add_axis 849162346430737  6  6 > $device_file

echo add_button 849162346430737 8  12 > $device_file
echo add_button 849162346430737 9  13 > $device_file
echo add_button 849162346430737 10 14 > $device_file
echo add_button 849162346430737 11 15 > $device_file

