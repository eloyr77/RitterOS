# poweroff: switch the machine off

```
poweroff
```

Switches the machine off. Only root may do it. First the elf `logd` is
asked to write what it still keeps into `/system/logs` (`poweroff` waits
up to 3 seconds for it), then the machine goes off: on a PC through ACPI
(sleep state S5), on ARM through PSCI. Nothing else is asked or printed.
Arguments are ignored. `poweroff` has been a program since step 11d; the
rescue console has a `poweroff` command of its own for when the system
cannot start.

## Exit status and messages

`poweroff` only comes back when the machine could not be switched off:

- `1`: `poweroff: the machine could not be switched off: REASON`.
  `Operation not permitted` means the user is not root; `I/O error` means
  the firmware did not switch the machine off.

## Examples

```
ritteros:/$ poweroff
```

```
ritteros:/$ su anna
ritteros:/$ poweroff
poweroff: the machine could not be switched off: Operation not permitted
```

## See also

[`reboot`](reboot.md), [`logd`](logd.md), [`sudo`](sudo.md)
