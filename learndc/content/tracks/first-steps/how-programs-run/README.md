# How a program runs

Before you write code, it helps to know what happens to it. This lecture is
short: three ideas, then a tour of the smallest useful program.

## Source code, compiler, executable

A computer cannot read the instructions you type. What you type is *source
code*: plain text that people can read and edit. A program called the
*compiler* reads that text and translates it into an *executable*, a file
the computer can run directly.

```text
source code  --compiler-->  executable  --run-->  output
(you write)                 (made for you)         (what you see)
```

The compiler is also a strict proofreader. If the text breaks a rule of the
language, it refuses to produce an executable and prints an *error message*
instead. Reading those messages is a skill you start practising in a few
lessons.

## The loop you will repeat

Every lesson in learndc works the same way:

- You edit a file, usually `src/main.dc`.
- learndc builds the project: the compiler turns your source into an
  executable. If the build fails, you get the error messages.
- If it builds, learndc runs the program and compares what it printed with
  what the lesson expects.
- You read the result, change the file, and go round again.

Small steps are the whole trick: change one thing, check, read. Nobody writes
a correct program in one go.

## The smallest program

This program prints one line.

```dc example=hello
```

Go through it line by line:

- `module main;` names the file's module. Every program in this course starts
  with it.
- `public Status(Error) main() { ... }` defines `main`, the place where the
  program starts. The braces `{ }` hold the steps it performs. The words
  before `main` say that it reports either success or an error when it ends.
- `println("Hello, world!")?;` is one *statement*: one instruction. `println`
  prints the text between the quotes and then starts a new line.
- `return Status::Ok;` ends `main` and reports that everything went fine.
- Every statement ends with a semicolon `;`.

About the `?`: printing can fail, for example when the output goes somewhere
that is closed. The `?` means: if it failed, stop here and report the
problem; if it worked, carry on.

## One statement after another

A program runs its statements from top to bottom. Nothing is skipped and
nothing runs twice unless you ask for it.

```dc example=top_to_bottom
```

Swapping two lines swaps the output. When a program does something
unexpected, reading it in this order is the first thing to try.

## Text with holes

Text inside double quotes is a *string*. A `{}` inside the quotes is a hole
called a *placeholder*. `println` fills each hole with the next value after
the string, separated by commas:

```dc example=greeting
```

The next lessons practise this until it is routine.

## Common mistakes

- Forgetting the `;` at the end of a statement.
- Forgetting the closing double quote of a string.
- Writing `Println` or `printLn`: names are case sensitive, so only
  `println` works.
- Deleting the `?` or the `return Status::Ok;` line while experimenting.
  Leave them in place until a lesson explains how to change them.

Next, you change a real program and watch it run.
