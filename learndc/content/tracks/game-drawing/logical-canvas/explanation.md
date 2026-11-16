# Why it works

Logical presentation puts a transform between your drawing calls and the window: every coordinate you pass is multiplied by a scale and shifted by an offset. That is what `letterbox` computes, and why `to_logical` is its inverse. Using the *smaller* scale guarantees the whole canvas fits; the unused direction is split into two equal bars, hence the division by two.

Your game logic can now use one fixed coordinate system. A paddle at x = 150 is at the right edge in a small window and in a full-screen one. Only code that touches the real window, like the mouse, needs the mapping: events carry *window* coordinates, so `to_logical` converts a click before the game sees it. SDL ships its own conversion; writing it yourself once shows there is no magic in it, and the check proves yours agrees.

Reading the pixels back shows the other half: the picture is made at the window's resolution, so the 1-pixel frame becomes 2 pixels wide in a window that is twice as large. Logical presentation scales the drawing; it does not make it sharper.
