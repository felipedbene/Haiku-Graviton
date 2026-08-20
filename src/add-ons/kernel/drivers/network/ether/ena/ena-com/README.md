# ena-com

Amazon's ENA host abstraction layer, copied here **verbatim** from
[amzn-drivers](https://github.com/amzn/amzn-drivers) so that it can be updated
by re-copying rather than by merging. BSD-3-Clause; see the SPDX headers.

The single exception is `ena_plat.h`, which is ours: it is the platform contract
the HAL reaches the operating system through, and the whole port consists of
implementing it. Do not "fix" anything else in this directory — if the HAL will
not build, the missing piece belongs in `ena_plat.h` or in `../ena_plat.cpp`.

## Re-importing

From the repository root, with `amzn-drivers` checked out beside `haiku/`
(`git clone --depth=1 https://github.com/amzn/amzn-drivers.git`):

```sh
D=haiku/src/add-ons/kernel/drivers/network/ether/ena/ena-com
S=amzn-drivers/kernel/fbsd/ena/ena-com
cp $S/ena_com.c $S/ena_com.h $S/ena_eth_com.c $S/ena_eth_com.h $D/
cp $S/ena_defs/*.h $D/ena_defs/
# deliberately NOT $S/ena_plat.h -- ours replaces it
```

Use the **FreeBSD** copy under `kernel/fbsd/`, not `kernel/linux/` and not
`userspace/dpdk/`. Two reasons: it is the variant whose `ena_plat.h` our own is
modelled on, and it is self-consistent. The nominally equivalent files in
FreeBSD's own src tree (`sys/contrib/ena-com`) are *not* usable — that snapshot
references macros such as `ENA_DMA_ADDR_TO_UINT32_HIGH` and
`ENA_COM_BOUNCE_BUFFER_CNTRL_CNT` that are defined in files it does not ship,
and it has no `ena_defs/ena_includes.h`, so it cannot be compiled as fetched.

After re-importing, re-check `ena_plat.h` against the vendor's: a newer HAL may
require platform primitives we do not yet provide. The build will tell you, but
the failure can be a link error rather than a compile error (`mb`/`wmb` arrived
that way).
