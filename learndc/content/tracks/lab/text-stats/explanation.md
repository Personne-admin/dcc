# Why these functions work

Every function here walks the text once and remembers just enough about
what it has seen. `count_words` remembers one bool, whether the previous
character was a letter, and counts the moments that bool turns from false
to true. Counting transitions instead of letters is why extra separators
and a word at the end both come out right.

`longest_word` needs the start of the current word as well. It runs the
index one position past the end so that the end of the text acts like a
separator and closes a final word without special code. It returns
`text[start..i]`, a slice that points into the original text: nothing is
copied, and the strict `>` comparison keeps the first of equally long
words.

`count_letters` uses a character as an array index. A character is a
number, `lower(c) as u32 - 97` maps `a` to 0 and `z` to 25, and the array of
26 counters is a histogram. `most_common` is the same search for a maximum
you wrote for plain numbers; the strict `>` again keeps the earlier letter
on a tie.

`main` only connects the pieces and formats them. Because each piece was
tested alone, a wrong report line points at the formatting, not at the
counting.
