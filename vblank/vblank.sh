program=vblank
meson compile -C builddir/ \
  && scp builddir/$program root@$DEVICE_IP:/root/$program \
  && ssh -t root@$DEVICE_IP /root/$program