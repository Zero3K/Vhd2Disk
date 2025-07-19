# Vhd2Disk + Disk2VHD

What Vhd2disk does?

It's a bidirectional V2P/P2V free tool that can:
1. Convert VHD files to physical drives (VHD → Disk)
2. Convert physical drives to VHD files (Disk → VHD)

The VHD to Disk functionality is designed to work only with "dynamic" VHD like Disk2vhd's output. Vhd2disk was tested successfully on win7 and win2K8: Disk2vhd -> vhd used for virtualisation -> Vhd2disk.

The new Disk to VHD functionality creates dynamic VHD files from physical drives, essentially providing the reverse operation of Microsoft's Disk2VHD tool.

This tool now works as a complete mirror and complement to the Sysinternal's Disk2Vhd tool (https://technet.microsoft.com/en-us/sysinternals/ee656415).

## New Features in v0.3:
- Bidirectional conversion support
- Disk to VHD conversion capability
- Updated UI with operation mode selection
- Enhanced dialog with proper file save options

Be caution using this tool since it will overwrite data on target drives or create large VHD files.
If you play with it, I can't be responsive about any data lost.

http://forum.sysinternals.com/vhd2disk_topic27311_page1.html

