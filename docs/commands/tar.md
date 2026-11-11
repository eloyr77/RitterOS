# tar: make (c), take apart (x) or list (t) archives of files and folders

```
tar c|x|t [vmpo] [-f ARCHIVE] [-C DIR] [FILE...]
tar -c|-x|-t [-vmpo] [-f ARCHIVE] [-C DIR] [FILE...]
```

`tar c` puts the FILEs (folders with everything in them) into one
archive, `tar x` takes an archive apart (all of it, or only the FILEs
named and what is inside them), `tar t` lists it. The archive is ARCHIVE,
or without `-f` (or with `-f -`) the standard input or output, so a
folder goes from one machine to another in one ssh call:

```
tar cf - myproject | ssh root@ADDRESS 'tar xf - -C /home/root'     # there
ssh root@ADDRESS 'tar cf - -C /home/root myproject' | tar xf -     # and back
```

The format is POSIX's ustar, with pax headers for names longer than it
holds: what the tar of macOS, Linux and the BSDs make and read (also their
pax headers and GNU's long names). On a Mac set `COPYFILE_DISABLE=1` before
`tar c`, or the archive carries `._NAME` files of Apple's metadata. Kept
are files, folders, symbolic links, their permission bits (setuid only
when root takes them apart), owners and times; hard links come out as
files of their own. Taken apart, files belong to who takes them apart and
get their times from the archive (since step 63m; folders' at the end, as
filling them changed them). A name loses its leading `/`; an entry with a
`..` part, or one on the way through a symbolic link, is left out, so an
archive never writes outside the folder it is taken apart in. A file in
the way is replaced. There is no compression yet (`-z`). `tar` has been in
RitterOS since step 63h.

## Options

| Option | Meaning |
|---|---|
| `c` | make an archive of the FILEs (at least one is needed) |
| `x` | take the archive apart: everything, or the FILEs named |
| `t` | list the archive: everything, or the FILEs named |
| `v` | name each file (on the error output when the archive goes to the standard output); with `t`, `ls -l`-like lines: permissions, owner/group, size, date and time (UTC), the name |
| `m` | with `x`: files and folders get the time of the moment, not the archive's |
| `f ARCHIVE` | the archive file; `-` is the standard input (`x`, `t`) or output (`c`) |
| `C DIR` | work in DIR: `c` takes the FILEs from there, `x` puts them there (a relative ARCHIVE is still from the current folder) |
| `p`, `o` | accepted for other tars' scripts; they change nothing (permissions are always kept, files always belong to who takes them apart) |
| `--` | the end of the options: what follows are FILEs |

The first word may hold the letters without a dash (`tar cvf A.tar
dir`); then `f` takes the next word. With a dash, letters may be joined
too (`-cvf A.tar`, `-xf A.tar`), and `f` or `C` takes the rest of its word
(`-fA.tar`, `-C/tmp`) or the next one. Exactly one of `c`, `x`, `t` is
needed.

## Exit status and messages

- `0`: everything was done.
- `1`:
  - `tar: ARCHIVE: REASON`, `tar: DIR: REASON` (the archive or DIR could
    not be opened)
  - `tar: NAME: REASON` for a file that could not be read or made; the
    others are still done
  - `tar: NAME: a path that would leave this folder: left out`
  - `tar: NAME: it would go through a symbolic link: left out`
  - `tar: NAME: not a file, folder or link (a device?): left out`
  - `tar: NAME: the file it is a link to is not there` (a hard link)
  - `tar: NAME: the path is too long` (`c`: over 4,096 bytes)
  - `tar: not a tar archive (or a damaged one): a header's checksum is
    wrong`, `tar: the archive ends inside NAME`, `tar: an extended header of N
    bytes: too long`, `tar: cannot write the
    archive: REASON`: tar stops.
- `2`: `tar: c needs the files to put in`, or wrong options: `usage: tar
  c|x|t [vm] [-f ARCHIVE] [-C DIR] [FILE...]`.

## Examples

```
ritteros:/$ tar cf /home/root/project.tar -C /home/root project
ritteros:/$ tar tvf /home/root/project.tar
drwxr-xr-x root/root            0 2026-10-27 09:12 project/
-rw-r--r-- root/root          214 2026-10-27 09:12 project/main.c
-rw-r--r-- root/root           96 2026-10-27 09:10 project/Makefile
ritteros:/$ tar xf /home/root/project.tar -C /tmp
ritteros:/$ tar xvf /home/root/project.tar project/main.c
project/main.c
```

## See also

[`cp`](cp.md), [`ssh`](ssh.md), [`ar`](ar.md), [`ls`](ls.md)
