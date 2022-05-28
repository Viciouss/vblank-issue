# general
project('vblank-fail', 'c')

### dependencies
drm = dependency('libdrm')

### executables ###
src_fb = ['main.c']
executable('vblank', src_fb,  dependencies: [drm])