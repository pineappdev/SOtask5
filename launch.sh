#!/usr/bin/bash
sudo qemu-system-x86_64 -curses -drive file=minix.img -drive file=extra.img,format=raw,index=1,media=disk -enable-kvm -localtime -net user,hostfwd=tcp::10022-:22 -net nic,model=virtio -m 1024M
