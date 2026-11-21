#!/usr/bin/env python3
"""Magic Trick carts: Game Gear, Lynx and Game Boy games on a reflashable Game Boy cart.

  mtcart.py build OUT.gb GAME [GAME...]   dedicated cart image: a small Game Boy
                                          program (a title card), the
                                          directory at 0x4000, then the games
  mtcart.py append BASE.gb OUT.gb GAME... an existing Game Boy ROM with games
                                          added after it
  mtcart.py list IMAGE                    show and check an image's games

Options: --capacity (default 4 MiB), --art NAME=FILE.cma (box art for a game),
build --preview OUT.ppm (the title card).

The installer's Cartridge tab writes carts over USB; these images are for
whole-cart writers. The format lives in installer/cartridge.py and matches
mcu/main/cart_store.h (version 2 directory; version 1 images still list).
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'installer'))
import cartridge  # noqa: E402


def games_from(paths, arts):
    art = {}
    for item in arts or []:
        name, _, file = item.partition('=')
        art[name] = Path(file).read_bytes()
    out = []
    for p in paths:
        name = cartridge.short_name(Path(p).name)
        out.append(cartridge.Game(name, Path(p).read_bytes(), art.get(Path(p).name) or art.get(name)))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('out')
    b.add_argument('games', nargs='*')
    b.add_argument('--capacity', type=lambda v: int(v, 0), default=4 << 20)
    b.add_argument('--art', action='append')
    b.add_argument('--preview', help='also write the title card as a PPM image')
    a = sub.add_parser('append')
    a.add_argument('base')
    a.add_argument('out')
    a.add_argument('games', nargs='+')
    a.add_argument('--capacity', type=lambda v: int(v, 0), default=4 << 20)
    a.add_argument('--art', action='append')
    l = sub.add_parser('list')
    l.add_argument('image')
    args = p.parse_args()

    try:
        if args.cmd == 'build':
            games = games_from(args.games, args.art)
            if args.preview:
                _, _, _, _, _, palettes, blocks = cartridge.cgb_screen(cartridge.title_card([g.name for g in games]))
                cartridge.write_ppm(cartridge.preview_pixels(blocks, palettes), args.preview)
            image = cartridge.build(args.capacity, games, dedicated=True)
            Path(args.out).write_bytes(image)
            print(f'{args.out}: {len(image)} bytes, {len(games)} games')
        elif args.cmd == 'append':
            base = Path(args.base).read_bytes()
            if cartridge.own_size(base) != len(base):
                sys.exit('base ROM size does not match its header')
            image = cartridge.build(args.capacity, games_from(args.games, args.art), own_image=base)
            Path(args.out).write_bytes(image)
            print(f'{args.out}: {len(image)} bytes; directory at 0x{len(base):x}')
        else:
            image = Path(args.image).read_bytes()
            cart = cartridge.parse(image)
            if not cart.version:
                sys.exit('no Magic Trick directory')
            print(f'version {cart.version}, ' + ('dedicated cart' if cart.dedicated else f'own game {cart.own_title}'))
            for g in cart.games:
                print(f'  {g.name}  size={len(g.data)} crc={cartridge.crc32(g.data):08x}' + ('  +art' if g.art else ''))
            if cart.damaged:
                print(f'  {cart.damaged} damaged entries')
            sys.exit(1 if cart.damaged else 0)
    except cartridge.CartError as e:
        sys.exit(str(e))


if __name__ == '__main__':
    main()
