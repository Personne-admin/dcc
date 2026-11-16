# Project: Bank Ledger

Needs: the memory chapter (owning `Array`, `defer`, pointers to arrays) and
the errors chapter (`Status`, `Optional`, custom error types).

A tiny bank keeps two growing lists: the *accounts* and a *journal* that
records every change of money. Everything is counted in cents so that
amounts are exact integers (`i64`): 12050 means 120.50.

```dc
public struct Account { []const char name; i64 balance; }
public struct Entry { usize account; i64 delta; []const char note; }
```

`Entry.account` is an index into the accounts array. `main` owns both arrays
and lends them to the functions in `src/ledger.dc` as `Array(Account)*` and
`Array(Entry)*`. `src/money.dc` prints cents as `12.50` and is finished.

The bank has one rule: *a refused operation changes nothing*. Not a balance,
not the journal. Every function therefore validates first and changes state
last, and refuses with a `LedgerError`:

```dc
public enum LedgerError {
    @implicit_construction Alloc(AllocError),   // push can run out of memory
    UnknownAccount, DuplicateAccount, InvalidAmount,
    InsufficientFunds, SameAccount,
}
```

Because of `@implicit_construction`, `accounts.push(...)?` inside a
`Status(LedgerError)` function turns an allocation failure into
`LedgerError::Alloc` automatically.

## Stage 1: Read the old ledger

A question about the previous bookkeeper's transfer. Read it before writing yours.

## Stage 2: Open accounts

- `find(accounts, name)` returns `Optional::Some(index)` or `Optional::None`.
- `open(accounts, name)` adds an account with balance 0, or refuses with
  `DuplicateAccount`.
- `balance_of(accounts, name)` returns `Optional(i64)`.

## Stage 3: Deposits and withdrawals

`deposit` and `withdraw` (arguments: accounts, journal, name, amount) check in
this order and stop at the first problem:

1. `InvalidAmount` when `amount <= 0`
2. `UnknownAccount`
3. `InsufficientFunds` (withdraw only: the balance must cover the amount)

On success they push an `Entry` (`delta` is `+amount` for a deposit, `-amount`
with the note `"withdrawal"` for a withdrawal, note `"deposit"` otherwise)
and change the balance.

## Stage 4: Transfers that never lose money

`transfer(accounts, journal, from, to, amount)` checks in this order:
`InvalidAmount`, `UnknownAccount` (either name), `SameAccount`,
`InsufficientFunds`. On success it journals two entries, first the debit
(`"transfer out"`, negative delta on `from`), then the credit (`"transfer in"`),
and moves the money.

## Stage 5: Print a statement

`statement(accounts, journal, name)` replays the journal for one account:

```text
statement for ann
  deposit       +120.50  balance 120.50
  transfer out  -20.00  balance 100.50
closing balance 100.45
```

An unknown account prints `statement for zed: no such account`. Entries of
other accounts are skipped; the closing balance is the stored balance.

## Stage 6: A day at the bank

`main` in `src/main.dc` owns the arrays and already has the list of
transactions and an `apply` function that calls the right operation. Print one
line per transaction, numbered from 1, then the statements of `ann` and `bob`:

```text
1 open ann: ok
3 open ann: refused, account exists
6 withdraw bob 80.00: refused, insufficient funds
7 transfer ann bob 30.00: ok
```

The refusal texts are: `unknown account`, `account exists`, `invalid amount`,
`insufficient funds`, `same account`; an allocation failure prints
`out of memory`. The whole expected output is in the check on failure.
