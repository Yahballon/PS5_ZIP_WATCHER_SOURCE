# PS5 ZIP Watcher 

A native PS5 ELF payload that watches `/mnt/usb0/homebrew/` for ZIP archives and installs homebrew updates. Existing files are overwritten; unrelated files are preserved. A ZIP is removed only after a successful installation.

This project was prepared with AI assistance. Local cross-compilation and host tests passed; execution on a real PS5 has not been verified.

## Download and run

Download `PS5_ZIP_WATCHER_PS5.zip` from the repository's Releases page and extract `ps5_zip_watcher.elf`.

1. Prepare a writable USB directory at `/mnt/usb0/homebrew/`.
2. Start your existing PS5 jailbreak and compatible ELF loader.
3. Load `ps5_zip_watcher.elf` using your loader.
4. Copy homebrew ZIP archives into the watched directory.

The payload checks every few seconds and shows notifications on startup and successful installation. It runs while its process remains alive and must be loaded again after reboot. USB write permissions depend on your loader and environment.

For uploads, use a temporary `.part` filename and rename it to `.zip` only when copying is complete. A stable file size alone cannot guarantee that an upload has finished.

## ZIP layout

For predictable updates, package files inside the application's top-level folder:

```text
PS5NES-update.zip
  PS5NES/
    eboot.bin
    sce_sys/icon0.png
```

This updates `/mnt/usb0/homebrew/PS5NES/`. Other files in that folder remain untouched.

Archives with root-level files, or mixed root files and folders, are installed into a directory named after the ZIP without its `.zip` extension. Use the same ZIP filename for later updates of these packages.

## Supported archives and limitations

- Classic ZIP with stored or deflated entries (methods 0 and 8).
- ZIP64, encryption, ZIP symlinks, absolute paths and `..` traversal are rejected.
- Invalid archives and archives that fail installation are retained.
- CRC checks are performed before staged files are installed.
- Staging needs temporary space approximately equal to the uncompressed contents.
- Installation is not fully transactional: a merge failure may leave some files updated. Keep backups.
- Real PS5 compatibility, long-lived payload execution and filesystem behavior need proper testing on your firmware and loader.

No BIOS, game images, firmware offsets or kernel exploits are included. This is unofficial homebrew and is not affiliated with Sony Interactive Entertainment.

## Build on Linux or WSL

Install Clang/LLVM, LLD, Make, curl and the public [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). Extract the SDK, then run:

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
bash fetch_miniz.sh
make
```

Output: `ps5_zip_watcher.elf`. The helper downloads pinned miniz 2.2.0 sources and generates the export header required for this static build. Generated dependencies and binaries are excluded from Git.

### Host tests

With a C compiler, Python 3 and zlib development headers installed:

```bash
make host
python3 tests/test_watcher.py
```

Tests cover overwrite updates, preservation of unrelated files, flat archives, invalid ZIPs, traversal rejection and CRC protection. They do not validate PS5 runtime behavior.

## GitHub builds and releases

Upload the source tree, including `.github/workflows/build.yml`. Pushes to `main` or `master`, and manual workflow runs, build and test the project and upload a release ZIP as an Actions artifact.

To publish a release, push a version tag such as `v1.0.0`. The workflow attaches the payload ZIP, standalone ELF and SHA-256 checksums to the GitHub Release. GitHub also provides source archives for the tag. Release metadata includes the AI-assisted disclosure.

The workflow obtains the latest public SDK release; miniz is pinned to 2.2.0. SDK updates may affect later builds. Record the SDK version when reproducing a release.

## Credits

Uses [miniz 2.2.0](https://github.com/richgel999/miniz/tree/2.2.0) by Rich Geldreich and contributors under the MIT license. Its upstream files retain their license notices. Built with the community `ps5-payload-dev/sdk`.
