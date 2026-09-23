Prince of Persia 2 - The Shadow and the Flame
Native Windows port  (v1.0.2)
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


If your antivirus complains
---------------------------
pop2.exe is a big unsigned program full of machine-translated code, which is
the shape Defender's machine-learning guesses at: some machines report
"Trojan:Win32/Wacatac" or another "!ml" verdict on a fresh download, while a
scan with current signatures finds nothing.  It is a false positive.  You can
check the SHA-256 published with the release, report the file at
https://www.microsoft.com/en-us/wdsi/filesubmission so the verdict is
corrected, or build the program yourself from the source repository.


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


Music
-----
The soundtrack is the game's own FM music: the Sound Blaster Pro driver that
ships with the game runs inside the port, driving an emulated OPL2 chip.  This
happens whatever your old SETUP chose, so the music sounds right even if the
game was installed for General MIDI, Roland or the PC speaker.

If you would rather hear your Windows General MIDI synth, set Music to
"General MIDI" in the launcher (Music=gm in pop2.ini).


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