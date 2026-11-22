"""Publish immutable cartridge and SD save snapshots for the desktop editor."""
import datetime
import os
import re
import tempfile
import uuid
from dataclasses import dataclass
from pathlib import Path

import cartridge


@dataclass(frozen=True)
class SaveSnapshot:
    path: Path
    source: str
    modified_ns: int
    size: int


_SNAPSHOT_PREFIXES = ('Cartridge snapshot ', 'Game snapshot ')


def _write_new(path, data):
    # Explicit creation prevents a save export from replacing an existing file.
    with path.open('xb') as handle:
        try:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        except BaseException:
            handle.close()
            path.unlink()
            raise


def _publish(directory, prefix, files, stamp):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    # Rename only after all files are closed and synced. An interrupted staged
    # snapshot is invisible to history and leaves every older version intact.
    with tempfile.TemporaryDirectory(prefix='.pending-', dir=directory) as pending:
        stage = Path(pending)
        for name, data in files.items():
            _write_new(stage / name, data)
        published = directory / (prefix + stamp + '-' + uuid.uuid4().hex[:16])
        while published.exists():
            published = directory / (prefix + stamp + '-' + uuid.uuid4().hex[:16])
        stage.rename(published)
    return published


def create_snapshot(directory, image, save=b'', *, title=None):
    """Archive the full ROM and associated SRAM, returning the restorable .bin."""
    stem = re.sub(r'[^0-9A-Za-z.-]+', '-', title or 'cart').strip('.-') or 'cart'
    stamp = datetime.datetime.now().strftime('%Y-%m-%d_%H%M%S-%f')
    name = '%s %s %08X' % (stem, stamp, cartridge.crc32(image))
    files = {name + '.bin': image}
    if save:
        files[name + '.sav'] = save
    folder = _publish(directory, _SNAPSHOT_PREFIXES[0], files, stamp)
    return folder / (name + '.bin')


def create_game_snapshot(directory, game_name, files):
    """Archive SD save/RTC files with their exact basenames and return the folder."""
    if not files:
        return None
    names = set()
    for name in files:
        if not name or name in ('.', '..') or any(ch in name for ch in '/\\:\0'):
            raise ValueError('Expected an SD save basename for %s, got %r.' % (game_name, name))
        if name.casefold() in names:
            raise ValueError('Duplicate SD save basenames for %s.' % game_name)
        names.add(name.casefold())
    stamp = datetime.datetime.now().strftime('%Y-%m-%d_%H%M%S-%f')
    return _publish(directory, _SNAPSHOT_PREFIXES[1], files, stamp)


def list_save_snapshots(directory):
    """Return completed cartridge and SD save files, including legacy flat pairs."""
    directory = Path(directory)
    if not directory.is_dir():
        return []
    rows = []
    folders = [(directory, 'Cartridge')]
    for child in directory.iterdir():
        if child.is_symlink() or not child.is_dir():
            continue
        if child.name.startswith(_SNAPSHOT_PREFIXES[0]):
            folders.append((child, 'Cartridge'))
        elif child.name.startswith(_SNAPSHOT_PREFIXES[1]):
            folders.append((child, 'SD card'))
    for folder, source in folders:
        for save in folder.iterdir():
            if save.suffix.lower() != '.sav' or save.is_symlink() or not save.is_file():
                continue
            if source == 'Cartridge':
                image = save.with_suffix('.bin')
                if image.is_symlink() or not image.is_file():
                    continue
            try:
                info = save.stat()
            except FileNotFoundError:
                continue
            rows.append(SaveSnapshot(save, source, info.st_mtime_ns, info.st_size))
    return sorted(rows, key=lambda entry: (entry.modified_ns, str(entry.path)), reverse=True)


def export_save(snapshot, destination):
    """Export just the chosen save, preserving any existing destination file."""
    _write_new(Path(destination), snapshot.path.read_bytes())
