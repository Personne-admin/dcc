# Refactor: split the invoice out of main

Refactoring changes structure and keeps behaviour. The run check proves the second part: it passes before you start and must still pass at the end. The harness proves the first: the new module exists with the requested interface and handles cases the program never prints (quantity 0, 9 and 10, an empty invoice). The duplicated block became one function, the numbers written into the code became data, and the discount rule now lives in exactly one place, so changing it is a one-line edit.
