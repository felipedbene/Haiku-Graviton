# ssh/ — OpenSSH for Haiku/arm64 on Graviton

SSH is the primary management interface for these instances: EC2 Nitro has no
framebuffer, so the GUI is unreachable and the serial console is the only other
way in.

Full write-up, including the measured findings and the validation transcript:
[`../docs/ssh-access.md`](../docs/ssh-access.md).

| File | What |
|---|---|
| `build-openssh-arm64.sh` | Cross-builds OpenSSH 10.4p1 for `aarch64-unknown-haiku` and wraps it in an hpkg. Never runs `jam`, so it is safe to run while someone else builds an image. |
| `UserBuildConfig` | Copy to `generated.arm64/UserBuildConfig`. The build system's own extension point; puts the hpkg, the configs, the launch job, the authorized key and the `sshd` privsep account into the image. **No Haiku source file is patched.** |
| `files/sshd_config` | Wildcard bind, key-only auth, no DNS, Ed25519 host key. |
| `files/ssh_config` | (generated at build time from upstream) |
| `files/launch-sshd` | `launch_daemon` service definition → `/boot/system/settings/launch/sshd`. |
| `files/sshd_boot.sh` | First-boot host key generation, then `exec sshd -D`. |
| `files/services` | Replacement `network/services` with the `net_server` ssh entry commented out, so only one thing owns port 22. |
| `files/authorized_keys` | The baked-in **test** Ed25519 public key. Replace for real use. |

## Quick start

```sh
rsync -az ssh/ ubuntu@<builder>:/tmp/hg-ssh-src/
ssh ubuntu@<builder> bash /tmp/hg-ssh-src/build-openssh-arm64.sh
ssh ubuntu@<builder> cp /tmp/hg-ssh-src/UserBuildConfig \
    /opt/haiku/haiku/generated.arm64/UserBuildConfig
# then, with no other jam running:
ssh ubuntu@<builder> 'cd /opt/haiku/haiku/generated.arm64 && \
    HAIKU_REVISION=hrev59996 jam -q -j64 @minimum-mmc'
# boot headless with tcp/2222 forwarded to the guest's tcp/22:
ssh ubuntu@<builder> '~/haiku-graviton/scripts/boot-qemu.sh ssh'
ssh -p 2222 -i ~/.ssh/haiku-graviton-ed25519 baron@127.0.0.1
```

## Three things that will bite you

1. **The account is `baron`, not `user`,** and it is uid 0. `@minimum` builds
   leave `HAIKU_ROOT_USER_NAME` unset, so `common-tail` falls back to `baron`.
2. **Only Ed25519 keys work.** There is no OpenSSL for Haiku/arm64, so OpenSSH is
   built `--without-openssl`: no RSA, no ECDSA. The existing EC2 key pair
   (`haiku-graviton.pem`, RSA-2048) is useless against this sshd.
3. **virtio-net needs `vectors=0`.** With MSI-X the Haiku guest transmits but
   never receives, so DHCP never completes and sshd is unreachable even though it
   is listening. Forcing INTx fixes it. This is a likely hazard for ENA too.
