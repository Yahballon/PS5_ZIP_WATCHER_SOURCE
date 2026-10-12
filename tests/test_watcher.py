#!/usr/bin/env python3
"""Integration tests for extraction, overwrite, preservation, and failure behavior."""
import pathlib
import subprocess
import tempfile
import zipfile

BIN = pathlib.Path(__file__).resolve().parents[1] / 'ps5_zip_watcher_host'


def run(root):
    result = subprocess.run([str(BIN), '--once', str(root)], capture_output=True,
                            text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return result.stdout


def makezip(path, members, compression=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(path, 'w', compression=compression) as z:
        for name, content in members.items():
            z.writestr(name, content)


def test_updates(root):
    app = root / 'PS5NES'
    app.mkdir()
    (app / 'eboot.bin').write_bytes(b'OLD')
    (app / 'settings.json').write_text('{"keep": true}')
    z = root / 'PS5NES-update.zip'
    makezip(z, {'PS5NES/eboot.bin': b'NEW_ELF',
                'PS5NES/sce_sys/icon0.png': b'NEW_ICON'})
    output = run(root)
    assert '[OK]' in output, output
    assert not z.exists()
    assert (app / 'eboot.bin').read_bytes() == b'NEW_ELF'
    assert (app / 'sce_sys/icon0.png').read_bytes() == b'NEW_ICON'
    assert (app / 'settings.json').read_text() == '{"keep": true}'


def test_flat(root):
    z = root / 'Cryptotracker.zip'
    makezip(z, {'eboot.bin': b'v1', 'sce_sys/icon0.png': b'ico'}, zipfile.ZIP_STORED)
    assert '[OK]' in run(root)
    assert not z.exists()
    assert (root / 'Cryptotracker/eboot.bin').read_bytes() == b'v1'
    z = root / 'Cryptotracker.zip'
    makezip(z, {'eboot.bin': b'v2'})
    assert '[OK]' in run(root)
    assert (root / 'Cryptotracker/eboot.bin').read_bytes() == b'v2'
    assert (root / 'Cryptotracker/sce_sys/icon0.png').read_bytes() == b'ico'


def test_corrupted(root):
    z = root / 'bad.zip'
    z.write_bytes(b'NOT A ZIP')
    assert '[ERROR]' in run(root)
    assert z.exists()


def test_traversal(root):
    for i, filename in enumerate(('../evil', 'X/../../evil', '/evil', 'C:\\evil')):
        z = root / f'malicious-{i}.zip'
        makezip(z, {filename: b'EVIL'})
        assert '[ERROR]' in run(root)
        assert z.exists()
    assert not (root.parent / 'evil').exists()


def test_crc_failure(root):
    z = root / 'PS5NES-crc.zip'
    makezip(z, {'PS5NES/eboot.bin': b'A' * 15000})
    data = bytearray(z.read_bytes())
    location = data.index(b'PS5NES/eboot.bin') + len(b'PS5NES/eboot.bin')
    data[location] ^= 0xff
    z.write_bytes(data)
    assert '[ERROR]' in run(root)
    assert z.exists()
    assert (root / 'PS5NES/eboot.bin').read_bytes() == b'NEW_ELF'


if __name__ == '__main__':
    assert BIN.exists(), 'Build host executable first: make host'
    with tempfile.TemporaryDirectory() as d:
        r = pathlib.Path(d) / 'homebrew'
        r.mkdir()
        test_updates(r)
        test_flat(r)
        test_corrupted(r)
        test_traversal(r)
        test_crc_failure(r)
    print('PASS: overwrite updates, preserve unrelated files, flat ZIP install, invalid ZIP, traversal, CRC protection')
