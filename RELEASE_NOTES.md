P mode no longer returns the board to level 1 on its own while it runs; the
level you set stays until you change it or switch P mode off.

P mode, from v0.13.0: a new item in the board's settings menu. While it is
on, a bold P shows at the top of the screen and the board presses one of
the four buttons on its own now and then, driven by the chip's hardware
random number generator. Four coloured blocks above the button names show
how close each button is, green far to red about to press. P mode is off
at every power-on and never remembered; the board does not sleep while it
runs. Its rate and threshold are on the settings page under P mode.
