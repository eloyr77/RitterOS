# reboot: restart the machine

```
reboot
```

Restarts the machine. Only root may do it. First the elf `logd` is asked
to write what it still keeps into `/system/logs` (`reboot` waits up to 3
seconds for it), then the machine restarts: on a PC through the reset
register the ACPI tables name, on ARM through PSCI. Nothing else is asked
or printed. Arguments are ignored. `reboot` has been a program since step
11d; the rescue console has a `reboot` command of its own for when the
system cannot start.

## Exit status and messages

`reboot` only comes back when the machine could not be restarted:

- `1`: `reboot: the machine could not be restarted: REASON`.
  `Operation not permitted` means the user is not root; `I/O error` means
  the firmware did not restart the machine.

## Examples

```
ritteros:/$ reboot
```

```
ritteros:/$ su anna
ritteros:/$ reboot
reboot: the machine could not be restarted: Operation not permitted
```

## See also

[`poweroff`](poweroff.md), [`logd`](logd.md), [`sudo`](sudo.md)
