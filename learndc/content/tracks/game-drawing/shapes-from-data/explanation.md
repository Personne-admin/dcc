# Why it works

`draw_shapes` separates *what to draw* (the data) from *how to draw it* (the SDL calls). A new shape is one more struct in the list, not another block of code. The flag `filled` chooses between two draw calls, and iterating in list order makes the data order the painter's order, which is why the second shape in the check shows through the third.

`for shape in shapes` gives each element by value; `&shape.rect` points at the rectangle inside that element, which is all SDL needs, since it only reads through the pointer during the call.

`draw_bars` computes rectangles from numbers instead of storing them. Two details carry the lesson. First, the top of a bar is `baseline - value`: with y growing downwards, a taller bar has a smaller top y, and the height is added going down to the baseline. Second, integer arithmetic stays in `i32` until the last moment; `as f32` converts at the boundary where SDL wants floats. Doing the position math in integers (`i * (bar_width + gap)`) avoids rounding surprises.
