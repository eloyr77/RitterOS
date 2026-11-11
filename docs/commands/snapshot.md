# snapshot: list, take or delete file system snapshots (/.snapshots/NAME)

```
snapshot [create NAME | delete NAME]
snapshot
snapshot create NAME
snapshot delete NAME
```

A snapshot keeps the state of the whole system disk at one moment. Without
arguments `snapshot` lists the snapshots with the generation (the number of
the commit) each one shows. `create` keeps the current state of everything
under NAME (1-63 characters, no `/`): it takes no time and no space at
first, since RitterFS is copy-on-write and only what changes later is
stored twice. `delete` removes a snapshot and reports how many blocks
became free again. Everyone may list them; only root may take or delete
one, as they cover everyone's files. `snapshot` has been in RitterOS since
step 18b.

A snapshot is readable at `/.snapshots/NAME/...`, read only: `ls`, `cat`,
`cd` and `cp` work there, even running an old program. `versions` shows a
file in every snapshot. Snapshots are of the system disk only, so far (not
of other volumes, and not on FAT or exFAT).

## Options

| Option | Meaning |
|---|---|
| `create NAME` | keep the current state as the snapshot NAME |
| `delete NAME` | remove the snapshot NAME; its blocks become free once nothing else uses them |

## Exit status and messages

- `0`: done. The list is `NAME generation N` per snapshot, or `no
  snapshots`; `create` prints `snapshot 'NAME' taken at generation N`,
  `delete` prints `snapshot 'NAME' deleted, N block(s) free again`.
- `1`: `snapshot: NAME: REASON`: `Operation not permitted` (not root),
  `File exists` (a snapshot of that name is there), `No such file or
  directory` (delete of one that is not there), `Invalid argument` (a name
  that is empty, too long or has a `/`), `Read-only file system`. A list
  that cannot be read: `snapshot: REASON`.
- `2`: anything else: `usage: snapshot [create NAME | delete NAME]`.

## Examples

```
ritteros:/$ snapshot
no snapshots
ritteros:/$ snapshot create monday
snapshot 'monday' taken at generation 12
ritteros:/$ snapshot
monday                   generation 12
ritteros:/$ ls /.snapshots/monday/home
ritteros:/$ cp /.snapshots/monday/home/root/notes.txt /home/root/notes-monday.txt
ritteros:/$ snapshot delete monday
snapshot 'monday' deleted, 3 block(s) free again
```

## See also

[`versions`](versions.md), [`attr`](attr.md), [`cp`](cp.md),
[`ls`](ls.md)
