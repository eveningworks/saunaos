# login

**a `/bin` program.**

**Category:** System information

## Synopsis

    login [--seat] [-f ACCOUNT]

## Options

- `--seat` -- the shell holds the seat: this terminal is AT the machine.
- `-f ACCOUNT` -- sign ACCOUNT in without asking, when it has no password.
- `-h`, `--help` -- the usage.

## Description

Ask for an account and its password on this terminal, then start that
account's shell. Linux's `login(1)` in the places `getty` and `telnetd`
start it, and like it **run as root**: init runs `login --seat` on the
console and the virtual terminals while no session is on (the login
screen is up), and `telnetd` runs `login -f` with the first person's
account for each connection (`telnetd.md`).

It checks nothing itself: `privd` checks the password (`privd.md`), and
the shell starts with the account's uid, groups, home and login
variables, in its own process group in front of the terminal.

`-f ACCOUNT` signs that account in at once **only while it has no
password** -- a machine nobody set one on keeps its shells without a
prompt. Once the account has one, `-f` asks like a plain `login`.

A wrong password, an unknown account and a refused one all answer
`Login incorrect`, so the terminal does not say which accounts exist;
after a failure `privd` makes the next try wait (`Wait a moment, then
try again.`).

    saunaos login: user
    Password:
    $ id
    uid=1000(user) gid=1000(user) groups=1000(user),...

Run as anyone but root, it says `login: run as root` and exits 1.
