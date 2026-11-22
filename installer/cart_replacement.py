"""Validation and staging for replacing a ROM without renaming its cart entry."""
import re
from pathlib import Path

import cartridge


def replace_game(game, filename, data):
    if cartridge.SYSTEMS.get(Path(filename).suffix.lower()) != game.system:
        raise cartridge.CartError('Choose a ROM for the same system as the selected game.')
    if not data:
        raise cartridge.CartError('The replacement ROM is empty.')
    if game.system == cartridge.SYSTEMS['.gb'] and not cartridge.header_ok(data):
        raise cartridge.CartError('The replacement Game Boy ROM has an invalid header.')
    if data == game.data:
        raise cartridge.CartError('The selected ROM is already on this row.')
    return cartridge.Game(game.name, data, game.art)


def replace_own_game(info, old_image, filename, data, save_size):
    if Path(filename).suffix.lower() not in ('.gb', '.gbc'):
        raise cartridge.CartError('Choose a Game Boy or Game Boy Color ROM for the first game.')
    image, title, notes = cartridge.own_game(data, save_size)
    if title != info.own_title:
        raise cartridge.CartError('This ROM is named %s in its header. Choose another build of %s to keep the '
                                  'name, or use OWN GAME to change which game goes first.' % (title, info.own_title))
    if image == old_image[:info.own_size]:
        raise cartridge.CartError('The selected ROM is already on this row.')
    return {'name': filename, 'image': image, 'title': title, 'notes': notes}


def changed_games(original, staged):
    previous = {game.name: game.data for game in original}
    return [game.name for game in staged if game.name in previous and game.data != previous[game.name]]


def own_save_entry(image):
    """Match CartStore's name for the first game's temporary save on the SD card."""
    color = bool(image[0x143] & 0x80)
    raw = image[0x134:0x143 if color else 0x144].split(b'\0', 1)[0].decode('latin-1')
    title = re.sub(r"[^A-Za-z0-9!&.'-]", ' ', raw).rstrip() or 'GAME BOY'
    return f"{title} (CARTRIDGE).{'gbc' if color else 'gb'}"
