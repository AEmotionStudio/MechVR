MechVR for Windows
==================

A VR mech game where every shape, effect and sound is made by code.
Project page, controls and source: https://github.com/AEmotionStudio/MechVR

THE WINDOWS VERSION IS UNTESTED ON REAL WINDOWS. It is built from the same
code as the Linux version, which is played and tested. In a window it has run
correctly under Wine, but nobody has played it on a Windows PC or in a headset
there yet. If something goes wrong, please open an issue on the project page
with the text the game printed in its console window.


Playing
-------

  play-in-window.bat   Play in a window with mouse and keyboard. No headset needed.
  play-vr.bat          Play in the headset. Start SteamVR first.

If the game stops with an error, the window stays open so you can read it.
You can also run mechvr.exe from a command prompt; see the project page for
all of its options (mechvr.exe --flat --foot, --build 1 2 3, ...).


What you need
-------------

  * A graphics card with OpenGL 4.5 (most cards from the last ten years) and
    an up-to-date driver from NVIDIA, AMD or Intel.
  * For the headset: an OpenXR runtime that supports OpenGL, and
    openxr_loader.dll in this folder. SteamVR works: in SteamVR's settings,
    under OpenXR, set it as the current OpenXR runtime if another one is.
  * The controls are made for Oculus Touch-style controllers (Rift, Quest).


Window controls
---------------

  W S           throttle / walk          A D      turn / strafe
  Mouse         aim                      Right mouse button + drag: look around
  Left button   fire plasma / pistol     Space    fire autocannon
  X             eject                    E        board a mech, graft an arm
  1 2 3         switch look              F        free camera
  R             restart                  Esc      quit


Licenses
--------

MechVR is under the MIT License (LICENSE.txt). openxr_loader.dll, when
included, is the Khronos OpenXR loader, under the Apache License 2.0
(openxr_loader-LICENSE.txt).
