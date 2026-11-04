name: build
on:
  push:
  workflow_dispatch:

jobs:
  sysmodule:
    runs-on: ubuntu-latest
    container: devkitpro/devkita64
    steps:
      - uses: actions/checkout@v4
      - name: Build sysmodule
        run: make
        working-directory: sysmodule
      - uses: actions/upload-artifact@v4
        with:
          name: usbaudio-sysmodule
          path: sysmodule/out/
          include-hidden-files: true

  overlay:
    runs-on: ubuntu-latest
    container: devkitpro/devkita64
    steps:
      - uses: actions/checkout@v4
      - name: Get libtesla
        run: git clone --depth 1 https://github.com/WerWolv/libtesla libtesla
        working-directory: overlay
      - name: Build overlay
        run: make
        working-directory: overlay
      - uses: actions/upload-artifact@v4
        with:
          name: usbaudio-overlay
          path: overlay/out/
          include-hidden-files: true