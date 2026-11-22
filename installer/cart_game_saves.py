"""Save files for games added to a Magic Trick cartridge."""
import re
from pathlib import PurePosixPath

import cartridge
import library


SAVE_FOLDER = 'CHROMAGIC/CART'
# Include publication recovery files and the current and legacy pre-state
# saves, which could otherwise bring old progress back after a reset.
SAVE_SUFFIX = re.compile(
    r'(?:\.sav(?:\.(?:before-load|state)-[0-9]{4}\.sav)?|\.rtc|'
    r'\.state[23]?|\.state-[0-9]{4}\.sav)(?:\.(?:old|tmp))?',
    re.IGNORECASE | re.ASCII,
)


class SaveClearError(ValueError):
    """A clear failed. The caller's original snapshot remains the backup."""

    def __init__(self, cause, unrecovered):
        self.unrecovered = tuple(unrecovered)
        if self.unrecovered:
            result = ('These files could not be confirmed restored. '
                      'Keep the backup for manual recovery: ' + ', '.join(self.unrecovered))
        else:
            result = 'Any deleted save files were restored from the backup.'
        super().__init__('Could not clear the game’s saves: %s. %s' % (cause, result))


def game_save_key(game_name):
    """The case-insensitive filename stem shared by a game's firmware save paths."""
    cartridge.check_name(game_name)
    return PurePosixPath(game_name).stem.casefold()


def _fresh_list(target, rel):
    target.forget(rel)
    return target.list(rel)


def _save_folder(target):
    target.require_physical_sd()
    parent = ''
    for part in SAVE_FOLDER.split('/'):
        matches = [entry for entry in _fresh_list(target, parent) if entry[0].casefold() == part.casefold()]
        if not matches:
            return None
        if len(matches) != 1 or not matches[0][1]:
            raise ValueError('Ambiguous save folder on the microSD: ' + SAVE_FOLDER)
        parent = parent + '/' + matches[0][0] if parent else matches[0][0]
    return parent


def _belongs_to(stem, name):
    return name.startswith(stem) and SAVE_SUFFIX.fullmatch(name[len(stem):]) is not None


def _save_paths(target, stem, other_stems=()):
    folder = _save_folder(target)
    if folder is None:
        return {}
    paths = {}
    for name, is_dir, size in _fresh_list(target, folder):
        key = name.casefold()
        if not _belongs_to(stem, key):
            continue
        if is_dir or any(_belongs_to(other, key) for other in other_stems) or key in paths:
            raise ValueError('Ambiguous save file on the microSD: ' + name)
        paths[key] = (folder + '/' + name, size)
    return dict(sorted(paths.values()))


def _snapshot(target, stem, other_stems=()):
    files = {}
    for rel, size in _save_paths(target, stem, other_stems).items():
        data = target.read(rel)
        if len(data) != size:
            raise IOError('The save changed while it was being read: ' + rel)
        files[rel] = data
    return files


def snapshot_game_saves(target, game_name, *, game_names=()) -> dict[str, bytes]:
    """Read a game's exact physical microSD paths and contents, retaining file spelling.

    Pass the other cartridge entries to reject any names the firmware maps to
    the same filename stem. Missing storage is an error, distinct from no saves.
    """
    stem = game_save_key(game_name)
    others = {game_save_key(name) for name in game_names if name != game_name}
    if stem in others:
        raise ValueError('More than one cartridge game uses the save name ' + stem + '.')
    return _snapshot(target, stem, others)


def _expected_stem(expected):
    candidates = None
    for rel in expected:
        name = rel.rsplit('/', 1)[-1].casefold()
        stems = {name[:i] for i, char in enumerate(name) if char == '.' and i and _belongs_to(name[:i], name)}
        candidates = stems if candidates is None else candidates & stems
    if len(candidates) != 1:
        raise ValueError('The backup does not identify exactly one game’s saves.')
    return candidates.pop()


def _recover_deleted(target, paths, expected, stem):
    if not paths or isinstance(target, library.UsbTarget):
        # A failed PCSD request ends the session. USB recovery needs a fresh
        # connection, so report the affected paths and preserve the backup.
        return paths
    try:
        present = _save_paths(target, stem)
    except Exception:
        return paths
    failed = []
    for rel in paths:
        data = expected[rel]
        try:
            if rel not in present:
                target.write(rel, data)
            if target.read(rel) != data:
                failed.append(rel)
        except Exception:
            failed.append(rel)
    return failed


def clear_game_saves(target, expected: dict[str, bytes], *, game_name=None):
    """Clear an unchanged snapshot. The caller must first save it durably."""
    if not expected and game_name is None:
        target.require_physical_sd()
        return
    stem = game_save_key(game_name) if game_name is not None else _expected_stem(expected)
    if _snapshot(target, stem) != expected:
        raise ValueError('The game’s save files changed after backup. Read them again before clearing.')
    if not expected:
        return
    attempted = []
    try:
        for rel, data in expected.items():
            if target.read(rel) != data:
                raise ValueError('The save changed after backup: ' + rel)
            attempted.append(rel)
            target.delete_save(rel)
        if _save_paths(target, stem):
            raise ValueError('A save appeared while the files were being cleared.')
    except Exception as exc:
        raise SaveClearError(exc, _recover_deleted(target, attempted, expected, stem)) from exc
