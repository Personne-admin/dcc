# @@name@@

An x86-64 kernel in DC, booted by [Limine](https://github.com/Limine-Bootloader/Limine), generated from the `limine-x86-64` dpm template.

## Building and running

    dpm task iso        build the kernel and a bootable ISO (BIOS and UEFI)
    dpm run             boot the ISO in QEMU (BIOS); arguments after -- go to QEMU
    dpm task run-uefi   boot the ISO in QEMU with OVMF firmware
    dpm task hdd        build a 64 MiB raw disk image
    dpm task run-hdd    boot the disk image (run-hdd-uefi for UEFI)
    dpm build           build only the kernel
    dpm task            list every task

The kernel is built by dpm from `src/main.dc` with the linker script in `linker-scripts/`. The Limine release and the OVMF firmware are assets: dpm downloads them once, checks them against the hashes in `project.lock.dcon` and keeps them in its cache, so `dpm fetch` followed by `--offline` works without the network. Images and the Limine host tool are written under `.dpm/tasks/`; `dpm clean` removes them.

## Host tools

`cc` builds the Limine host utility, `xorriso` builds the ISO, `sgdisk` and `mtools` (`mformat`, `mmd`, `mcopy`) build the disk image, and `qemu-system-x86_64` runs either. Each task lists the tools it needs and dpm reports a missing one before it starts.

Tasks run the commands written in `project.dcon` directly, without a shell; the two image scripts in `scripts/` are run with `sh`. Review them before running a task.
