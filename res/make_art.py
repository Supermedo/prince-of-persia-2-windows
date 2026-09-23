"""Build the icons and the launcher banner from your own copy of the game.

  python res/make_art.py [game_dir] [title_screen.bmp]

  pop2.ico / launcher.ico   copied from the game's own prince.ico
  banner.bmp                optional 320x200 title-screen grab, shown in the launcher
                            (POP2_SHOTS=<dir> pop2.exe writes one BMP per second)

Without these files the build still works; the executables just have no icon and the
launcher shows a plain header.  They are not in the repository because they are the
game's artwork.
"""
import os
import shutil
import sys

here = os.path.dirname(os.path.abspath(__file__))
game = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('POP2_GAMEDIR', 'D:/Dos/prince 2.pc')

src = None
for name in ('prince.ico', 'PRINCE.ICO'):
    if os.path.exists(os.path.join(game, name)):
        src = os.path.join(game, name)
        break
if not src:
    sys.exit('no prince.ico in %s - pass the game directory as the first argument' % game)
for dst in ('pop2.ico', 'launcher.ico'):
    shutil.copyfile(src, os.path.join(here, dst))
    print('wrote res/%s' % dst)

if len(sys.argv) > 2:
    shutil.copyfile(sys.argv[2], os.path.join(here, 'banner.bmp'))
    print('wrote res/banner.bmp')
