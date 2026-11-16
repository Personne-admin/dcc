## Hint 1

Work in order and run the checks after each stage. Every function follows the
same shape: convert DC data to what C expects, make one C call, then turn
everything C reports (a return value, an end pointer, errno, null) into a
typed DC result. Keep the C calls inside `native.dc`.

## Hint 2

Three rules recur. Clear `errno` before a call you will check and read it
immediately afterwards. Test pointers returned by C for null before you use
them. Release what you opened with `defer` right after the open succeeded.
The report stage only combines the earlier stages, so a stage that fails
there points at the function that is still wrong.

## Hint 3

In `parse_int`: `Result(const char*, CTextError) converted = to_c(text, buffer[0..64]);`,
then `*__errno_location() = 0; i64 value = strtol(start, &end, 10);`,
`i32 code = *__errno_location();`, and the checks in this order: `end` equals
`start`, `code == ERANGE`, `*end != '\0'`. In `read_file`:
`void* file = fopen(path, "rb\0".ptr); if file == null {...} defer fclose(file);`.
