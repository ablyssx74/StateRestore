# StateRestore

A small Haiku program that takes the system's packages back to an earlier state, without the boot menu.

Haiku keeps each earlier state of `/boot/system` as a folder `state_<date>_<time>` in
`/boot/system/packages/administrative/`, with the list of the packages that were active in it and the files of the
packages that an update took out. StateRestore lists those states, shows what a restore would change (packages whose
version changes, are taken out or are put back), and does it: the packages of that state are copied or moved into
`/boot/system/packages`, and the ones that are not in it are moved to a folder of their own,
`~/config/var/StateRestore/held/<date>/`. Nothing is deleted. **Undo last restore** puts them back, and the kept packages
are looked for when an earlier state needs them again.

Restart afterwards: the kernel and the system libraries come from the `haiku` package, so a restore of it starts with
the next boot.

Several earlier states can be selected and deleted with **Delete selected…**, after a confirmation that says how much space it
frees and how many other states could not be restored any more (the files of a package are kept in the folder of the state
that took it out, and an older state can need them). Deleting cannot be undone. The system running now is never touched.

The states that cannot be restored are shown too, under "Cannot be restored", dimmed and marked DAMAGED (their list of packages is
damaged, which a shutdown in the middle of a write can cause), MISSING (package files they need are gone) or SAME (nothing to restore).
Selecting one explains why.

**Delete unrestorable…** deletes all of those states, after a confirmation that counts each kind.
A state whose folder holds package files that a restorable state needs is kept, and the confirmation says how many.

The window also has **Restart…**, with a confirmation.

It only helps when the system still boots. A system that does not start at all needs the boot menu or a rescue disk.
Only the system packages are handled, not the ones in `~/config/packages`.

## Install

Install `staterestore-<version>-1-x86_64.hpkg` (64-bit Haiku) or `staterestore-<version>-1-x86_gcc2.hpkg` (32-bit Haiku) from the
releases page for the whole system. It adds StateRestore to the
Deskbar's Applications menu, and `StateRestore` to the command line.

## Build

On Haiku: `make`, and `make package` for the package. It needs `g++` and the Haiku development headers. On 32-bit Haiku it builds
with `g++-x86` (the secondary architecture's compiler), as Cricket does.

## Command line

    StateRestore --list                       the states, and how many packages differ from now
    StateRestore --plan <state>               what a restore would do
    StateRestore --restore <state> [--dry-run]
    StateRestore --undo
    StateRestore --delete <state>... [--dry-run]
    StateRestore --cleanup [--dry-run]
    StateRestore --syslog

`<state>` is a folder name such as `state_2026-10-15_17:52:34`, or only `2026-10-15_17:52:34`.

## Licence

MIT.

For testing, `STATERESTORE_ADMIN_DIR=<folder>` makes the program look at another folder of states; nothing is changed then.
