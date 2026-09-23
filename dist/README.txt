Prince of Persia 2 - The Shadow and the Flame
Native Windows port  (v1.0)
=============================================

This package is only the port: the Windows program that runs the game.  It
contains no game data, artwork or music - you need your own copy of the
original DOS Prince of Persia 2, the folder that holds PRINCE.EXE and the
.DAT files.  The launcher reads that folder and builds your setup from it.

Nothing is emulated: the original DOS code was recompiled to a native Windows
executable, so it runs at full speed with no DOSBox and no configuration.


Installing
----------
1. Unzip "PoP2 Launcher.exe" and "pop2.exe" into the same folder, anywhere.
2. Run "PoP2 Launcher.exe".
3. Game folder: browse to your Prince of Persia 2 folder (the one with
   PRINCE.EXE in it).  The launcher will not start without it.
4. Pick fullscreen or a window size, the display filter and the view.
5. Press PLAY.  "Desktop shortcut" puts an icon on your desktop.

Settings are kept in pop2.ini next to the launcher.  Windows 10 or 11,
64-bit; no runtime or redistributable to install.


Controls
--------
  Arrow keys      run, jump (up), crouch (down)        pad: D-pad / stick
  Up              jump, climb, block                   pad: A / Cross
  Shift           grab a ledge, pick up, drink         pad: B / Circle
  Ctrl            sword strike                         pad: X / Square
  Down            crouch, put the sword away           pad: Y / Triangle
  Space           skip the intro, show the time left   pad: Back / Create
  Esc             pause                                pad: Start / Options
  Alt+Enter, F11  fullscreen on and off
  F7, F8          cycle the display filter / the view
  Alt+G, Alt+L    save, load  (the game's own screens)

Xbox pads work through XInput; PlayStation, Switch and generic USB pads
through the Windows joystick interface.


Music and sound
---------------
The port emulates a Sound Blaster Pro: the game's own FM driver runs inside it,
driving an emulated OPL2 chip, and the sound effects play as digital samples.

It does this whatever your old DOS SETUP chose.  The game decides what music
data to send from its CONFIG.DAT and talks to whichever MIDI.DRV and DIGI.DRV
are in the folder, so a copy installed for General MIDI, Roland or the PC
speaker would send the driver data it cannot play and the music would come out
wrong.  The port quietly serves its own copies of those files instead; nothing
in your game folder is modified.  Put Sound=keep in pop2.ini if you would
rather it used your own setup as it stands.


Display
-------
  Filters   Sharp (crisp, evenly sized pixels at any window size), Pixel,
            Soft, Smooth HQ (edge smoothing)
  Views     4:3 as the original, Wide (stretch), Wide panorama (the centre
            stays 4:3 and the stretch grows toward the sides)


Gameplay
--------
Everything is the original game, including the 75 minute limit: the clock
starts when you reach level 5, Space shows the time left, and the game
announces it every five minutes and every minute near the end.

"Extra checkpoints" in the launcher is the one addition, and it is off by
default.  With it on, the game remembers each room you reach and a key press
after dying puts you back there instead of at the start of the level.


Support
-------
This port is free and stays that way.  If it brought the game back for you and
you would like to say thanks, there is a "Buy me a coffee" link in the launcher,
or go to https://buymeacoffee.com/mohmmadpodt


Source: https://github.com/Supermedo/prince-of-persia-2-windows